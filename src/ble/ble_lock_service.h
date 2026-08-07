#ifndef BLE_LOCK_SERVICE_H_
#define BLE_LOCK_SERVICE_H_

#include "lock_manager.h"

#include <zephyr/bluetooth/uuid.h>

/* Service UUID: 1e3557d1-0001-5887-a357-c9acbd789b14 */
#define BT_UUID_LOCK_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x1e3557d1, 0x0001, 0x5887, 0xa357, 0xc9acbd789b14)
#define BT_UUID_LOCK_SERVICE BT_UUID_DECLARE_128(BT_UUID_LOCK_SERVICE_VAL)

/* Lock Status UUID: 1e3557d1-0002-5887-a357-c9acbd789b14 */
#define BT_UUID_LOCK_STATUS_VAL \
	BT_UUID_128_ENCODE(0x1e3557d1, 0x0002, 0x5887, 0xa357, 0xc9acbd789b14)
#define BT_UUID_LOCK_STATUS BT_UUID_DECLARE_128(BT_UUID_LOCK_STATUS_VAL)

/* Lock Request UUID: 1e3557d1-0003-5887-a357-c9acbd789b14 */
#define BT_UUID_LOCK_REQUEST_VAL \
	BT_UUID_128_ENCODE(0x1e3557d1, 0x0003, 0x5887, 0xa357, 0xc9acbd789b14)
#define BT_UUID_LOCK_REQUEST BT_UUID_DECLARE_128(BT_UUID_LOCK_REQUEST_VAL)

void ble_lock_service_init(void);
int ble_lock_service_notify_state(enum lock_state state);

#endif /* BLE_LOCK_SERVICE_H_ */
