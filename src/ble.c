#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/hogp.h>
#include "ble.h"
#include "clock.h"

K_MSGQ_DEFINE(ble_events, sizeof(struct ble_event), 16, 4);

BUILD_ASSERT(CONFIG_BT_MAX_CONN == DEVICE_KINDS);
BUILD_ASSERT(CONFIG_BT_MAX_PAIRED == BTPOLICY_STORED_MAX);
BUILD_ASSERT(BTPOLICY_APPEARANCE_KEYBOARD == BT_APPEARANCE_HID_KEYBOARD);
BUILD_ASSERT(BTPOLICY_APPEARANCE_MOUSE == BT_APPEARANCE_HID_MOUSE);

struct link {
	struct bt_conn *conn;
	struct bt_hogp hogp;
	uint8_t map[BLE_MAP_MAX];
	size_t map_len;
	char name[BLE_NAME_MAX + 1];
	struct bt_gatt_read_params name_read;
	bool failed;
	bool passkey_used;
};

static struct link links[DEVICE_KINDS];
/* A new device's advert that lacked the HID service or a kind, kept for its scan response. */
static struct btpolicy_advert pending;
static bool pending_set;
static enum btpolicy_scan scan_mode;
static uint32_t passkey;
static bool offering;

static enum device_kind kind_of(const struct link *l)
{
	return (enum device_kind)(l - links);
}

static struct link *link_of_conn(const struct bt_conn *conn)
{
	for (int k = 0; k < DEVICE_KINDS; k++) {
		if (links[k].conn == conn) {
			return &links[k];
		}
	}
	return NULL;
}

static void emit(struct link *l, struct ble_event ev)
{
	ev.kind = kind_of(l);
	if (k_msgq_put(&ble_events, &ev, K_NO_WAIT) != 0) {
		printk("bluetooth event dropped\n");
	}
}

static enum btpolicy_failure failure_of(enum ble_stage stage, int error)
{
	if (stage != BLE_STAGE_SECURITY || error == BT_SECURITY_ERR_UNSPECIFIED) {
		return BTPOLICY_FAILURE_LINK;
	}
	return error == BT_SECURITY_ERR_PIN_OR_KEY_MISSING ? BTPOLICY_FAILURE_KEY_MISSING
							   : BTPOLICY_FAILURE_REFUSAL;
}

static void emit_failed(struct link *l, enum ble_stage stage, int error)
{
	emit(l, (struct ble_event){
			.type = BLE_FAILED,
			.failed = {.stage = stage, .error = error, .why = failure_of(stage, error)},
		});
}

static void fail(struct link *l, enum ble_stage stage, int error)
{
	/* The stages still running when one fails, or when the connection is being closed, end in
	 * errors of their own. */
	if (l->failed) {
		return;
	}
	l->failed = true;
	emit_failed(l, stage, error);
	ble_disconnect(kind_of(l));
}

static uint8_t on_report(struct bt_hogp *h, struct bt_hogp_rep_info *rep, uint8_t err,
			 const uint8_t *data)
{
	struct ble_event ev = {
		.type = BLE_REPORT,
		.report = {.received_us = clock_now_us(), .id = bt_hogp_rep_id(rep)},
	};
	size_t len = bt_hogp_rep_size(rep);

	if (err || !data) {
		return BT_GATT_ITER_STOP;
	}
	ev.report.len = MIN(len, sizeof(ev.report.data));
	memcpy(ev.report.data, data, ev.report.len);
	emit(CONTAINER_OF(h, struct link, hogp), ev);
	return BT_GATT_ITER_CONTINUE;
}

/* Each subscription holds an ATT buffer until the device answers, and all are sent at once. */
BUILD_ASSERT(CONFIG_BT_ATT_TX_COUNT >= CONFIG_BT_HOGP_REPORTS_MAX);

static void subscribe(struct link *l)
{
	struct bt_hogp_rep_info *rep = NULL;

	while ((rep = bt_hogp_rep_next(&l->hogp, rep)) != NULL) {
		int err;

		if (bt_hogp_rep_type(rep) != BT_HIDS_REPORT_TYPE_INPUT) {
			continue;
		}
		err = bt_hogp_rep_subscribe(&l->hogp, rep, on_report);
		if (err) {
			fail(l, BLE_STAGE_SUBSCRIBE, err);
			return;
		}
	}
}

