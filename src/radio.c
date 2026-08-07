/*
 * Fans the radio_*() API out to whichever backends are enabled, so the
 * application never knows whether the lock speaks one protocol or two.
 *
 * Downward calls are a plain fan-out. Upward, two things need reconciling:
 *
 *   - Discovery and identify are OR-ed across backends. The UI setter is
 *     last-writer-wins, so one backend closing its window would otherwise
 *     switch the LED off while the other was still open.
 *
 *   - One button press opens an onboarding window on every backend and lets the
 *     user's app pick a protocol; the first to succeed closes the rest. Someone
 *     who wants both Matter and a BLE fallback presses the button twice.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include "radio.h"
#include "radio_backend.h"

#if defined(CONFIG_LOCK_RADIO_BLE)
#include "ble/ble_radio.h"
#endif
#if defined(CONFIG_LOCK_RADIO_MATTER)
#include "matter/matter_radio.h"
#endif

LOG_MODULE_REGISTER(radio, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * Matter first: it owns the shared Bluetooth stack. Its init calls bt_enable()
 * and settings_load(), which the BLE backend then borrows, so BLE has to come
 * up second. matter_init() does not return until those have run —
 * PrepareServer() only schedules the server init, but StartServer() blocks on
 * it (WaitForReadiness/InitGuard in the NCS matter_init.cpp).
 *
 * The enum and the table are written in the same order so an index is always
 * valid for both.
 */
enum backend_id {
#if defined(CONFIG_LOCK_RADIO_MATTER)
	BACKEND_MATTER,
#endif
#if defined(CONFIG_LOCK_RADIO_BLE)
	BACKEND_BLE,
#endif
	BACKEND_COUNT,
};

static const struct radio_backend_ops *const backends[BACKEND_COUNT] = {
#if defined(CONFIG_LOCK_RADIO_MATTER)
	[BACKEND_MATTER] = &matter_radio_ops,
#endif
#if defined(CONFIG_LOCK_RADIO_BLE)
	[BACKEND_BLE] = &ble_radio_ops,
#endif
};

BUILD_ASSERT(BACKEND_COUNT > 0,
	     "No radio backend enabled. Set CONFIG_LOCK_RADIO_BLE and/or "
	     "CONFIG_LOCK_RADIO_MATTER.");

/* Application callbacks, as handed to us by main(). */
static const struct radio_cb *app_cbs;

/*
 * The peripheral modules are initialised before the backends and their GPIO
 * interrupts are live immediately, so state changes can arrive before there is
 * anything to dispatch them to. Drop those rather than call into a half-built
 * stack; main() publishes the initial state once radio_init() returns.
 */
static bool radio_ready;

/* One bit per backend; the value the UI sees is the OR of all of them. */
static atomic_t discovery_flags;
static atomic_t identify_flags;

static void stop_discovery_all(void)
{
	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		if (backends[i]->stop_discovery) {
			backends[i]->stop_discovery();
		}
	}
}

/* --- upward: state the backends share ------------------------------- */

static void flag_changed(atomic_t *flags, enum backend_id id, bool active,
			 void (*sink)(bool))
{
	atomic_val_t before = active ? atomic_or(flags, BIT(id))
				     : atomic_and(flags, ~BIT(id));
	atomic_val_t after = active ? (before | BIT(id)) : (before & ~BIT(id));

	/* Only report transitions of the OR-ed value, not of one backend's bit. */
	if (sink && (!before != !after)) {
		sink(after != 0);
	}
}

static void discovery_changed(enum backend_id id, bool discoverable)
{
	flag_changed(&discovery_flags, id, discoverable,
		     app_cbs ? app_cbs->on_discovery_changed : NULL);
}

static void identify_changed(enum backend_id id, bool active)
{
	flag_changed(&identify_flags, id, active,
		     app_cbs ? app_cbs->on_identify : NULL);
}

static void on_onboarded(void)
{
	LOG_INF("Radio: onboarding complete, closing remaining windows");

	stop_discovery_all();
}

static int backend_lock_request(enum lock_action action,
				enum lock_op_source source,
				const struct lock_originator *originator)
{
	if (!app_cbs || !app_cbs->lock_request) {
		return -ENOTSUP;
	}

	return app_cbs->lock_request(action, source, originator);
}

#if defined(CONFIG_LOCK_RADIO_MATTER)
static void matter_discovery_changed(bool d) { discovery_changed(BACKEND_MATTER, d); }
static void matter_identify(bool a) { identify_changed(BACKEND_MATTER, a); }

static const struct radio_backend_cb matter_cbs = {
	.lock_request = backend_lock_request,
	.on_discovery_changed = matter_discovery_changed,
	.on_identify = matter_identify,
	.on_onboarded = on_onboarded,
};
#endif

