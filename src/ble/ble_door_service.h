#ifndef BLE_DOOR_SERVICE_H_
#define BLE_DOOR_SERVICE_H_

#include "door_monitor.h"

#include <zephyr/bluetooth/uuid.h>

/* Service UUID: 1e3557d1-0010-5887-a357-c9acbd789b14 */
#define BT_UUID_DOOR_STATE_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x1e3557d1, 0x0010, 0x5887, 0xa357, 0xc9acbd789b14)
#define BT_UUID_DOOR_STATE_SERVICE \
	BT_UUID_DECLARE_128(BT_UUID_DOOR_STATE_SERVICE_VAL)

/* Door State UUID: 1e3557d1-0011-5887-a357-c9acbd789b14 */
#define BT_UUID_DOOR_STATE_VAL \
	BT_UUID_128_ENCODE(0x1e3557d1, 0x0011, 0x5887, 0xa357, 0xc9acbd789b14)
#define BT_UUID_DOOR_STATE BT_UUID_DECLARE_128(BT_UUID_DOOR_STATE_VAL)

void ble_door_service_init(void);
int ble_door_service_notify_state(enum door_state state);

#endif /* BLE_DOOR_SERVICE_H_ */
