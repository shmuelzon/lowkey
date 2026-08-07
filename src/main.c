#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <ram_pwrdn.h>

#include "radio.h"
#include "lock_manager.h"
#include "door_monitor.h"
#include "battery_manager.h"
#include "watchdog.h"
#include "ui.h"

LOG_MODULE_REGISTER(app, CONFIG_LOG_DEFAULT_LEVEL);

/* --- Lock state changed — application-level handler ----------------- */

static void on_lock_state_changed(const struct lock_state_change *change)
{
	radio_on_lock_state_changed(change);

	/*
	 * Poll the gauge once the bolt is at rest, while the motor load is still
	 * reflected in the readings. UNLATCHED is skipped because its settle to
	 * UNLOCKED follows within seconds and would poll again for nothing.
	 */
	if (change->state == LOCK_STATE_LOCKED ||
	    change->state == LOCK_STATE_UNLOCKED) {
		battery_manager_poll();
	}
}

/* --- UI callbacks --------------------------------------------------- */

/*
 * No originator: a key press has no client behind it, and MANUAL is what a
 * Matter controller turns into "unlocked at the door" rather than attributing
 * the action to a fabric member.
 */
static int on_ui_lock_request(enum lock_action action)
{
	return lock_manager_request(action, LOCK_OP_SOURCE_MANUAL, NULL);
}

static const struct ui_cb ui_cbs = {
	.lock_request = on_ui_lock_request,
	.start_discovery = radio_start_discovery,
	.factory_reset = radio_factory_reset,
};

/* --- Battery callbacks ----------------------------------------------- */

static void on_charger_changed(bool charging, bool complete)
{
	ui_on_charging_changed(charging, complete);
	radio_on_charger_changed(charging, complete);
}

static const struct battery_manager_cb battery_cbs = {
	.on_battery_changed = radio_on_battery_changed,
	.on_charger_changed = on_charger_changed,
};

/* --- Radio callbacks ------------------------------------------------ */

static const struct radio_cb radio_cbs = {
	.lock_request = lock_manager_request,
	.on_discovery_changed = ui_on_discovery_changed,
	.on_identify = ui_on_identify,
};

/* --- Main ----------------------------------------------------------- */

int main(void)
{
	int err;

	/*
	 * Power down the SRAM sections above the image. Called here rather than
	 * left to the library's own SYS_INIT, which only exists under
	 * CONFIG_RAM_POWER_ADJUST_ON_HEAP_RESIZE — that needs newlib, and Matter
	 * forces picolibc, so nothing would call it in a BLE-only build.
	 */
	power_down_unused_ram();

	/*
	 * Peripheral init failures are logged but not fatal: the hardware may
	 * simply not be connected during development, and the radio is still
	 * worth bringing up.
	 *
	 * These register callbacks that lead into the radio layer, and the
	 * sensor interrupts are live from here, so radio.c ignores anything
	 * arriving before radio_init() rather than dispatching into backends
	 * that do not exist yet.
	 */
	err = lock_manager_init(on_lock_state_changed);
	if (err) {
		LOG_WRN("App: lock manager init failed (hardware absent?): %d",
			err);
	}

	err = door_monitor_init(radio_on_door_state_changed);
	if (err) {
		LOG_WRN("App: door monitor init failed (hardware absent?): %d",
			err);
	}

	err = ui_init(&ui_cbs);
	if (err) {
		LOG_WRN("App: UI init failed (hardware absent?): %d", err);
	}

	err = battery_manager_init(&battery_cbs);
	if (err) {
		LOG_WRN("App: battery init failed (hardware absent?): %d", err);
	}

	/* Before the radio, which claims a watchdog channel of its own. */
	err = watchdog_init();
	if (err) {
		LOG_ERR("App: watchdog init failed: %d", err);
		return err;
	}

	err = radio_init(&radio_cbs);
	if (err) {
		/*
		 * Reboot rather than return: the watchdog is already being fed
		 * from the system workqueue, so a bare return would leave a
		 * radio-less lock sitting there looking healthy.
		 */
		LOG_ERR("App: radio init failed, rebooting: %d", err);
		sys_reboot(SYS_REBOOT_WARM);
	}

	/*
	 * Publish the state the hardware was found in, now that the backends
	 * exist. operated stays clear: nobody operated the lock, and a Matter
	 * build would otherwise announce an operation on every boot.
	 *
	 * The door is deliberately absent. Its only non-eventing publish path
	 * runs from the Matter cluster init callback inside radio_init() (see
	 * matter_radio_publish_initial_state), whereas going through
	 * radio_on_door_state_changed() here would reach SetDoorState() and emit
	 * a DoorStateChange event as if the door had just moved. BLE needs no
	 * seeding — it has no subscribers yet and serves reads on demand.
	 */
	struct lock_state_change initial_state = {
		.state = lock_manager_get_state(),
		.source = LOCK_OP_SOURCE_MANUAL,
	};

	radio_on_lock_state_changed(&initial_state);
	radio_on_battery_changed(battery_get_soc(), battery_get_voltage(),
				 battery_get_remaining_mah());
	radio_on_charger_changed(battery_is_charging(),
				 battery_is_charge_complete());

	LOG_INF("App: ready (version %s)", APP_VERSION_STRING);

	return radio_run();
}
