#include "ble_radio.h"
#include "ble_lock_service.h"
#include "ble_door_service.h"
#include "radio_backend.h"
#include "battery_manager.h"
#include "lock_manager.h"
#include "door_monitor.h"
#include "factory_data.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_radio, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * CONFIG_CHIP_DFU_OVER_BT_SMP, which a combined build needs to arbitrate
 * against Matter OTA, defaults the SMP permission choice to plain read/write —
 * where a BLE-only build gets the authenticated variant for free from BT_SMP.
 * Kconfig puts it back; this fails the build rather than quietly shipping an
 * open firmware-upload service if either default ever moves.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_AUTHEN),
	     "SMP over Bluetooth must require an authenticated link");

static const struct radio_backend_cb *app_cbs;
static uint32_t ble_passkey = 123456;

/* --- Advertising state machine -------------------------------------- */

enum adv_state {
	ADV_IDLE,
	ADV_BONDED,
	ADV_DISCOVERY,
};

/*
 * Written only from the system workqueue — every path that changes it
 * (adv_work, discovery_stop_work, the UI-driven discovery and factory-reset
 * entry points) runs there, so they serialise against each other.
 *
 * The connection callbacks run on the Bluetooth RX thread instead, and so touch
 * nothing here: they set ble_connected and submit adv_work.
 */
static enum adv_state adv_state;
static atomic_t ble_connected;

/*
 * The mode the advertising set was last programmed with. Only meaningful while
 * the set is actually enabled, and whether it is enabled is asked of the host
 * (adv_is_enabled()) rather than shadowed in a flag of our own.
 *
 * That is deliberate. The controller stops a connectable set the moment it
 * accepts a connection, and Zephyr also stops and restarts it behind our back
 * while it updates the controller's resolving list (adv_pause_enabled() in the
 * host's id.c). A local "is it running" bit therefore has to be cleared from
 * contexts that race the workqueue, and losing one clear is unrecoverable: the
 * early return in start_advertising() believes the advertiser is already in the
 * wanted state and never restarts it, leaving the lock unreachable until it is
 * power cycled.
 */
static enum adv_state adv_programmed_state;

static struct k_timer discovery_timer;
static struct k_work_delayable adv_work;
static struct k_work discovery_stop_work;

/*
 * bt_le_ext_adv_start() reserves the bt_conn for the next incoming link, so it
 * fails with -ENOMEM while the stack is still holding the one just torn down —
 * with CONFIG_BT_MAX_CONN at 1, in a BLE-only build, that is the only one there
 * is. Retry rather than return: nothing else would come along to try again, and
 * an advertiser that stays down takes the lock off the air until it reboots.
 *
 * The backoff is what keeps that from becoming its own problem. A transient
 * failure clears on the first retry; one that does not clear is a fault no
 * amount of retrying fixes, and twice a second forever is a real current draw
 * on a battery device. A connect or disconnect still gets an immediate attempt
 * whatever the backoff has grown to — see request_advertising_update().
 */
#define ADV_RETRY_MIN_MS 500
#define ADV_RETRY_MAX_MS 30000

static uint32_t adv_retry_ms;

/*
 * Our own extended-advertising set, on BT_ID_DEFAULT.
 *
 * With Matter also built in, Matter owns Bluetooth identity 1 — BLEManagerImpl
 * picks that whenever CONFIG_BT_BONDABLE is set, which CONFIG_BT_SMP turns on —
 * and advertises for commissioning through its own BLEAdvertisingArbiter on the
 * legacy set. Keeping our services on identity 0 with a set of our own means the
 * two advertise concurrently instead of taking turns, and our bonds sit on an
 * identity Matter never touches. That second part matters: BLEManagerImpl calls
 * bt_id_reset() on its own identity on every boot after the first, which
 * unpairs everything bonded to it.
 *
 * An extended-advertising *set* is used even in a BLE-only build so there is
 * one advertising code path rather than two. The advertisements themselves stay
 * legacy — BT_LE_ADV_OPT_EXT_ADV is deliberately not set, for scanner
 * compatibility.
 */
static struct bt_le_ext_adv *adv_set;

/*
 * Per-connection TX power is applied from a work item, so the handle has to
 * survive the hop to the system workqueue. Indexed by connection: with a
 * commissioning link and ours both possible, a single global could be
 * overwritten between submit and run.
 */
