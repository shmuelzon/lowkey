#include "matter_radio.h"
#include "radio_backend.h"
#include "lock_manager.h"
#include "door_monitor.h"
#include "battery_manager.h"
#include "watchdog.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "clusters/identify.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/DefaultTimerDelegate.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/server/AppDelegate.h>
#include <app/server/Server.h>
#include <lib/support/CHIPMem.h>
#include <platform/CHIPDeviceLayer.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(matter_radio, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{
constexpr EndpointId kLockEndpointId = 1;

const struct radio_backend_cb *app_cbs;

static Clusters::DoorLock::OperationSourceEnum
map_op_source(enum lock_op_source source)
{
	switch (source) {
	case LOCK_OP_SOURCE_REMOTE:
		return Clusters::DoorLock::OperationSourceEnum::kRemote;
	case LOCK_OP_SOURCE_AUTO:
		return Clusters::DoorLock::OperationSourceEnum::kAuto;
	case LOCK_OP_SOURCE_MANUAL:
	default:
		return Clusters::DoorLock::OperationSourceEnum::kManual;
	}
}

/*
 * lock.zap advertises the Unbolting feature, which promises a controller that
 * UnlockDoor retracts the bolt AND pulls the latch, and that the lock reports
 * Unlatched when it does. With no latch travel configured the hardware cannot
 * honour either half, and an open would report Unlocked instead. Fail the build
 * rather than ship a data model that overstates what the lock can do — a
 * latchless build wants the Unbolting bit cleared from the FeatureMap too.
 */
BUILD_ASSERT(CONFIG_LOCK_LATCH_RELEASE_MS > 0,
	     "Matter builds advertise the DoorLock Unbolting feature, which "
	     "needs a non-zero CONFIG_LOCK_LATCH_RELEASE_MS to be honest");

static Clusters::DoorLock::DlLockState map_lock_state(enum lock_state state)
{
	switch (state) {
	case LOCK_STATE_LOCKED:
		return Clusters::DoorLock::DlLockState::kLocked;
	case LOCK_STATE_UNLOCKED:
		return Clusters::DoorLock::DlLockState::kUnlocked;
	/*
	 * Unlatched is a distinct cluster state, not a flavour of unlocked: the
	 * bolt is retracted AND the latch is held back, which is what a Matter
	 * UnlockDoor asks of a lock advertising the Unbolting feature. Reporting
	 * it also makes the SDK log the operation as Unlatch rather than Unlock
	 * (see DoorLockServer::SetLockState).
	 */
	case LOCK_STATE_UNLATCHED:
		return Clusters::DoorLock::DlLockState::kUnlatched;
	/*
	 * The cluster has no vocabulary for movement in progress, so every
	 * interim position collapses onto NotFullyLocked. SetLockState
	 * deliberately emits no event for it, which is why the interim reports
	 * do not turn into a stream of LockOperation events.
	 */
	case LOCK_STATE_LOCKING:
	case LOCK_STATE_UNLOCKING:
	case LOCK_STATE_JAMMED:
	case LOCK_STATE_UNKNOWN:
	default:
		return Clusters::DoorLock::DlLockState::kNotFullyLocked;
	}
}

static Clusters::DoorLock::DoorStateEnum map_door_state(enum door_state state)
{
	return (state == DOOR_STATE_CLOSED)
		       ? Clusters::DoorLock::DoorStateEnum::kDoorClosed
		       : Clusters::DoorLock::DoorStateEnum::kDoorOpen;
}

/*
 * An open commissioning window is the Matter equivalent of "discoverable", so
 * the CommissioningWindowManager owns that state rather than the UI latching it
 * on button press. One delegate covers every edge, including the spec-mandated
 * timeout, which the stack handles silently — there is no ChipDeviceEvent for
 * it. Callbacks run on the CHIP thread.
 */
class CommissioningWindowDelegate : public AppDelegate {
public:
	void OnCommissioningWindowOpened() override
	{
		LOG_INF("Matter: commissioning window opened");

		if (app_cbs && app_cbs->on_discovery_changed) {
			app_cbs->on_discovery_changed(true);
		}
	}

