#include "ble_lock_service.h"
#include "ble_radio.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_lock_svc, CONFIG_LOG_DEFAULT_LEVEL);

static const struct bt_gatt_attr *lock_status_attr;

static ssize_t read_lock_status(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				void *buf, uint16_t len, uint16_t offset)
{
	uint8_t state = (uint8_t)lock_manager_get_state();

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &state, sizeof(state));
}

static ssize_t write_lock_request(struct bt_conn *conn,
				  const struct bt_gatt_attr *attr,
				  const void *buf, uint16_t len,
				  uint16_t offset, uint8_t flags)
{
	if (len != sizeof(uint8_t) || offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	uint8_t action = *(const uint8_t *)buf;

	if (action > LOCK_ACTION_OPEN) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	int err = ble_radio_lock_request((enum lock_action)action,
				         LOCK_OP_SOURCE_REMOTE);

	if (err == -EALREADY) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	} else if (err) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(lock_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_LOCK_SERVICE),

	/* Lock Status — Read + Notify, requires encrypted connection */
	BT_GATT_CHARACTERISTIC(BT_UUID_LOCK_STATUS,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ_ENCRYPT,
		read_lock_status, NULL, NULL),
	BT_GATT_CCC(NULL,
		BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CUD("Lock status", BT_GATT_PERM_READ),

	/* Lock Request — Write, requires authenticated (bonded) connection */
	BT_GATT_CHARACTERISTIC(BT_UUID_LOCK_REQUEST,
		BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE_AUTHEN,
		NULL, write_lock_request, NULL),
	BT_GATT_CUD("Lock request", BT_GATT_PERM_READ),
);

void ble_lock_service_init(void)
{
	lock_status_attr = bt_gatt_find_by_uuid(
		lock_svc.attrs, lock_svc.attr_count, BT_UUID_LOCK_STATUS);
	if (!lock_status_attr) {
		LOG_ERR("Lock status characteristic not found");
	}
}

int ble_lock_service_notify_state(enum lock_state state)
{
	uint8_t val = (uint8_t)state;

	if (!lock_status_attr) {
		return -ENOENT;
	}

	return bt_gatt_notify(NULL, lock_status_attr, &val, sizeof(val));
}