static struct k_work conn_txp_work[CONFIG_BT_MAX_CONN];
static uint16_t conn_txp_handle[CONFIG_BT_MAX_CONN];

/* HCI connection handles are 12-bit, so this cannot collide with a real one. */
#define CONN_HANDLE_NONE 0xFFFFU

/*
 * Discovery advertises LE General Discoverable Mode. The bonded state sets
 * neither discoverable bit — BR/EDR-not-supported and nothing else — which is
 * GAP's encoding for connectable but *not* discoverable (Core spec Vol 3,
 * Part C, 9.3.2). A scanner that honours the flags, and every "discoverable
 * devices only" filter, drops it on that basis alone.
 */
static const uint8_t adv_flags_discovery = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
static const uint8_t adv_flags_bonded = BT_LE_AD_NO_BREDR;

/* Flags plus the suffixed name; filled in by build_adv_data(). */
static struct bt_data ad_discovery[2];

/*
 * The steady-state payload: the three bytes of a flags field and nothing else.
 *
 * A bonded lock has to keep advertising to be connectable — Bluetooth has no
 * passive-listen state — but nothing obliges it to say what it is while doing
 * so. Without the name there is no product, no vendor and no per-device serial
 * on the air, so a scanner that is not already bonded sees an unnamed device it
 * cannot identify and, thanks to the filter policy below, cannot connect to
 * either. A peer that *is* bonded needs none of it: it has the address, and the
 * name it displays comes from the GATT Device Name it read while paired.
 */
static const struct bt_data ad_bonded[] = {
	BT_DATA(BT_DATA_FLAGS, &adv_flags_bonded, sizeof(adv_flags_bonded)),
};

/*
 * The advertised name, "<GAP name>-<last 4 of the serial>", e.g. LowKey-21C4.
 *
 * Built from CONFIG_BT_DEVICE_NAME rather than bt_get_name(), because a
 * BLE-only build writes the result back with bt_set_name(): CONFIG_BT_SETTINGS
 * persists that and settings_load() restores it, so taking the running name as
 * the base would append a second suffix on every boot.
 */
#define ADV_NAME_SUFFIX "-XXXX"
static char adv_name[sizeof(CONFIG_BT_DEVICE_NAME) + sizeof(ADV_NAME_SUFFIX) - 1];

#if !defined(CONFIG_LOCK_RADIO_MATTER)
BUILD_ASSERT(sizeof(adv_name) - 1 <= CONFIG_BT_DEVICE_NAME_MAX,
	     "CONFIG_BT_DEVICE_NAME_MAX cannot hold the suffixed device name");
#endif

/*
 * Scan response, sent in discovery mode only — the service UUID is what makes
 * the lock findable while the user is deliberately pairing, and it is exactly
 * what should not be on the air the rest of the time. See ad_bonded above.
 */
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_SOME, BT_UUID_LOCK_SERVICE_VAL),
};

/*
 * The suffix reuses the factory-data serial rather than a field of its own, so
 * the advertised name can go on the sticker alongside the pairing PIN without
 * reading anything back off the device.
 */
static void build_adv_data(void)
{
	const char *base = CONFIG_BT_DEVICE_NAME;
	size_t len = sizeof(CONFIG_BT_DEVICE_NAME) - 1;
	const uint8_t *sn;
	size_t sn_len;

	memcpy(adv_name, base, len);

	if (factory_data_get_string(FACTORY_DATA_STRING_SERIAL, &sn, &sn_len) &&
	    sn_len >= 4) {
		adv_name[len++] = '-';
		memcpy(&adv_name[len], &sn[sn_len - 4], 4);
		len += 4;
	} else {
		LOG_WRN("BLE: no serial in factory data, advertising as \"%s\" "
			"— this device is not distinguishable by name", base);
	}

	adv_name[len] = '\0';
	LOG_INF("BLE: advertising as \"%s\"", adv_name);

	/*
	 * In a BLE-only build the suffixed name is also the GAP name, which is
	 * what a phone shows once it has bonded — from then on it reads the
	 * GATT Device Name characteristic instead of the advertised Local Name,
	 * and a generic "LowKey" there makes two locks indistinguishable in the
	 * system Bluetooth list.
	 *
	 * A combined build must keep the GAP name generic: Matter puts it in
	 * its commissioning scan response, so a per-device name there would
	 * leave a scanner showing two similarly named entries of which only one
	 * carries the lock service.
	 */
	if (!IS_ENABLED(CONFIG_LOCK_RADIO_MATTER)) {
		int err = bt_set_name(adv_name);

		if (err) {
			LOG_WRN("BLE: could not set GAP name: %d", err);
		}
	}

	ad_discovery[0] = (struct bt_data)BT_DATA(BT_DATA_FLAGS,
						  &adv_flags_discovery,
						  sizeof(adv_flags_discovery));
	ad_discovery[1] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE,
						  adv_name, (uint8_t)len);
}