	void OnCommissioningWindowClosed() override
	{
		LOG_INF("Matter: commissioning window closed");

		if (app_cbs && app_cbs->on_discovery_changed) {
			app_cbs->on_discovery_changed(false);
		}
	}
};

CommissioningWindowDelegate sCommissioningWindowDelegate;

/* --- Identify cluster ----------------------------------------------- */

/*
 * Our own delegate rather than Nrf::Matter::IdentifyDelegateImplNrf, which
 * blinks an LED through Nrf::GetBoard() — the development-kit abstraction this
 * project does not use. Identify output belongs to ui.c, reached through
 * on_identify like every other indication.
 *
 * TriggerEffect stays off because IdentifyCluster::AcceptedCommands() derives
 * the command list from IsTriggerEffectEnabled(), so claiming it would
 * advertise a command lock.zap does not declare and OnTriggerEffect() does not
 * implement.
 */
class IdentifyDelegateImpl : public Clusters::IdentifyDelegate {
public:
	void OnIdentifyStart(Clusters::IdentifyCluster &) override
	{
		if (app_cbs && app_cbs->on_identify) {
			app_cbs->on_identify(true);
		}
	}

	void OnIdentifyStop(Clusters::IdentifyCluster &) override
	{
		if (app_cbs && app_cbs->on_identify) {
			app_cbs->on_identify(false);
		}
	}

	void OnTriggerEffect(Clusters::IdentifyCluster &) override {}

	bool IsTriggerEffectEnabled() const override { return false; }
};

IdentifyDelegateImpl sIdentifyDelegate;
DefaultTimerDelegate sIdentifyTimerDelegate;

Nrf::Matter::IdentifyCluster sIdentifyCluster(
	kLockEndpointId, sIdentifyDelegate, sIdentifyTimerDelegate,
	Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator);
} /* namespace */

/* --- internal helpers ----------------------------------------------- */

/*
 * Report an operation that moved the bolt or latch. Runs on the Matter thread.
 *
 * The source-carrying SetLockState() overload also emits a LockOperation event,
 * so this path is strictly for changes somebody caused; everything else has its
 * own non-eventing path below.
 *
 * fabricIdx/nodeId identify the client, and the cluster logs an error for a
 * remote operation missing either — a controller turns them into "Alice
 * unlocked the door" rather than attributing the action to nobody.
 */
static void report_lock_operation(const struct lock_state_change &change)
{
	Nullable<FabricIndex> fabricIdx;
	Nullable<NodeId> nodeId;

	if (change.originator.valid) {
		fabricIdx.SetNonNull(
			static_cast<FabricIndex>(change.originator.fabric_index));
		nodeId.SetNonNull(static_cast<NodeId>(change.originator.node_id));
	}

	if (!DoorLockServer::Instance().SetLockState(
		    kLockEndpointId, map_lock_state(change.state),
		    map_op_source(change.source),
		    DataModel::NullNullable /* userIndex */,
		    DataModel::NullNullable /* credentials */, fabricIdx,
		    nodeId)) {
		LOG_ERR("Matter: LockState update failed");
	}

	/*
	 * A stalled motor only reaches the cluster as NotFullyLocked, which
	 * tells a controller nothing is wrong. The DoorLockAlarm event does.
	 * lock_manager only reports JAMMED on an actual transition, so this
	 * fires once per jam.
	 */
	if (change.state == LOCK_STATE_JAMMED) {
		DoorLockServer::Instance().SendLockAlarmEvent(
			kLockEndpointId,
			Clusters::DoorLock::AlarmCodeEnum::kLockJammed);
	}
}

static void update_lock_state(const struct lock_state_change *change)
{
	/*
	 * Heap-copied rather than captured by value: a scheduled lambda's
	 * storage is CHIP_CONFIG_LAMBDA_EVENT_SIZE bytes at pointer alignment,
	 * and the 64-bit node ID alone exceeds the alignment the bridge accepts.
	 * The nRF door lock sample allocates its StateData for the same reason.
	 */
	auto *copy = Platform::New<struct lock_state_change>(*change);

	if (copy == nullptr) {
		LOG_ERR("Matter: no memory for lock state change");
		return;
	}

	CHIP_ERROR err = SystemLayer().ScheduleLambda([copy]() {
		report_lock_operation(*copy);
		Platform::Delete(copy);
	});

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: lock state schedule failed: %" CHIP_ERROR_FORMAT,
			err.Format());
		Platform::Delete(copy);
	}
}

