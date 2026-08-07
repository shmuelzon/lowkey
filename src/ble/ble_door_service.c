#include "ble_door_service.h"
#include "door_monitor.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_door_svc, CONFIG_LOG_DEFAULT_LEVEL);

static const struct bt_gatt_attr *door_state_attr;

static ssize_t read_door_state(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	uint8_t state = (uint8_t)door_monitor_get_state();

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &state, sizeof(state));
}

BT_GATT_SERVICE_DEFINE(door_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DOOR_STATE_SERVICE),

	/* Door State — Read + Notify, requires encrypted connection */
	BT_GATT_CHARACTERISTIC(BT_UUID_DOOR_STATE,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ_ENCRYPT,
		read_door_state, NULL, NULL),
	BT_GATT_CCC(NULL,
		BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CUD("Door state", BT_GATT_PERM_READ),
);

void ble_door_service_init(void)
{
	door_state_attr = bt_gatt_find_by_uuid(
		door_svc.attrs, door_svc.attr_count, BT_UUID_DOOR_STATE);
	if (!door_state_attr) {
		LOG_ERR("Door state characteristic not found");
	}
}

int ble_door_service_notify_state(enum door_state state)
{
	uint8_t val = (uint8_t)state;

	if (!door_state_attr) {
		return -ENOENT;
	}

	return bt_gatt_notify(NULL, door_state_attr, &val, sizeof(val));
}