static void on_map_part(struct bt_hogp *h, uint8_t err, const uint8_t *data, size_t size,
			size_t offset)
{
	struct link *l = CONTAINER_OF(h, struct link, hogp);
	int ret;

	if (err) {
		fail(l, BLE_STAGE_MAP, err);
		return;
	}
	if (!data || size == 0) {
		l->map_len = offset;
		emit(l, (struct ble_event){.type = BLE_MAP_READ});
		subscribe(l);
		return;
	}
	if (offset + size > sizeof(l->map)) {
		fail(l, BLE_STAGE_MAP, -ENOMEM);
		return;
	}
	memcpy(&l->map[offset], data, size);
	ret = bt_hogp_map_read(h, on_map_part, offset + size, K_NO_WAIT);
	if (ret) {
		fail(l, BLE_STAGE_MAP, ret);
	}
}

static void on_hogp_ready(struct bt_hogp *h)
{
	int err = bt_hogp_map_read(h, on_map_part, 0, K_NO_WAIT);

	if (err) {
		fail(CONTAINER_OF(h, struct link, hogp), BLE_STAGE_MAP, err);
	}
}

static void on_hogp_failed(struct bt_hogp *h, int err)
{
	fail(CONTAINER_OF(h, struct link, hogp), BLE_STAGE_HID_SETUP, err);
}

static const struct bt_hogp_init_params hogp_params = {
	.ready_cb = on_hogp_ready,
	.prep_error_cb = on_hogp_failed,
};

static void on_discovered(struct bt_gatt_dm *dm, void *context)
{
	struct link *l = context;
	int err = bt_hogp_handles_assign(dm, &l->hogp);

	bt_gatt_dm_data_release(dm);
	if (err) {
		fail(l, BLE_STAGE_HID_SETUP, err);
	}
}

static void on_no_hid_service(struct bt_conn *conn, void *context)
{
	ARG_UNUSED(conn);
	fail(context, BLE_STAGE_DISCOVERY, -ENOENT);
}

static void on_discovery_error(struct bt_conn *conn, int err, void *context)
{
	ARG_UNUSED(conn);
	fail(context, BLE_STAGE_DISCOVERY, err);
}

static const struct bt_gatt_dm_cb discovery = {
	.completed = on_discovered,
	.service_not_found = on_no_hid_service,
	.error_found = on_discovery_error,
};

static void discover(struct link *l)
{
	int err = bt_gatt_dm_start(l->conn, BT_UUID_HIDS, &discovery, l);

	if (err) {
		fail(l, BLE_STAGE_DISCOVERY, err);
	}
}

static uint8_t on_name(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
		       const void *data, uint16_t length)
{
	struct link *l = CONTAINER_OF(params, struct link, name_read);

	ARG_UNUSED(conn);
	if (!err && data) {
		size_t len = MIN(length, BLE_NAME_MAX);

		memcpy(l->name, data, len);
		l->name[len] = '\0';
	}
	discover(l);
	return BT_GATT_ITER_STOP;
}

static void read_name(struct link *l)
{
	l->name_read = (struct bt_gatt_read_params){
		.func = on_name,
		.by_uuid = {
			.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE,
			.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE,
			.uuid = BT_UUID_GAP_DEVICE_NAME,
		},
	};
	if (bt_gatt_read(l->conn, &l->name_read) != 0) {
		discover(l);
	}
}

BUILD_ASSERT(BLE_ADDRESS_MAX >= BT_ADDR_LE_STR_LEN);
BUILD_ASSERT(BLE_REASON_NOT_ESTABLISHED == BT_HCI_ERR_CONN_FAIL_TO_ESTAB);
BUILD_ASSERT(sizeof(bt_addr_le_t) == BTPOLICY_ADDR_LEN);

static void connection_ended(struct link *l, uint8_t reason)
{
	if (bt_hogp_assign_check(&l->hogp)) {
		bt_hogp_release(&l->hogp);
	}
	bt_conn_unref(l->conn);
	l->conn = NULL;
	emit(l, (struct ble_event){.type = BLE_DISCONNECTED, .reason = reason});
}

static void print_params(struct link *l)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(l->conn, &info) == 0) {
		printk("bluetooth %s link: interval %u us, latency %u, timeout %u ms\n",
		       device_kind_name(kind_of(l)), info.le.interval_us,
		       info.le.latency, info.le.timeout * 10u);
	}
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct link *l = link_of_conn(conn);
	struct ble_event ev = {.type = BLE_CONNECTED};
	int ret;

	if (!l) {
		return;
	}
	if (err) {
		emit_failed(l, BLE_STAGE_CONNECT, err);
		connection_ended(l, err);
		return;
	}
	l->map_len = 0;
	l->name[0] = '\0';
	bt_addr_le_to_str(bt_conn_get_dst(conn), ev.address, sizeof(ev.address));
	emit(l, ev);
	print_params(l);
	l->passkey_used = false;
	l->failed = false;
	ret = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (ret) {
		fail(l, BLE_STAGE_SECURITY, ret);
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct link *l = link_of_conn(conn);

	if (l) {
		connection_ended(l, reason);
	}
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	struct link *l = link_of_conn(conn);

	ARG_UNUSED(level);
	if (!l) {
		return;
	}
	if (err) {
		fail(l, BLE_STAGE_SECURITY, err);
		return;
	}
	read_name(l);
}