/*
 * Report where the bolt is without claiming anyone put it there.
 *
 * The two-argument SetLockState() overload is documented as not generating a
 * LockOperation event. Used for the boot-time snapshot and for the latch
 * re-engaging on its own after an open — the latter matters because a single
 * UnlockDoor would otherwise produce both the Unlatch event it should and a
 * bogus Unlock event as the lock settled.
 */
static void publish_lock_state(enum lock_state state)
{
	CHIP_ERROR err = SystemLayer().ScheduleLambda([state]() {
		if (!DoorLockServer::Instance().SetLockState(
			    kLockEndpointId, map_lock_state(state))) {
			LOG_ERR("Matter: LockState publish failed");
		}
	});

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: lock state publish schedule failed: "
			"%" CHIP_ERROR_FORMAT, err.Format());
	}
}

/*
 * Report a door movement.
 *
 * DoorLockServer::SetDoorState() rather than the DoorState::Set() accessor:
 * the accessor only writes the attribute, while this also emits the
 * DoorStateChange event that the DoorPositionSensor feature — advertised in
 * our FeatureMap — is expected to produce.
 */
static void update_door_state(enum door_state state)
{
	CHIP_ERROR err = SystemLayer().ScheduleLambda([state]() {
		if (!DoorLockServer::Instance().SetDoorState(
			    kLockEndpointId, map_door_state(state))) {
			LOG_ERR("Matter: DoorState update failed");
		}
	});

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: door state schedule failed: %" CHIP_ERROR_FORMAT,
			err.Format());
	}
}

/*
 * Report where the door is without claiming it just moved there.
 *
 * The raw accessor, deliberately: unlike SetDoorState() it writes the attribute
 * without emitting a DoorStateChange event, which is what the boot-time
 * snapshot wants.
 */
static void publish_door_state(enum door_state state)
{
	CHIP_ERROR err = SystemLayer().ScheduleLambda([state]() {
		using namespace Clusters::DoorLock::Attributes;

		auto imStatus = DoorState::Set(kLockEndpointId,
					       map_door_state(state));

		if (imStatus != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Matter: DoorState publish failed: 0x%x",
				to_underlying(imStatus));
		}
	});

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: door state publish schedule failed: "
			"%" CHIP_ERROR_FORMAT, err.Format());
	}
}

static void update_battery_cluster(uint8_t soc, uint16_t voltage_mv)
{
	CHIP_ERROR err =
		SystemLayer().ScheduleLambda([soc, voltage_mv]() {
		using namespace Clusters::PowerSource::Attributes;
		using Protocols::InteractionModel::Status;

		const auto logOnFailure = [](Status status, const char *name) {
			if (status != Status::Success) {
				LOG_ERR("Matter: PowerSource %s set failed: 0x%x",
					name, to_underlying(status));
			}
		};

		/* Matter BatPercentRemaining is in half-percent (0..200). */
		uint8_t soc_clamped = soc > 100 ? 100 : soc;

		logOnFailure(BatPercentRemaining::Set(kLockEndpointId,
						      (uint8_t)(soc_clamped * 2)),
			     "BatPercentRemaining");
		logOnFailure(BatVoltage::Set(kLockEndpointId,
					     (uint32_t)voltage_mv),
			     "BatVoltage");

		Clusters::PowerSource::BatChargeLevelEnum chargeLevel;

		if (soc > 20) {
			chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kOk;
		} else if (soc > 5) {
			chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kWarning;
		} else {
			chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kCritical;
		}
		logOnFailure(BatChargeLevel::Set(kLockEndpointId, chargeLevel),
			     "BatChargeLevel");
	});

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: battery schedule failed: %" CHIP_ERROR_FORMAT,
			err.Format());
	}
}

