#ifndef RADIO_BACKEND_H_
#define RADIO_BACKEND_H_

#include <stdbool.h>
#include <stdint.h>

#include "lock_manager.h"
#include "door_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The contract between radio.c and the protocol backends (BLE, Matter). The
 * application never sees this — it calls the radio_*() functions in radio.h,
 * which fan out to whichever backends are enabled.
 */

/**
 * Callbacks from a backend up into radio.c. Each backend gets its own instance
 * during init, carrying its identity where that matters.
 */
struct radio_backend_cb {
	/**
	 * Request a bolt/latch operation.
	 * @param originator Client behind a remote request, or NULL when the
	 *                   backend has no identity to report.
	 */
	int (*lock_request)(enum lock_action action,
			    enum lock_op_source source,
			    const struct lock_originator *originator);
	void (*on_discovery_changed)(bool discoverable);
	void (*on_identify)(bool active);

	/**
	 * A client finished onboarding on this backend — a BLE peer completed
	 * pairing, or a Matter commissioner completed commissioning.
	 *
	 * One button press opens an onboarding window on every backend at once
	 * and lets the user's app pick a protocol; the first one to succeed ends
	 * the whole session. Reporting it here is what lets radio.c close the
	 * other backends' windows. May be NULL.
	 */
	void (*on_onboarded)(void);
};

/**
 * Radio backend operations. Each radio implementation (BLE, Matter, ...)
 * provides a single instance of this struct.
 */
struct radio_backend_ops {
	/**
	 * Initialize the radio stack. Peripheral hardware is already
	 * initialized before this is called.
	 * @param cbs  Callbacks for radio-initiated events.
	 * @return 0 on success, negative errno on failure.
	 */
	int (*init)(const struct radio_backend_cb *cbs);

	/**
	 * Run the radio event loop. May block forever (e.g. Matter) or
	 * return immediately (e.g. BLE on Zephyr's cooperative scheduler).
	 * @return 0 on success, negative errno on failure.
	 */
	int (*run)(void);

	/** Notify the radio layer that the lock state has changed. */
	void (*on_lock_state_changed)(const struct lock_state_change *change);

	/** Notify the radio layer that the door state has changed. */
	void (*on_door_state_changed)(enum door_state state);

	/** Notify the radio layer that the battery state has changed. */
	void (*on_battery_changed)(uint8_t soc_percent, uint16_t voltage_mv,
				   uint16_t remaining_mah);

	/**
	 * Notify the radio layer that the charger state has changed.
	 * @param charging  true while the pack is actively charging.
	 * @param complete  true when external power is present but charging
	 *                  has finished (fully charged).
	 * Neither flag set means the device is running on battery.
	 * May be NULL for backends that do not expose charge state.
	 */
	void (*on_charger_changed)(bool charging, bool complete);

	/**
	 * Make the device discoverable to new clients for a limited time.
	 * BLE: switch to general advertising. Matter: open commissioning.
	 * @return 0 on success, negative errno on failure.
	 */
	int (*start_discovery)(void);

	/**
	 * Close this backend's onboarding window early, before its timeout.
	 * Called when another backend reports a completed onboarding, so this
	 * is not a failure — the window simply is not needed any more.
	 * May be NULL for backends with no window to close.
	 */
	void (*stop_discovery)(void);

	/**
	 * Erase this backend's pairing/commissioning data.
	 *
	 * Only ever reached from the factory-reset button, never from a
	 * protocol event: a Matter fabric removal must not take BLE bonds with
	 * it. Backends that reboot themselves must say so with
	 * factory_reset_reboots below.
	 */
	void (*factory_reset)(void);

	/**
	 * True if factory_reset() reboots the device on its own, possibly
	 * asynchronously. radio.c runs every non-rebooting backend first so
	 * their erases complete, then the rebooting one, and reboots itself
	 * only if no backend did.
	 */
	bool factory_reset_reboots;
};

#ifdef __cplusplus
}
#endif

#endif /* RADIO_BACKEND_H_ */
