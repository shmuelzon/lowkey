#ifndef RADIO_H_
#define RADIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "lock_manager.h"
#include "door_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callbacks from the radio layer to the application.
 * Registered during radio_init() so the radio layer can request actions or
 * notify the application without knowing peripheral details.
 */
struct radio_cb {
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
};

/**
 * Initialize the radio layer. Peripheral hardware is already initialized
 * before this is called.
 * @param cbs  Application callbacks for radio-initiated events.
 * @return 0 on success, negative errno on failure.
 */
int radio_init(const struct radio_cb *cbs);

/**
 * Run the radio event loop. Does not return in a Matter build (the CHIP task
 * dispatch loop runs here); returns immediately in a BLE-only build, where the
 * work happens on the Bluetooth RX thread and the system workqueue.
 * @return 0 on success, negative errno on failure.
 */
int radio_run(void);

/** Notify the radio layer that the lock state has changed. */
void radio_on_lock_state_changed(const struct lock_state_change *change);

/** Notify the radio layer that the door state has changed. */
void radio_on_door_state_changed(enum door_state state);

/** Notify the radio layer that the battery state has changed. */
void radio_on_battery_changed(uint8_t soc_percent, uint16_t voltage_mv,
			      uint16_t remaining_mah);

/**
 * Notify the radio layer that the charger state has changed.
 * @param charging  true while the pack is actively charging.
 * @param complete  true when external power is present but charging has
 *                  finished (fully charged).
 * Neither flag set means the device is running on battery.
 */
void radio_on_charger_changed(bool charging, bool complete);

/**
 * Make the device discoverable to new clients for a limited time: BLE switches
 * to general advertising and Matter opens commissioning, so the user's app can
 * pick a protocol.
 * @return 0 on success, negative errno if any protocol failed to open its
 *         window.
 */
int radio_start_discovery(void);

/**
 * Erase all pairing/commissioning data and reboot.
 *
 * Only ever reached from the factory-reset button, never from a protocol
 * event: a Matter fabric removal must not take BLE bonds with it. Does not
 * return in the usual case — the device reboots once every backend has erased
 * its state.
 */
void radio_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H_ */