static void update_charge_state(bool charging, bool complete)
{
	CHIP_ERROR err =
		SystemLayer().ScheduleLambda([charging, complete]() {
		using namespace Clusters::PowerSource::Attributes;
		using Protocols::InteractionModel::Status;
		using Clusters::PowerSource::BatChargeStateEnum;

		BatChargeStateEnum state;

		if (charging) {
			state = BatChargeStateEnum::kIsCharging;
		} else if (complete) {
			state = BatChargeStateEnum::kIsAtFullCharge;
		} else {
			state = BatChargeStateEnum::kIsNotCharging;
		}

		Status status = BatChargeState::Set(kLockEndpointId, state);

		if (status != Status::Success) {
			LOG_ERR("Matter: PowerSource BatChargeState set failed: "
				"0x%x", to_underlying(status));
		}
	});

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: charge-state schedule failed: %" CHIP_ERROR_FORMAT,
			err.Format());
	}
}

/* --- Watchdog ------------------------------------------------------- */

/*
 * The CHIP event loop runs on its own thread and serves every Matter command,
 * so it gets a watchdog channel of its own — the system workqueue being healthy
 * says nothing about it. Fed from a self-rearming CHIP timer, which is the only
 * way to prove the loop is still dispatching.
 */
static int chip_wdt_channel = -1;

static void feed_chip_watchdog(System::Layer *, void *)
{
	watchdog_channel_feed(chip_wdt_channel);
	SystemLayer().StartTimer(
		System::Clock::Milliseconds32(CONFIG_LOCK_WATCHDOG_FEED_MS),
		feed_chip_watchdog, nullptr);
}

/* --- Platform event handler ----------------------------------------- */

static void MatterEventHandler(const ChipDeviceEvent *event, intptr_t)
{
	switch (event->Type) {
	case DeviceEventType::kCommissioningComplete:
		/*
		 * No on_discovery_changed(false) here: the commissioning window
		 * closes when PASE is established, well before this event, and
		 * sCommissioningWindowDelegate reports it.
		 */
		LOG_INF("Matter: commissioning complete");

		/*
		 * The user chose Matter, so the onboarding session is over. This
		 * event rather than the window closing: the window also closes on
		 * timeout and at PASE, neither of which means a fabric was
		 * actually added.
		 */
		if (app_cbs && app_cbs->on_onboarded) {
			app_cbs->on_onboarded();
		}
		break;
	default:
		break;
	}
}

/* --- radio_backend_ops implementation ------------------------------- */

static void matter_on_lock_state_changed(const struct lock_state_change *change)
{
	/*
	 * Only an operation gets a LockOperation event. A change the mechanism
	 * made on its own — the latch springing back after an open, or the state
	 * pushed at startup — updates the attribute silently.
	 */
	if (change->operated) {
		update_lock_state(change);
	} else {
		publish_lock_state(change->state);
	}
}

static void matter_on_door_state_changed(enum door_state state)
{
	update_door_state(state);
}

static void matter_on_battery_changed(uint8_t soc_percent, uint16_t voltage_mv,
				      uint16_t remaining_mah)
{
	update_battery_cluster(soc_percent, voltage_mv);
}

static void matter_on_charger_changed(bool charging, bool complete)
{
	update_charge_state(charging, complete);
}

static int matter_start_discovery(void)
{
	/*
	 * Marshalled onto the CHIP thread rather than run on the caller's (the
	 * system workqueue): opening a window runs SPAKE2+ crypto, which needs
	 * the stack lock and a far deeper stack than sysworkq has.
	 */
	CHIP_ERROR err = SystemLayer().ScheduleLambda([] {
		auto &commissionMgr =
			Server::GetInstance().GetCommissioningWindowManager();

		/*
		 * The CommissioningWindowManager rejects anything below
		 * MinCommissioningTimeout() (spec 5.4.2.3: 3 minutes) with
		 * CHIP_ERROR_INVALID_ARGUMENT, so clamp up rather than fail
		 * when LOCK_DISCOVERY_TIMEOUT_S is tuned for BLE.
		 */
		uint32_t timeout_s = CONFIG_LOCK_DISCOVERY_TIMEOUT_S;
		uint32_t min_s = commissionMgr.MinCommissioningTimeout().count();

		if (timeout_s < min_s) {
			LOG_WRN("Matter: discovery timeout %u s below Matter "
				"minimum, using %u s", timeout_s, min_s);
			timeout_s = min_s;
		}

		CHIP_ERROR werr = commissionMgr.OpenBasicCommissioningWindow(
			System::Clock::Seconds16(timeout_s));

		if (werr != CHIP_NO_ERROR) {
			LOG_ERR("Matter: open commissioning window failed: "
				"%" CHIP_ERROR_FORMAT, werr.Format());
			return;
		}

		LOG_INF("Matter: commissioning window open for %u s", timeout_s);
	});

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: failed to schedule commissioning window: "
			"%" CHIP_ERROR_FORMAT, err.Format());
		return -EIO;
	}

	return 0;
}