static void on_params_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			      uint16_t timeout)
{
	struct link *l = link_of_conn(conn);

	ARG_UNUSED(interval);
	ARG_UNUSED(latency);
	ARG_UNUSED(timeout);
	if (l) {
		print_params(l);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.security_changed = on_security_changed,
	.le_param_updated = on_params_updated,
};

static void on_pairing_complete(struct bt_conn *conn, bool bonded)
{
	struct link *l = link_of_conn(conn);

	if (l && bonded) {
		emit(l, (struct ble_event){.type = BLE_PAIRED, .passkey_used = l->passkey_used});
	}
}

static struct bt_conn_auth_info_cb pairing_callbacks = {
	.pairing_complete = on_pairing_complete,
};

static bool parse_field(struct bt_data *data, void *user_data)
{
	struct btpolicy_advert *a = user_data;

	if (data->type == BT_DATA_GAP_APPEARANCE && data->data_len == 2 &&
	    a->kind == DEVICE_NO_KIND) {
		a->kind = btpolicy_kind_of(sys_get_le16(data->data));
	}
	if (data->type != BT_DATA_UUID16_SOME && data->type != BT_DATA_UUID16_ALL) {
		return true;
	}
	for (size_t i = 0; i + 1 < data->data_len; i += 2) {
		if (sys_get_le16(&data->data[i]) == BT_UUID_HIDS_VAL) {
			a->hid_service = true;
		}
	}
	return true;
}

static void on_advert(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		      struct net_buf_simple *data)
{
	struct ble_event ev = {.type = BLE_ADVERT};
	struct btpolicy_advert *a = &ev.advert;
	bool response = type == BT_GAP_ADV_TYPE_SCAN_RSP;

	ARG_UNUSED(rssi);
	if (response) {
		if (!pending_set || memcmp(pending.addr, addr, sizeof(pending.addr)) != 0) {
			return;
		}
		*a = pending;
		pending_set = false;
	} else {
		a->directed = type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND;
		a->connectable = a->directed || type == BT_GAP_ADV_TYPE_ADV_IND;
		a->stored = bt_le_bond_exists(BT_ID_DEFAULT, addr);
		a->kind = DEVICE_NO_KIND;
		memcpy(a->addr, addr, sizeof(a->addr));
	}
	if (!a->directed) {
		bt_data_parse(data, parse_field, a);
	}
	/* Adverts from other devices would fill the event queue. */
	if (!btpolicy_interesting(a)) {
		if (!response && scan_mode == BTPOLICY_SCAN_ACTIVE && a->connectable &&
		    !a->directed) {
			pending = *a;
			pending_set = true;
		}
		return;
	}
	k_msgq_put(&ble_events, &ev, K_NO_WAIT);
}

int ble_scan(enum btpolicy_scan mode)
{
	bool pairing;
	int err;

	if (mode == scan_mode) {
		return 0;
	}
	if (scan_mode != BTPOLICY_SCAN_OFF) {
		err = bt_le_scan_stop();
		if (err) {
			return err;
		}
		scan_mode = BTPOLICY_SCAN_OFF;
	}
	if (mode == BTPOLICY_SCAN_OFF) {
		return 0;
	}
	pending_set = false;
	pairing = mode == BTPOLICY_SCAN_ACTIVE;
	/* The fast scan window takes radio time from a connected mouse, about an eighth of its
	 * reports. */
	err = bt_le_scan_start(
		BT_LE_SCAN_PARAM(pairing ? BT_LE_SCAN_TYPE_ACTIVE : BT_LE_SCAN_TYPE_PASSIVE,
				 BT_LE_SCAN_OPT_NONE,
				 pairing ? BT_GAP_SCAN_FAST_INTERVAL : BT_GAP_SCAN_SLOW_INTERVAL_1,
				 pairing ? BT_GAP_SCAN_FAST_WINDOW : BT_GAP_SCAN_SLOW_WINDOW_1),
		on_advert);
	if (!err) {
		scan_mode = mode;
	}
	return err;
}

/* Reading a device's services takes dozens of exchanges, each two connection intervals long. */
#define SHORTEST_INTERVAL 6 /* 7.5 ms */
#define SETUP_CONN_PARAM                                                                           \
	BT_LE_CONN_PARAM(SHORTEST_INTERVAL, SHORTEST_INTERVAL, 0, BT_GAP_MS_TO_CONN_TIMEOUT(4000))

int ble_connect(enum device_kind kind, const uint8_t addr[BTPOLICY_ADDR_LEN])
{
	struct link *l = &links[kind];
	bt_addr_le_t peer;
	int err;

	if (l->conn) {
		return -EALREADY;
	}
	memcpy(&peer, addr, sizeof(peer));
	err = bt_conn_le_create(&peer, BT_CONN_LE_CREATE_CONN, SETUP_CONN_PARAM, &l->conn);
	if (err) {
		l->conn = NULL;
	}
	return err;
}

static uint32_t on_app_passkey(struct bt_conn *conn)
{
	struct link *l = link_of_conn(conn);

	if (l) {
		l->passkey_used = true;
	}
	return passkey;
}

static void on_pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
}

