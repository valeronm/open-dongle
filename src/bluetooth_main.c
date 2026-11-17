#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include "ble.h"
#include "btlist.h"
#include "btpolicy.h"
#include "core.h"
#include "diag.h"
#include "hidkeys.h"
#include "hidmap.h"
#include "indicator.h"
#include "pairwin.h"
#include "store.h"

#define TICK_MS 10
#define RSSI_PERIOD_MS 1000

static int no_home(void)
{
	return CHANMOVE_NO_HOME;
}

static void nothing(void)
{
}

enum {
	LINK_NO_KEYBOARD,
	LINK_CONNECTED,
	LINK_START_FAILED,
};

static uint8_t map_bytes[BLE_MAP_MAX];
static struct hidmap map;
static struct hidkeys keys;
static struct btpolicy policy;
static struct btlist list;
static struct pairwin window;
static char name[BLE_NAME_MAX + 1];
static int start_err;
static bool passkey_asked;
static bool rssi_read;
static int8_t rssi;
static uint32_t rssi_tried_ms;
static bool reported;
static uint32_t report_ms;

static void fill_status(struct diag_status *st)
{
	st->link_state = start_err			? LINK_START_FAILED
			 : btpolicy_is_ready(&policy)	? LINK_CONNECTED
							: LINK_NO_KEYBOARD;
	st->pairing_open = pairwin_is_open(&window, k_uptime_get_32());
	if (rssi_read) {
		st->last_rssi = rssi;
	}
	if (reported) {
		st->last_rx_age_ms = k_uptime_get_32() - report_ms;
	}
	if (btpolicy_is_ready(&policy)) {
		strncpy((char *)st->keyboard_name, name, sizeof(st->keyboard_name));
	}
}

static size_t no_candidates(const uint8_t **channels)
{
	static const uint8_t none[1];

	*channels = none;
	return 0;
}

static enum chanmove_result no_move(uint8_t channel, uint8_t *from)
{
	ARG_UNUSED(channel);
	*from = 0;
	return CHANMOVE_NOT_MOVABLE;
}

static const struct core_loop loop = {
	.home = no_home,
	.radio_resumed = nothing,
	.fill_status = fill_status,
	.movable = no_home,
	.candidates = no_candidates,
	.replies_dropped = nothing,
	.move = no_move,
};

static const char *map_fault(enum hidmap_result result)
{
	switch (result) {
	case HIDMAP_MALFORMED:
		return "malformed";
	case HIDMAP_TOO_LARGE:
		return "too large";
	case HIDMAP_NO_KEYBOARD:
		return "without a keyboard";
	case HIDMAP_OK:
		break;
	}
	return "usable";
}

static void save_list(bool changed)
{
	if (changed && store_save_bluetooth(&list, sizeof(list)) != 0) {
		printk("bluetooth keyboard list not saved\n");
	}
}

/* Bonds decide which keyboards are listed; the list adds their names, passkey mark and order. */
static void load_list(void)
{
	uint8_t bonded[BTPOLICY_STORED_MAX][BTPOLICY_ADDR_LEN];
	uint8_t n = ble_stored(bonded, BTPOLICY_STORED_MAX);

	if (!store_load_bluetooth(&list, sizeof(list)) || list.count > BTPOLICY_STORED_MAX) {
		list = (struct btlist){0};
	}
	save_list(btlist_reconcile(&list, bonded, MIN(n, BTPOLICY_STORED_MAX)));
}

static int forget(const uint8_t *addr)
{
	int err = ble_forget(addr);

	if (err) {
		printk("bluetooth bond not deleted: %d\n", err);
		return err;
	}
	save_list(btlist_remove(&list, addr));
	return 0;
}

BUILD_ASSERT(HISTORY_ADDR_LEN == BTPOLICY_ADDR_LEN);
BUILD_ASSERT(DIAG_REQUEST_DATA_LEN == BTPOLICY_ADDR_LEN);

static void log_keyboard(uint8_t type, const uint8_t *addr, uint8_t c)
{
	struct history_event e = history_keyboard_event(type, addr, c);

	core_log(e.type, e.a, e.b, e.c, e.value);
}

static void send_leds(uint8_t leds)
{
	int err;

	if (!btpolicy_is_ready(&policy) || !map.has_leds) {
		return;
	}
	err = ble_set_leds(map.led_report_id, leds);
	if (err) {
		printk("bluetooth leds not sent: %d\n", err);
	}
}

static void on_advert(const struct btpolicy_advert *a)
{
	uint32_t now = k_uptime_get_32();
	int err;

	if (!btpolicy_wants(&policy, a, pairwin_is_open(&window, now), ble_stored_count(), now)) {
		return;
	}
	ble_scan(false);
	err = ble_connect(a->addr);
	if (err) {
		printk("bluetooth connect not started: %d\n", err);
		return;
	}
	btpolicy_connecting(&policy, a);
}

