#ifndef BLE_RADIO_H_
#define BLE_RADIO_H_

#include "lock_manager.h"
#include "radio_backend.h"

/** The BLE backend, fanned out to by radio.c. */
extern const struct radio_backend_ops ble_radio_ops;

/**
 * Request a lock action on behalf of a BLE client.
 * Called by BLE services; routes through the registered radio callback.
 * @return 0 on success, negative errno on failure.
 */
int ble_radio_lock_request(enum lock_action action,
			   enum lock_op_source source);

#endif /* BLE_RADIO_H_ */