/* Discovery: fast advertising, only during the limited window. */
static const struct bt_le_adv_param adv_param_general =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN,
			      BT_GAP_ADV_FAST_INT_MIN_2,
			      BT_GAP_ADV_FAST_INT_MAX_2,
			      NULL);

/*
 * Bonded: slow advertising, and both halves of the filter accept list.
 *
 * FILTER_CONN is what makes a stranger's connection attempt fail; FILTER_SCAN_REQ
 * stops an active scanner eliciting a scan response at all. Neither hides the
 * advertisement itself — an undirected connectable PDU is visible to any passive
 * scanner by construction, and the only advertising a scanner is obliged to
 * ignore is ADV_DIRECT_IND, which no generic BLE app can then reconnect to. So
 * the payload does the work here instead: see ad_bonded.
 *
 * Reconnection is less responsive at this interval, but advertising is what the
 * lock does all day, and the ~1 s interval takes roughly an order of magnitude
 * off its average current.
 */
static const struct bt_le_adv_param adv_param_whitelist =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN |
			      BT_LE_ADV_OPT_FILTER_CONN |
			      BT_LE_ADV_OPT_FILTER_SCAN_REQ,
			      BT_GAP_ADV_SLOW_INT_MIN,
			      BT_GAP_ADV_SLOW_INT_MAX,
			      NULL);

/*
 * Zephyr's CONFIG_BT_CTLR_TX_PWR_* choice only reaches the software link layer,
 * so on the SoftDevice Controller the level has to be written over the
 * vendor-specific HCI command instead (see LOCK_BLE_TX_POWER_DBM). Advertising
 * and connections are separate handles and neither inherits from the other,
 * hence one call per advertising start and one per connection.
 */
static void set_tx_power(uint8_t handle_type, uint16_t handle)
{
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	int err;

	if (CONFIG_LOCK_BLE_TX_POWER_DBM == 0) {
		return;
	}

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		LOG_ERR("BLE: no HCI buf for TX power");
		return;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle_type = handle_type;
	cp->handle = sys_cpu_to_le16(handle);
	cp->tx_power_level = CONFIG_LOCK_BLE_TX_POWER_DBM;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf,
				   &rsp);
	if (err) {
		/* Levels the SoC cannot produce are rejected, not clamped. */
		LOG_ERR("BLE: TX power %d dBm rejected (type %u): %d",
			CONFIG_LOCK_BLE_TX_POWER_DBM, handle_type, err);
		return;
	}

	if (!rsp) {
		return;
	}

	rp = (void *)rsp->data;
	LOG_INF("BLE: TX power (type %u) = %d dBm", handle_type,
		rp->selected_tx_power);
	net_buf_unref(rsp);
}

static void conn_txp_work_handler(struct k_work *work)
{
	size_t i = work - conn_txp_work;
	uint16_t handle = conn_txp_handle[i];

	/*
	 * The slot is indexed by bt_conn_index(), which the stack reuses once a
	 * connection is released. A link that dropped before this ran would have
	 * its handle applied to whatever took its place, so the claim is dropped
	 * here and a stale one simply does nothing.
	 */
	if (handle == CONN_HANDLE_NONE) {
		return;
	}

	conn_txp_handle[i] = CONN_HANDLE_NONE;
	set_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_CONN, handle);
}

/* Apply the configured TX power to the advertising set we just started. */
static void set_adv_tx_power(void)
{
	uint8_t handle;

	if (adv_set && bt_hci_get_adv_handle(adv_set, &handle) == 0) {
		set_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, handle);
	}
}

static void add_bonded_addr(const struct bt_bond_info *info, void *user_data)
{
	bool *has_bonds = user_data;

	bt_le_filter_accept_list_add(&info->addr);
	*has_bonds = true;
}

