#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/hogp.h>
#include "ble.h"

K_MSGQ_DEFINE(ble_events, sizeof(struct ble_event), 16, 4);

static struct bt_conn *keyboard;
static struct bt_hogp hogp;
static uint8_t map[BLE_MAP_MAX];
static size_t map_len;
static char name[BLE_NAME_MAX + 1];
static struct bt_gatt_read_params name_read;
static bool scanning;

static void emit(const struct ble_event *ev)
{
	if (k_msgq_put(&ble_events, ev, K_NO_WAIT) != 0) {
		printk("bluetooth event dropped\n");
	}
}

static void emit_failed(enum ble_stage stage, int error)
{
	struct ble_event ev = {.type = BLE_FAILED, .failed = {.stage = stage, .error = error}};

	emit(&ev);
}

static void fail(enum ble_stage stage, int error)
{
	emit_failed(stage, error);
	ble_disconnect();
}

static uint8_t on_report(struct bt_hogp *h, struct bt_hogp_rep_info *rep, uint8_t err,
			 const uint8_t *data)
{
	struct ble_event ev = {.type = BLE_REPORT, .report = {.id = bt_hogp_rep_id(rep)}};
	size_t len = bt_hogp_rep_size(rep);

	ARG_UNUSED(h);
	if (err || !data) {
		return BT_GATT_ITER_STOP;
	}
	ev.report.len = MIN(len, sizeof(ev.report.data));
	memcpy(ev.report.data, data, ev.report.len);
	emit(&ev);
	return BT_GATT_ITER_CONTINUE;
}

/* Each subscription holds an ATT buffer until the keyboard answers, and all are sent at once. */
BUILD_ASSERT(CONFIG_BT_ATT_TX_COUNT >= CONFIG_BT_HOGP_REPORTS_MAX);

static void subscribe(void)
{
	struct bt_hogp_rep_info *rep = NULL;

	while ((rep = bt_hogp_rep_next(&hogp, rep)) != NULL) {
		int err;

		if (bt_hogp_rep_type(rep) != BT_HIDS_REPORT_TYPE_INPUT) {
			continue;
		}
		err = bt_hogp_rep_subscribe(&hogp, rep, on_report);
		if (err) {
			fail(BLE_STAGE_SUBSCRIBE, err);
			return;
		}
	}
}

static void on_map_part(struct bt_hogp *h, uint8_t err, const uint8_t *data, size_t size,
			size_t offset)
{
	int ret;

	if (err) {
		fail(BLE_STAGE_MAP, err);
		return;
	}
	if (!data || size == 0) {
		map_len = offset;
		emit(&(struct ble_event){.type = BLE_MAP_READ});
		subscribe();
		return;
	}
	if (offset + size > sizeof(map)) {
		fail(BLE_STAGE_MAP, -ENOMEM);
		return;
	}
	memcpy(&map[offset], data, size);
	ret = bt_hogp_map_read(h, on_map_part, offset + size, K_NO_WAIT);
	if (ret) {
		fail(BLE_STAGE_MAP, ret);
	}
}

static void on_hogp_ready(struct bt_hogp *h)
{
	int err = bt_hogp_map_read(h, on_map_part, 0, K_NO_WAIT);

	if (err) {
		fail(BLE_STAGE_MAP, err);
	}
}

static void on_hogp_failed(struct bt_hogp *h, int err)
{
	ARG_UNUSED(h);
	fail(BLE_STAGE_HID_SETUP, err);
}

static const struct bt_hogp_init_params hogp_params = {
	.ready_cb = on_hogp_ready,
	.prep_error_cb = on_hogp_failed,
};

static void on_discovered(struct bt_gatt_dm *dm, void *context)
{
	int err = bt_hogp_handles_assign(dm, &hogp);

	ARG_UNUSED(context);
	bt_gatt_dm_data_release(dm);
	if (err) {
		fail(BLE_STAGE_HID_SETUP, err);
	}
}

static void on_no_hid_service(struct bt_conn *conn, void *context)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(context);
	fail(BLE_STAGE_DISCOVERY, -ENOENT);
}

static void on_discovery_error(struct bt_conn *conn, int err, void *context)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(context);
	fail(BLE_STAGE_DISCOVERY, err);
}

