#ifndef UI_H_
#define UI_H_

#include <stdbool.h>

#include "lock_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callbacks from the UI module to the application.
 *
 * Which key press invokes which callback is decided at build time from
 * the device tree — see the key action table at the top of ui.c.
 */
struct ui_cb {
	/** Operate the bolt. Returns 0, or a negative errno (-EALREADY
	 *  when the lock is already in the requested position). */
	int (*lock_request)(enum lock_action action);

	/** Start BLE/Matter discovery. */
	int (*start_discovery)(void);

	/** Erase all pairing/commissioning data and reboot. */
	void (*factory_reset)(void);
};

/**
 * Initialize the user interface (keys + LED).
 *
 * Uses DT aliases `key0` (always) and `key1` (optional) for the keys,
 * and `led0` for the LED.  If the hardware is absent the function
 * returns a negative errno; the caller should treat this as non-fatal
 * since the UI is not mission-critical.
 *
 * @param cbs  Application callbacks.
 * @return 0 on success, negative errno on failure.
 */
int ui_init(const struct ui_cb *cbs);

/** Signal that the device is now discoverable (LED blinks). */
void ui_on_discovery_changed(bool discoverable);

/** Signal an Identify cluster event (LED blinks while active). */
void ui_on_identify(bool active);

/**
 * Signal charger state change (LED blinks while charging, solid when
 * complete).  Lower priority than discovery and identify.
 *
 * @param charging  true if charge is in progress.
 * @param complete  true if charge is complete and power is still connected.
 */
void ui_on_charging_changed(bool charging, bool complete);

#ifdef __cplusplus
}
#endif

#endif /* UI_H_ */