static bool populate_filter_accept_list(void)
{
	bool has_bonds = false;

	bt_le_filter_accept_list_clear();
	bt_foreach_bond(BT_ID_DEFAULT, add_bonded_addr, &has_bonds);
	return has_bonds;
}

/*
 * The host's own view of the advertising set, which is the only one that stays
 * true across everything that can stop it without going through us.
 */
static bool adv_is_enabled(void)
{
	struct bt_le_ext_adv_info info;

	return adv_set && bt_le_ext_adv_get_info(adv_set, &info) == 0 &&
	       info.ext_adv_state == BT_LE_EXT_ADV_STATE_ENABLED;
}

static void stop_advertising(void)
{
	int err;

	if (!adv_set) {
		return;
	}

	err = bt_le_ext_adv_stop(adv_set);
	if (err) {
		LOG_WRN("BLE: adv stop failed: %d", err);
	}
}

/*
 * Bring the advertising set in line with adv_state. Runs only from the system
 * workqueue.
 *
 * Switching between bonded and discovery changes both parameters and data, and
 * bt_le_ext_adv_update_param() requires the set stopped, so a real change means
 * stop, reconfigure, start.
 *
 * The no-op when already in the wanted state is load-bearing, not an
 * optimisation: bt_le_ext_adv_stop() on a connectable set releases the bt_conn
 * the controller reserved for an incoming connection (le_adv_stop_free_conn()
 * in Zephyr's adv.c), and returning it to the pool fires the `recycled`
 * callback — which is where advertising is restarted. Without the early return,
 * our own stop drives recycled, which calls back in here, which stops again:
 * an unbounded loop on the system workqueue that starves everything else,
 * Matter's commissioning advertising included.
 */
static void start_advertising(void)
{
	const struct bt_le_adv_param *param;
	const struct bt_data *adv_data;
	size_t adv_data_len;
	const struct bt_data *scan_rsp;
	size_t scan_rsp_len;
	int err;

	if (!adv_set || atomic_get(&ble_connected)) {
		return;
	}

	if (adv_state == ADV_IDLE) {
		stop_advertising();
		return;
	}

	if (adv_is_enabled() && adv_programmed_state == adv_state) {
		return;
	}

	/*
	 * Stop before touching the filter accept list. The controller rejects
	 * LE Clear/Add Device To Filter Accept List with Command Disallowed
	 * (0x0c) while an enabled advertiser is using that list as its filter
	 * policy, which is what populating it from under a running whitelist
	 * advertiser would do.
	 */
	stop_advertising();

	switch (adv_state) {
	case ADV_BONDED:
		if (!populate_filter_accept_list()) {
			adv_state = ADV_IDLE;
			return;
		}
		param = &adv_param_whitelist;
		adv_data = ad_bonded;
		adv_data_len = ARRAY_SIZE(ad_bonded);
		scan_rsp = NULL;
		scan_rsp_len = 0;
		break;
	case ADV_DISCOVERY:
		param = &adv_param_general;
		adv_data = ad_discovery;
		adv_data_len = ARRAY_SIZE(ad_discovery);
		scan_rsp = sd;
		scan_rsp_len = ARRAY_SIZE(sd);
		break;
	default:
		return;
	}

	err = bt_le_ext_adv_update_param(adv_set, param);
	if (err) {
		LOG_ERR("BLE: adv param update failed: %d", err);
		goto retry;
	}

	err = bt_le_ext_adv_set_data(adv_set, adv_data, adv_data_len,
				     scan_rsp, scan_rsp_len);
	if (err) {
		LOG_ERR("BLE: adv data set failed: %d", err);
		goto retry;
	}

	err = bt_le_ext_adv_start(adv_set, BT_LE_EXT_ADV_START_DEFAULT);
	if (err) {
		LOG_ERR("BLE: adv start failed: %d", err);
		goto retry;
	}

	adv_programmed_state = adv_state;
	adv_retry_ms = 0;

	/*
	 * Worth a line: the two states differ only in what a scanner sees, so
	 * this is the cheapest way to tell them apart from the outside.
	 */
	LOG_INF("BLE: advertising (%s)",
		adv_state == ADV_DISCOVERY ? "discovery, named"
					   : "bonded peers only, unnamed");

	set_adv_tx_power();
	return;

retry:
	adv_retry_ms = adv_retry_ms ? MIN(adv_retry_ms * 2, ADV_RETRY_MAX_MS)
				    : ADV_RETRY_MIN_MS;
	k_work_reschedule(&adv_work, K_MSEC(adv_retry_ms));
}