/* No-op when no window is open; closing drives sCommissioningWindowDelegate. */
static void matter_stop_discovery(void)
{
	CHIP_ERROR err = SystemLayer().ScheduleLambda([] {
		Server::GetInstance().GetCommissioningWindowManager()
			.CloseCommissioningWindow();
	});

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: failed to schedule window close: "
			"%" CHIP_ERROR_FORMAT, err.Format());
	}
}

/*
 * ScheduleFactoryReset() erases on the CHIP thread and reboots when done, hence
 * factory_reset_reboots.
 *
 * The button path only. Matter also resets itself when the last fabric is
 * removed, and that never comes through here — deliberately, so losing a fabric
 * does not cost the user their BLE bonds. Keeping that erase off the bt/
 * settings subtree is what CHIP_FACTORY_RESET_ERASE_SETTINGS=n is for.
 */
static void matter_factory_reset(void)
{
	LOG_INF("Matter: factory reset requested");
	Server::GetInstance().ScheduleFactoryReset();
}

static int matter_init(const struct radio_backend_cb *cbs)
{
	app_cbs = cbs;

	/*
	 * Register the commissioning-window delegate through the server init
	 * params rather than SetAppDelegate() after the fact: Server::Init()
	 * installs it and may open the window immediately (uncommissioned
	 * device), and registering later would miss that first "opened" edge.
	 */
	Nrf::Matter::InitData initData;

	initData.mServerInitParams->appDelegate = &sCommissioningWindowDelegate;

	CHIP_ERROR err = Nrf::Matter::PrepareServer(initData);

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: PrepareServer failed: %" CHIP_ERROR_FORMAT,
			err.Format());
		return -EIO;
	}

	PlatformMgr().AddEventHandler(MatterEventHandler, 0);

	/*
	 * Register the Identify cluster between PrepareServer() and
	 * StartServer(): registration is not thread-safe and must land before
	 * the endpoints come up and their cluster init callbacks run.
	 */
	err = sIdentifyCluster.Init();

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: Identify cluster init failed: %" CHIP_ERROR_FORMAT,
			err.Format());
		return -EIO;
	}

	err = Nrf::Matter::StartServer();

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Matter: StartServer failed: %" CHIP_ERROR_FORMAT,
			err.Format());
		return -EIO;
	}

	/* The event loop is running from here, so its watchdog can start. */
	chip_wdt_channel = watchdog_channel_add();
	if (chip_wdt_channel >= 0) {
		SystemLayer().ScheduleLambda(
			[] { feed_chip_watchdog(nullptr, nullptr); });
	}

	return 0;
}

static int matter_run(void)
{
	while (true) {
		Nrf::DispatchNextTask();
	}

	return 0;
}

extern "C" const struct radio_backend_ops matter_radio_ops = {
	.init = matter_init,
	.run = matter_run,
	.on_lock_state_changed = matter_on_lock_state_changed,
	.on_door_state_changed = matter_on_door_state_changed,
	.on_battery_changed = matter_on_battery_changed,
	.on_charger_changed = matter_on_charger_changed,
	.start_discovery = matter_start_discovery,
	.stop_discovery = matter_stop_discovery,
	.factory_reset = matter_factory_reset,
	.factory_reset_reboots = true,
};

/* --- public helpers for zcl_callbacks ------------------------------- */

extern "C" int matter_radio_lock_request(enum lock_action action,
					 enum lock_op_source source,
					 const struct lock_originator *originator)
{
	if (app_cbs && app_cbs->lock_request) {
		return app_cbs->lock_request(action, source, originator);
	}

	return -ENOTSUP;
}

extern "C" void matter_radio_publish_initial_state(enum lock_state lock,
						   enum door_state door)
{
	publish_lock_state(lock);
	publish_door_state(door);
}