static const struct bt_conn_auth_cb passkey_callbacks = {
	.app_passkey = on_app_passkey,
	.cancel = on_pairing_cancel,
};

int ble_offer_passkey(bool on, uint32_t value)
{
	int err;

	passkey = value;
	if (on == offering) {
		return 0;
	}
	err = bt_conn_auth_cb_register(on ? &passkey_callbacks : NULL);
	if (!err) {
		offering = on;
	}
	return err;
}

struct bonds {
	uint8_t (*addrs)[BTPOLICY_ADDR_LEN];
	uint8_t cap;
	uint8_t count;
};

static void add_bond(const struct bt_bond_info *info, void *user_data)
{
	struct bonds *bonds = user_data;

	if (bonds->count < bonds->cap) {
		memcpy(bonds->addrs[bonds->count], &info->addr, BTPOLICY_ADDR_LEN);
	}
	bonds->count++;
}

uint8_t ble_stored(uint8_t addrs[][BTPOLICY_ADDR_LEN], uint8_t cap)
{
	struct bonds bonds = {.addrs = addrs, .cap = cap};

	bt_foreach_bond(BT_ID_DEFAULT, add_bond, &bonds);
	return bonds.count;
}

uint8_t ble_stored_count(void)
{
	return ble_stored(NULL, 0);
}

int ble_forget(const uint8_t addr[BTPOLICY_ADDR_LEN])
{
	bt_addr_le_t peer;

	memcpy(&peer, addr, sizeof(peer));
	return bt_unpair(BT_ID_DEFAULT, &peer);
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
	for (int k = 0; !err && k < DEVICE_KINDS; k++) {
		bt_hogp_init(&links[k].hogp, &hogp_params);
	}
	return err;
}

bool ble_take_event(struct ble_event *ev, k_timeout_t wait)
{
	return k_msgq_get(&ble_events, ev, wait) == 0;
}

size_t ble_copy_map(enum device_kind kind, uint8_t *out, size_t cap)
{
	size_t len;

	k_sched_lock();
	len = MIN(links[kind].map_len, cap);
	memcpy(out, links[kind].map, len);
	k_sched_unlock();
	return len;
}

void ble_copy_name(enum device_kind kind, char out[BLE_NAME_MAX + 1])
{
	k_sched_lock();
	memcpy(out, links[kind].name, sizeof(links[kind].name));
	k_sched_unlock();
}

void ble_disconnect(enum device_kind kind)
{
	struct link *l = &links[kind];

	l->failed = true;
	if (l->conn) {
		bt_conn_disconnect(l->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
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
	struct bt_hogp *hogp = &links[DEVICE_KEYBOARD].hogp;
	struct bt_hogp_rep_info *rep;

	if (!bt_hogp_ready_check(hogp)) {
		return -ENOTCONN;
	}
	rep = bt_hogp_rep_find(hogp, BT_HIDS_REPORT_TYPE_OUTPUT, report_id);
	if (!rep) {
		return -ENOENT;
	}
	/* The library refuses a write without a completion callback. */
	return bt_hogp_rep_write_wo_rsp(hogp, rep, &leds, sizeof(leds), on_leds_written);
}

int ble_rssi(enum device_kind kind, int8_t *dbm)
{
	struct bt_conn *conn = links[kind].conn;
	struct bt_hci_cp_read_rssi *cp;
	struct net_buf *buf;
	struct net_buf *rsp;
	uint16_t handle;
	int8_t rssi;
	int err;

	if (!conn || bt_hci_get_conn_handle(conn, &handle)) {
		return -ENOTCONN;
	}
	buf = bt_hci_cmd_alloc(K_MSEC(10));
	if (!buf) {
		return -ENOBUFS;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp);
	if (err) {
		return err;
	}
	rssi = ((struct bt_hci_rp_read_rssi *)rsp->data)->rssi;
	net_buf_unref(rsp);
	if (rssi == BT_HCI_LE_RSSI_NOT_AVAILABLE) {
		return -ENODATA;
	}
	*dbm = rssi;
	return 0;
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