static void adv_work_handler(struct k_work *work)
{
	start_advertising();
}

/*
 * Ask for a reconciliation from a context that must not do the work itself —
 * the connection callbacks, which run on the Bluetooth RX thread.
 *
 * Reschedule rather than schedule, so a real event always brings a pending
 * retry forward instead of waiting out its backoff. The backoff itself is left
 * alone here: only the workqueue writes it, and the attempt this causes happens
 * immediately either way.
 */
static void request_advertising_update(void)
{
	k_work_reschedule(&adv_work, K_NO_WAIT);
}

static void discovery_stop_handler(struct k_work *work)
{
	stop_advertising();
	adv_state = populate_filter_accept_list() ? ADV_BONDED : ADV_IDLE;
	start_advertising();

	if (app_cbs && app_cbs->on_discovery_changed) {
		app_cbs->on_discovery_changed(false);
	}

	LOG_INF("BLE: discovery ended");
}

static void discovery_timer_expiry(struct k_timer *timer)
{
	k_work_submit(&discovery_stop_work);
}

/* --- factory data --------------------------------------------------- */

static void apply_dis_str(const char *dis_key, enum factory_data_string field)
{
	const uint8_t *val;
	size_t len;

	if (factory_data_get_string(field, &val, &len)) {
		settings_runtime_set(dis_key, val, len);
	}
}

static void load_and_apply_factory_data(void)
{
	apply_dis_str("bt/dis/serial", FACTORY_DATA_STRING_SERIAL);
	apply_dis_str("bt/dis/manuf", FACTORY_DATA_STRING_VENDOR_NAME);
	apply_dis_str("bt/dis/model", FACTORY_DATA_STRING_PRODUCT_NAME);
	apply_dis_str("bt/dis/hw", FACTORY_DATA_STRING_HW_VER);

	ble_passkey = factory_data_ble_passkey(ble_passkey);
}

/* --- connection callbacks ------------------------------------------- */

/*
 * A combined build also carries Matter's CHIPoBLE links, on a different
 * identity; letting those through the handlers below would restart our
 * advertiser on a commissioner's disconnect. Matched by identity rather than by
 * tracking bt_conn pointers, because the identity is what the advertising set
 * was created with and cannot drift out of sync.
 */
static bool is_our_conn(struct bt_conn *conn)
{
	struct bt_conn_info info;

	return bt_conn_get_info(conn, &info) == 0 && info.id == BT_ID_DEFAULT;
}

/*
 * Per-advertising-set connected callback rather than the global one: it fires
 * only for connections this advertiser accepted, so there is nothing to filter.
 */
static void adv_connected(struct bt_le_ext_adv *adv,
			  struct bt_le_ext_adv_connected_info *info)
{
	uint8_t idx = bt_conn_index(info->conn);

	atomic_set(&ble_connected, 1);
	LOG_INF("BLE: connected");

	/*
	 * Deferred: set_tx_power() blocks until the command-complete is
	 * processed, which happens in the context this callback runs in.
	 */
	if (idx < ARRAY_SIZE(conn_txp_work) &&
	    bt_hci_get_conn_handle(info->conn, &conn_txp_handle[idx]) == 0) {
		k_work_submit(&conn_txp_work[idx]);
	}

	/*
	 * Per-connection rather than through CONFIG_BT_PERIPHERAL_PREF_*, which
	 * is global and would slow Matter commissioning too — CHIPoBLE wants the
	 * much shorter interval Matter's own defaults ask for.
	 */
	if (IS_ENABLED(CONFIG_LOCK_RADIO_MATTER)) {
		const struct bt_le_conn_param param = {
			.interval_min = 200,	/* 250 ms */
			.interval_max = 200,
			.latency = 0,
			.timeout = 400,
		};

		bt_conn_le_param_update(info->conn, &param);
	}
}