static void on_failed(const struct ble_event *ev)
{
	printk("bluetooth %s failed: %d\n", ble_stage_name(ev->failed.stage), ev->failed.error);
	if (ev->failed.stage != BLE_STAGE_SECURITY) {
		return;
	}
	switch (btpolicy_security_failed(&policy, ev->failed.why)) {
	case BTPOLICY_PAIRING_REFUSED:
		printk("bluetooth pairing refused\n");
		core_log(HISTORY_BT_REFUSED, HISTORY_REFUSED_BY_KEYBOARD, 0, 0, 0);
		break;
	case BTPOLICY_BOND_LOST:
		printk("bluetooth keyboard lost its bond\n");
		log_keyboard(HISTORY_BT_BOND_LOST, btpolicy_peer(&policy), 0);
		break;
	case BTPOLICY_REFUSED_BEFORE:
	case BTPOLICY_SECURITY_FAILED:
		break;
	}
}

static void on_map_read(void)
{
	size_t len = ble_copy_map(map_bytes, sizeof(map_bytes));
	enum hidmap_result result = hidmap_parse(map_bytes, len, &map);
	uint8_t leds;

	ble_copy_name(name);
	if (result != HIDMAP_OK) {
		printk("bluetooth keyboard \"%s\" refused: report map %s\n", name,
		       map_fault(result));
		if (result == HIDMAP_NO_KEYBOARD) {
			forget(btpolicy_peer(&policy));
		}
		ble_disconnect();
		return;
	}
	printk("bluetooth keyboard \"%s\" ready\n", name);
	btpolicy_ready(&policy);
	log_keyboard(HISTORY_BT_CONNECTED, btpolicy_peer(&policy), 0);
	save_list(btlist_connected(&list, btpolicy_peer(&policy), name));
	core_take_leds(&leds);
	send_leds(leds);
}

static void on_disconnected(uint8_t reason)
{
	if (btpolicy_is_ready(&policy)) {
		core_log(HISTORY_BT_DISCONNECTED, reason, core_minutes_since_key(), 0, 0);
		core_release_keys();
	}
	btpolicy_disconnected(&policy, k_uptime_get_32(), reason == BLE_REASON_NOT_ESTABLISHED);
	hidkeys_reset(&keys);
	rssi_read = false;
}

static void on_report(const struct ble_event *ev)
{
	struct report out[REPORT_KINDS];
	size_t n;

	if (!btpolicy_is_ready(&policy)) {
		return;
	}
	core_count(CORE_PACKETS);
	reported = true;
	report_ms = k_uptime_get_32();
	n = hidkeys_translate(&keys, &map, ev->report.id, ev->report.data, ev->report.len, out);
	for (size_t i = 0; i < n; i++) {
		core_submit(&out[i]);
	}
}

static void on_event(const struct ble_event *ev)
{
	switch (ev->type) {
	case BLE_ADVERT:
		on_advert(&ev->advert);
		break;
	case BLE_CONNECTED:
		printk("bluetooth connected: %s\n", ev->address);
		btpolicy_connected(&policy);
		break;
	case BLE_DISCONNECTED:
		printk("bluetooth disconnected: reason 0x%02x\n", ev->reason);
		on_disconnected(ev->reason);
		break;
	case BLE_FAILED:
		on_failed(ev);
		break;
	case BLE_PAIRED:
		if (btpolicy_paired(&policy, ev->passkey_used) == BTPOLICY_PAIRING_FORGOTTEN) {
			printk("bluetooth pairing refused: no passkey typed\n");
			core_log(HISTORY_BT_REFUSED, HISTORY_REFUSED_NO_PASSKEY, 0, 0, 0);
			forget(btpolicy_peer(&policy));
			ble_disconnect();
			break;
		}
		printk("bluetooth paired\n");
		log_keyboard(HISTORY_BT_PAIRED, btpolicy_peer(&policy), ev->passkey_used);
		save_list(btlist_paired(&list, btpolicy_peer(&policy), !ev->passkey_used));
		pairwin_paired(&window);
		break;
	case BLE_MAP_READ:
		on_map_read();
		break;
	case BLE_REPORT:
		on_report(ev);
		break;
	}
}

#define PASSKEY_LIMIT 1000000