static const struct bt_gatt_dm_cb discovery = {
	.completed = on_discovered,
	.service_not_found = on_no_hid_service,
	.error_found = on_discovery_error,
};

static void discover(struct bt_conn *conn)
{
	int err = bt_gatt_dm_start(conn, BT_UUID_HIDS, &discovery, NULL);

	if (err) {
		fail(BLE_STAGE_DISCOVERY, err);
	}
}

static uint8_t on_name(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
		       const void *data, uint16_t length)
{
	ARG_UNUSED(params);
	if (!err && data) {
		size_t len = MIN(length, BLE_NAME_MAX);

		memcpy(name, data, len);
		name[len] = '\0';
	}
	discover(conn);
	return BT_GATT_ITER_STOP;
}

static void read_name(struct bt_conn *conn)
{
	name_read = (struct bt_gatt_read_params){
		.func = on_name,
		.by_uuid = {
			.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE,
			.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE,
			.uuid = BT_UUID_GAP_DEVICE_NAME,
		},
	};
	if (bt_gatt_read(conn, &name_read) != 0) {
		discover(conn);
	}
}

BUILD_ASSERT(BLE_ADDRESS_MAX >= BT_ADDR_LE_STR_LEN);
BUILD_ASSERT(sizeof(bt_addr_le_t) == BTPOLICY_ADDR_LEN);
BUILD_ASSERT(BLE_SECURITY_KEY_MISSING == BT_SECURITY_ERR_PIN_OR_KEY_MISSING);