static const struct bt_le_ext_adv_cb adv_cbs = {
	.connected = adv_connected,
};

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	uint8_t idx = bt_conn_index(conn);

	if (idx < ARRAY_SIZE(conn_txp_handle)) {
		conn_txp_handle[idx] = CONN_HANDLE_NONE;
	}

	if (!is_our_conn(conn)) {
		return;
	}

	LOG_INF("BLE: disconnected: 0x%02x", reason);
	atomic_set(&ble_connected, 0);

	/*
	 * Zephyr 4.0 dropped automatic resumption, so the restart is ours to
	 * ask for. Deferred to the workqueue rather than done here because a
	 * connectable advertiser started before the bt_conn is back in the pool
	 * fails with -ENOMEM; by the time the work runs the stack has dropped
	 * its own reference.
	 *
	 * The `recycled` callback asks for the same thing and is the more
	 * precise signal, but it only fires once *every* reference to the
	 * connection has been released — one held a moment longer by another
	 * subsystem would leave the lock off the air. Both paths converge on
	 * one idempotent work item, and start_advertising() retries anyway if
	 * it turns out to be early.
	 */
	request_advertising_update();
}

/*
 * A bt_conn returned to the pool, so there is room to advertise connectably
 * again. Fires for any connection, ours or Matter's, and also for the
 * placeholder our own stop_advertising() releases — see start_advertising() for
 * why that last case must not become a loop.
 */