static void open_pairing(const struct diag_request *req)
{
	bool with_passkey = (req->arg & DIAG_PAIR_NO_PASSKEY) == 0;
	uint8_t reply[DIAG_PAIR_REPLY_LEN] = {with_passkey};
	uint32_t passkey = 0;
	int err;

	if (start_err) {
		diag_reply_error(req, DIAG_ERR_RADIO, 0);
		return;
	}
	if (ble_stored_count() >= BTPOLICY_STORED_MAX) {
		diag_reply_error(req, DIAG_ERR_LIST_FULL, 0);
		return;
	}
	if (with_passkey) {
		sys_csrand_get(&passkey, sizeof(passkey));
		passkey %= PASSKEY_LIMIT;
	}
	err = ble_offer_passkey(with_passkey, passkey);
	if (err) {
		printk("bluetooth passkey not offered: %d\n", err);
		diag_reply_error(req, DIAG_ERR_RADIO, 0);
		return;
	}
	passkey_asked = with_passkey;
	pairwin_open(&window, PAIRWIN_REQUEST, true, k_uptime_get_32());
	core_log_pairing_opened(&window);
	printk("pairing open for %u s\n", PAIRWIN_MS / 1000);
	ble_disconnect();
	sys_put_le32(passkey, &reply[1]);
	diag_reply(req, reply, sizeof(reply), DIAG_AFTER_NONE);
}

static void send_keyboards(const struct diag_request *req)
{
	uint8_t payload[DIAG_KEYBOARDS_MAX];
	const uint8_t *connected = btpolicy_is_ready(&policy) ? btpolicy_peer(&policy) : NULL;

	diag_reply(req, payload, diag_encode_keyboards(payload, &list, connected),
		   DIAG_AFTER_NONE);
}

static void forget_listed(const struct diag_request *req)
{
	if (!btlist_has(&list, req->data)) {
		diag_reply_error(req, DIAG_ERR_NO_SUCH_KEYBOARD, 0);
		return;
	}
	if (forget(req->data) != 0) {
		diag_reply_error(req, DIAG_ERR_STORE, 0);
		return;
	}
	diag_reply(req, NULL, 0, DIAG_AFTER_NONE);
}

static void on_request(const struct diag_request *req)
{
	switch (req->command) {
	case DIAG_PAIR:
		open_pairing(req);
		break;
	case DIAG_KEYBOARDS:
		send_keyboards(req);
		break;
	case DIAG_FORGET:
		forget_listed(req);
		break;
	default:
		diag_reply_error(req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		break;
	}
}

static void read_rssi(uint32_t now)
{
	int8_t dbm;

	if (!btpolicy_is_ready(&policy) || now - rssi_tried_ms < RSSI_PERIOD_MS) {
		return;
	}
	rssi_tried_ms = now;
	if (ble_rssi(&dbm) == 0) {
		rssi = dbm;
		rssi_read = true;
		core_rssi(dbm);
	}
}

static enum indicator_mode indicator(uint32_t now)
{
	if (start_err) {
		return IND_ERROR;
	}
	if (btpolicy_is_ready(&policy)) {
		return IND_CONNECTED;
	}
	return pairwin_is_open(&window, now) ? IND_PAIRING : IND_IDLE;
}

FUNC_NORETURN void bluetooth_run(int store_err)
{
	ARG_UNUSED(store_err);
	core_start_history(&loop);
	btpolicy_init(&policy);
	start_err = ble_start();
	if (start_err) {
		printk("bluetooth start failed: %d\n", start_err);
	} else {
		uint8_t stored = ble_stored_count();

		load_list();
		printk("bluetooth started, %u stored\n", stored);
		if (stored == 0) {
			pairwin_open(&window, PAIRWIN_BOOT, false, k_uptime_get_32());
			core_log_pairing_opened(&window);
		}
	}

	for (;;) {
		struct diag_request req;
		struct ble_event ev;
		uint32_t now;
		uint8_t leds;

		k_msleep(TICK_MS);
		core_poll();
		while (core_take_request(&req)) {
			on_request(&req);
		}
		while (ble_take_event(&ev)) {
			on_event(&ev);
		}
		now = k_uptime_get_32();
		if (pairwin_ran_out(&window, now)) {
			printk("pairing closed\n");
			core_log(HISTORY_PAIRING_CLOSED, 0, 0, 0, 0);
		}
		if (!pairwin_is_open(&window, now)) {
			passkey_asked = false;
			ble_offer_passkey(false, 0);
		}
		read_rssi(now);
		btpolicy_window(&policy, pairwin_is_open(&window, now), passkey_asked);
		if (start_err == 0) {
			int err = ble_scan(btpolicy_scanning(&policy));

			if (err) {
				printk("bluetooth scan not switched: %d\n", err);
			}
		}
		if (core_take_leds(&leds)) {
			send_leds(leds);
		}
		core_show_state(indicator(now));
		core_watchdog_feed();
	}
}