#if defined(CONFIG_LOCK_RADIO_BLE)
static void ble_discovery_changed(bool d) { discovery_changed(BACKEND_BLE, d); }
static void ble_identify(bool a) { identify_changed(BACKEND_BLE, a); }

static const struct radio_backend_cb ble_cbs = {
	.lock_request = backend_lock_request,
	.on_discovery_changed = ble_discovery_changed,
	.on_identify = ble_identify,
	.on_onboarded = on_onboarded,
};
#endif

static const struct radio_backend_cb *const backend_cbs[BACKEND_COUNT] = {
#if defined(CONFIG_LOCK_RADIO_MATTER)
	[BACKEND_MATTER] = &matter_cbs,
#endif
#if defined(CONFIG_LOCK_RADIO_BLE)
	[BACKEND_BLE] = &ble_cbs,
#endif
};

/* --- downward: the application-facing API --------------------------- */

int radio_init(const struct radio_cb *cbs)
{
	app_cbs = cbs;

	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		int err = backends[i]->init(backend_cbs[i]);

		if (err) {
			LOG_ERR("Radio: backend %u init failed: %d",
				(unsigned int)i, err);
			return err;
		}
	}

	radio_ready = true;
	return 0;
}

/*
 * Written out rather than looped, because the order is a constraint: every
 * backend but the last must return promptly, and Matter's does not return at
 * all — it parks the main thread on the NCS application task queue, the CHIP
 * event loop having its own thread. BLE's returns immediately, its work
 * happening on the Bluetooth RX thread and the system workqueue.
 */
int radio_run(void)
{
#if defined(CONFIG_LOCK_RADIO_BLE)
	int err = ble_radio_ops.run();

	if (err) {
		return err;
	}
#endif

#if defined(CONFIG_LOCK_RADIO_MATTER)
	return matter_radio_ops.run();
#else
	return 0;
#endif
}

void radio_on_lock_state_changed(const struct lock_state_change *change)
{
	if (!radio_ready) {
		return;
	}

	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		backends[i]->on_lock_state_changed(change);
	}
}

void radio_on_door_state_changed(enum door_state state)
{
	if (!radio_ready) {
		return;
	}

	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		backends[i]->on_door_state_changed(state);
	}
}

void radio_on_battery_changed(uint8_t soc_percent, uint16_t voltage_mv,
			      uint16_t remaining_mah)
{
	if (!radio_ready) {
		return;
	}

	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		backends[i]->on_battery_changed(soc_percent, voltage_mv,
						remaining_mah);
	}
}

void radio_on_charger_changed(bool charging, bool complete)
{
	if (!radio_ready) {
		return;
	}

	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		if (backends[i]->on_charger_changed) {
			backends[i]->on_charger_changed(charging, complete);
		}
	}
}

/*
 * Open every backend's window so the user's app can pick a protocol. Errors are
 * reported but do not stop the remaining backends: one protocol failing to
 * become discoverable is no reason to deny the other.
 */
int radio_start_discovery(void)
{
	int first_err = 0;

	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		int err = backends[i]->start_discovery();

		if (err) {
			LOG_WRN("Radio: backend %u start_discovery failed: %d",
				(unsigned int)i, err);
			if (!first_err) {
				first_err = err;
			}
		}
	}

	return first_err;
}

/*
 * Erase everything, then reboot exactly once.
 *
 * Backends that erase synchronously go first so their writes land before
 * anything reboots; one that reboots itself goes last. Only the factory-reset
 * button reaches this — a Matter fabric removal stays inside its own backend so
 * it cannot take the other protocol's pairings with it.
 */
static void reset_reboot_expiry(struct k_timer *timer)
{
	sys_reboot(SYS_REBOOT_WARM);
}

static K_TIMER_DEFINE(reset_reboot_timer, reset_reboot_expiry, NULL);

void radio_factory_reset(void)
{
	bool rebooting = false;

	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		if (!backends[i]->factory_reset_reboots) {
			backends[i]->factory_reset();
		}
	}

	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		if (backends[i]->factory_reset_reboots) {
			backends[i]->factory_reset();
			rebooting = true;
		}
	}

	if (!rebooting) {
		sys_reboot(SYS_REBOOT_WARM);
	}

	/*
	 * Matter's reset only schedules the erase-and-reboot onto the CHIP
	 * thread. If that thread is wedged it never happens, and the lock would
	 * sit with its BLE bonds already gone and its fabrics intact — the one
	 * state a user cannot recover from. Reboot anyway if it takes too long.
	 */
	k_timer_start(&reset_reboot_timer, K_SECONDS(10), K_NO_WAIT);
}