static void recycled(void)
{
	request_advertising_update();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (!is_our_conn(conn)) {
		return;
	}

	if (err) {
		LOG_ERR("BLE: security failed: level %u err %d", level, err);
	} else {
		LOG_INF("BLE: security level %u", level);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

static uint32_t auth_app_passkey(struct bt_conn *conn)
{
	return ble_passkey;
}

static void auth_cancel(struct bt_conn *conn)
{
	LOG_INF("BLE: pairing cancelled");
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	LOG_INF("BLE: paired, bonded=%d", bonded);

	/*
	 * Only a bond counts as onboarding — an unbonded pairing lasts one
	 * connection and leaves nothing behind to reconnect with.
	 */
	if (bonded && app_cbs && app_cbs->on_onboarded) {
		app_cbs->on_onboarded();
	}
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("BLE: pairing failed: %d", reason);
}

#if defined(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
/*
 * Refuse pairing that arrives on any identity but ours.
 *
 * CHIPoBLE characteristics are unencrypted so a commissioner has no reason to
 * pair, but some phone stacks bond opportunistically on connect, and with a
 * passkey configured that stalls commissioning waiting for a PIN nobody was
 * asked for. One auth callback serves the whole process, so this is the only
 * place to draw the line.
 */
static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *const feat)
{
	if (!is_our_conn(conn)) {
		LOG_INF("BLE: refusing pairing on another identity");
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}

	return BT_SECURITY_ERR_SUCCESS;
}
#endif

static struct bt_conn_auth_cb conn_auth_callbacks = {
	.app_passkey = auth_app_passkey,
	.cancel = auth_cancel,
#if defined(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
	.pairing_accept = pairing_accept,
#endif
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* --- radio_backend_ops implementation ------------------------------- */

static void ble_on_lock_state_changed(const struct lock_state_change *change)
{
	/* The characteristic carries the position only. */
	ble_lock_service_notify_state(change->state);
}

static void ble_on_door_state_changed(enum door_state state)
{
	ble_door_service_notify_state(state);
}

static void ble_on_battery_changed(uint8_t soc_percent, uint16_t voltage_mv,
				   uint16_t remaining_mah)
{
	bt_bas_set_battery_level(soc_percent);
}

static int ble_start_discovery(void)
{
	stop_advertising();
	adv_state = ADV_DISCOVERY;
	k_timer_start(&discovery_timer,
		      K_SECONDS(CONFIG_LOCK_DISCOVERY_TIMEOUT_S), K_NO_WAIT);
	start_advertising();

	if (app_cbs && app_cbs->on_discovery_changed) {
		app_cbs->on_discovery_changed(true);
	}

	LOG_INF("BLE: discoverable for %d s", CONFIG_LOCK_DISCOVERY_TIMEOUT_S);
	return 0;
}

/* Same path as the timeout, minus the timer. */
static void ble_stop_discovery(void)
{
	if (adv_state != ADV_DISCOVERY) {
		return;
	}

	k_timer_stop(&discovery_timer);
	k_work_submit(&discovery_stop_work);
}

/* Bonds only — radio.c owns the reboot; see factory_reset_reboots. */
static void ble_factory_reset(void)
{
	LOG_INF("BLE: factory reset — clearing bonds");
	stop_advertising();
	k_timer_stop(&discovery_timer);
	bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	bt_le_filter_accept_list_clear();
	adv_state = ADV_IDLE;
}

static int ble_init(const struct radio_backend_cb *cbs)
{
	int err;

	app_cbs = cbs;

	/*
	 * Before anything can touch them. The connection callbacks are
	 * registered at link time, and in a combined build Matter has the stack
	 * up — and can be servicing a commissioning link — before this function
	 * is entered, so `recycled` may reach request_advertising_update()
	 * immediately.
	 */
	k_work_init_delayable(&adv_work, adv_work_handler);
	k_work_init(&discovery_stop_work, discovery_stop_handler);
	for (size_t i = 0; i < ARRAY_SIZE(conn_txp_work); i++) {
		k_work_init(&conn_txp_work[i], conn_txp_work_handler);
		conn_txp_handle[i] = CONN_HANDLE_NONE;
	}
	k_timer_init(&discovery_timer, discovery_timer_expiry, NULL);

	err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err) {
		LOG_ERR("BLE: auth cb register failed: %d", err);
		return err;
	}

	err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
	if (err) {
		LOG_ERR("BLE: auth info cb register failed: %d", err);
		return err;
	}

	/*
	 * Matter owns the Bluetooth stack when built in: radio.c initialises it
	 * first, and its bt_enable()/settings_load() have run by the time
	 * matter_init() returns. Calling either again here would fail.
	 */
	if (!IS_ENABLED(CONFIG_LOCK_RADIO_MATTER)) {
		err = bt_enable(NULL);
		if (err) {
			LOG_ERR("BLE: init failed: %d", err);
			return err;
		}

		if (IS_ENABLED(CONFIG_SETTINGS)) {
			settings_load();
		}
	}
	LOG_INF("BLE: init");

	/*
	 * If the identity count is ever wrong, our bonds land on Matter's
	 * identity and stop surviving reboots — silently, and only in the field.
	 */
	if (IS_ENABLED(CONFIG_LOCK_RADIO_MATTER)) {
		bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
		size_t count = ARRAY_SIZE(addrs);

		bt_id_get(addrs, &count);
		LOG_INF("BLE: %u Bluetooth identities, ours is %u", count,
			BT_ID_DEFAULT);
		if (count < 2) {
			LOG_ERR("BLE: expected a second identity for Matter; "
				"bonds may not survive a reboot");
		}
	}

	build_adv_data();

	err = bt_le_ext_adv_create(&adv_param_whitelist, &adv_cbs, &adv_set);
	if (err) {
		LOG_ERR("BLE: adv set create failed: %d", err);
		return err;
	}

	load_and_apply_factory_data();

#ifdef APP_VERSION_STRING
	settings_runtime_set("bt/dis/fw",
			     APP_VERSION_STRING,
			     sizeof(APP_VERSION_STRING) - 1);
#endif

	bt_bas_set_battery_level(battery_get_soc());

	ble_lock_service_init();
	ble_door_service_init();

	if (populate_filter_accept_list()) {
		adv_state = ADV_BONDED;
	} else {
		adv_state = ADV_DISCOVERY;
		k_timer_start(&discovery_timer,
			      K_SECONDS(CONFIG_LOCK_DISCOVERY_TIMEOUT_S),
			      K_NO_WAIT);

		if (app_cbs && app_cbs->on_discovery_changed) {
			app_cbs->on_discovery_changed(true);
		}

		LOG_INF("BLE: no bonds, auto-discovery for %d s",
			CONFIG_LOCK_DISCOVERY_TIMEOUT_S);
	}

	/* Onto the workqueue, where every other advertising change happens. */
	request_advertising_update();

	return 0;
}

static int ble_run(void)
{
	return 0;
}

const struct radio_backend_ops ble_radio_ops = {
	.init = ble_init,
	.run = ble_run,
	.on_lock_state_changed = ble_on_lock_state_changed,
	.on_door_state_changed = ble_on_door_state_changed,
	.on_battery_changed = ble_on_battery_changed,
	.start_discovery = ble_start_discovery,
	.stop_discovery = ble_stop_discovery,
	.factory_reset = ble_factory_reset,
	.factory_reset_reboots = false,
};

int ble_radio_lock_request(enum lock_action action, enum lock_op_source source)
{
	if (app_cbs && app_cbs->lock_request) {
		/* No originator: a BLE peer has no fabric/node identity. */
		return app_cbs->lock_request(action, source, NULL);
	}

	return -ENOTSUP;
}