static void connection_ended(uint8_t reason)
{
	if (bt_hogp_assign_check(&hogp)) {
		bt_hogp_release(&hogp);
	}
	bt_conn_unref(keyboard);
	keyboard = NULL;
	emit(&(struct ble_event){.type = BLE_DISCONNECTED, .reason = reason});
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct ble_event ev = {.type = BLE_CONNECTED};
	int ret;

	if (conn != keyboard) {
		return;
	}
	if (err) {
		emit_failed(BLE_STAGE_CONNECT, err);
		connection_ended(err);
		return;
	}
	map_len = 0;
	name[0] = '\0';
	bt_addr_le_to_str(bt_conn_get_dst(conn), ev.address, sizeof(ev.address));
	emit(&ev);
	ret = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (ret) {
		fail(BLE_STAGE_SECURITY, ret);
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (conn == keyboard) {
		connection_ended(reason);
	}
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	ARG_UNUSED(level);
	if (conn != keyboard) {
		return;
	}
	if (err) {
		fail(BLE_STAGE_SECURITY, err);
		return;
	}
	read_name(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.security_changed = on_security_changed,
};

static void on_pairing_complete(struct bt_conn *conn, bool bonded)
{
	if (conn == keyboard && bonded) {
		emit(&(struct ble_event){.type = BLE_PAIRED});
	}
}

static struct bt_conn_auth_info_cb pairing_callbacks = {
	.pairing_complete = on_pairing_complete,
};

static bool is_hid_uuid(struct bt_data *data, void *user_data)
{
	bool *hid = user_data;

	if (data->type != BT_DATA_UUID16_SOME && data->type != BT_DATA_UUID16_ALL) {
		return true;
	}
	for (size_t i = 0; i + 1 < data->data_len; i += 2) {
		if (sys_get_le16(&data->data[i]) == BT_UUID_HIDS_VAL) {
			*hid = true;
			return false;
		}
	}
	return true;
}

static void on_advert(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		      struct net_buf_simple *data)
{
	struct ble_event ev = {.type = BLE_ADVERT};
	struct btpolicy_advert *a = &ev.advert;

	ARG_UNUSED(rssi);
	a->directed = type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND;
	a->connectable = a->directed || type == BT_GAP_ADV_TYPE_ADV_IND;
	a->stored = bt_le_bond_exists(BT_ID_DEFAULT, addr);
	if (!a->directed) {
		bt_data_parse(data, is_hid_uuid, &a->hid_service);
	}
	/* Adverts from other devices would fill the event queue. */
	if (!btpolicy_interesting(a)) {
		return;
	}
	memcpy(a->addr, addr, sizeof(a->addr));
	k_msgq_put(&ble_events, &ev, K_NO_WAIT);
}

int ble_scan(bool on)
{
	int err;

	if (on == scanning) {
		return 0;
	}
	if (on) {
		err = bt_le_scan_start(BT_LE_SCAN_PARAM(BT_LE_SCAN_TYPE_PASSIVE,
							BT_LE_SCAN_OPT_NONE,
							BT_GAP_SCAN_FAST_INTERVAL,
							BT_GAP_SCAN_FAST_WINDOW),
				       on_advert);
	} else {
		err = bt_le_scan_stop();
	}
	if (!err) {
		scanning = on;
	}
	return err;
}

/* Reading a keyboard's services takes dozens of exchanges, each two connection intervals long. */
#define SHORTEST_INTERVAL 6 /* 7.5 ms */
#define SETUP_CONN_PARAM                                                                           \
	BT_LE_CONN_PARAM(SHORTEST_INTERVAL, SHORTEST_INTERVAL, 0, BT_GAP_MS_TO_CONN_TIMEOUT(4000))

int ble_connect(const uint8_t addr[BTPOLICY_ADDR_LEN])
{
	bt_addr_le_t peer;
	int err;

	if (keyboard) {
		return -EALREADY;
	}
	memcpy(&peer, addr, sizeof(peer));
	err = bt_conn_le_create(&peer, BT_CONN_LE_CREATE_CONN, SETUP_CONN_PARAM, &keyboard);
	if (err) {
		keyboard = NULL;
	}
	return err;
}

static void count_bond(const struct bt_bond_info *info, void *user_data)
{
	uint8_t *count = user_data;

	ARG_UNUSED(info);
	(*count)++;
}

uint8_t ble_stored_count(void)
{
	uint8_t count = 0;

	bt_foreach_bond(BT_ID_DEFAULT, count_bond, &count);
	return count;
}

int ble_forget_connected(void)
{
	if (!keyboard) {
		return -ENOTCONN;
	}
	return bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(keyboard));
}

int ble_start(void)
{
	int err = bt_enable(NULL);

	if (!err) {
		err = settings_load();
	}
	if (!err) {
		err = bt_conn_auth_info_cb_register(&pairing_callbacks);
	}
	if (!err) {
		bt_hogp_init(&hogp, &hogp_params);
	}
	return err;
}

bool ble_take_event(struct ble_event *ev)
{
	return k_msgq_get(&ble_events, ev, K_NO_WAIT) == 0;
}

size_t ble_copy_map(uint8_t *out, size_t cap)
{
	size_t len;

	k_sched_lock();
	len = MIN(map_len, cap);
	memcpy(out, map, len);
	k_sched_unlock();
	return len;
}

void ble_copy_name(char out[BLE_NAME_MAX + 1])
{
	k_sched_lock();
	memcpy(out, name, sizeof(name));
	k_sched_unlock();
}

void ble_disconnect(void)
{
	if (keyboard) {
		bt_conn_disconnect(keyboard, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void on_leds_written(struct bt_hogp *h, struct bt_hogp_rep_info *rep, uint8_t err)
{
	ARG_UNUSED(h);
	ARG_UNUSED(rep);
	ARG_UNUSED(err);
}

int ble_set_leds(uint8_t report_id, uint8_t leds)
{
	struct bt_hogp_rep_info *rep;

	if (!bt_hogp_ready_check(&hogp)) {
		return -ENOTCONN;
	}
	rep = bt_hogp_rep_find(&hogp, BT_HIDS_REPORT_TYPE_OUTPUT, report_id);
	if (!rep) {
		return -ENOENT;
	}
	/* The library refuses a write without a completion callback. */
	return bt_hogp_rep_write_wo_rsp(&hogp, rep, &leds, sizeof(leds), on_leds_written);
}

const char *ble_stage_name(enum ble_stage stage)
{
	switch (stage) {
	case BLE_STAGE_CONNECT:
		return "connect";
	case BLE_STAGE_SECURITY:
		return "security";
	case BLE_STAGE_DISCOVERY:
		return "discovery";
	case BLE_STAGE_HID_SETUP:
		return "hid setup";
	case BLE_STAGE_MAP:
		return "report map";
	case BLE_STAGE_SUBSCRIBE:
		return "subscribe";
	}
	return "unknown";
}
