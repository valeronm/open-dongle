#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include "ble.h"
#include "btpolicy.h"
#include "core.h"
#include "diag.h"
#include "hidkeys.h"
#include "hidmap.h"
#include "indicator.h"
#include "pairwin.h"

#define TICK_MS 10

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
static struct pairwin window;
static char name[BLE_NAME_MAX + 1];
static int start_err;
static bool passkey_asked;

static void fill_status(struct diag_status *st)
{
	st->link_state = start_err			? LINK_START_FAILED
			 : btpolicy_is_ready(&policy)	? LINK_CONNECTED
							: LINK_NO_KEYBOARD;
	st->pairing_open = pairwin_is_open(&window, k_uptime_get_32());
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
		break;
	case BTPOLICY_BOND_LOST:
		printk("bluetooth keyboard lost its bond\n");
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
			ble_forget_connected();
		}
		ble_disconnect();
		return;
	}
	printk("bluetooth keyboard \"%s\" ready\n", name);
	btpolicy_ready(&policy);
	core_take_leds(&leds);
	send_leds(leds);
}

static void on_disconnected(void)
{
	if (btpolicy_is_ready(&policy)) {
		core_release_keys();
	}
	btpolicy_disconnected(&policy, k_uptime_get_32());
	hidkeys_reset(&keys);
}

static void on_report(const struct ble_event *ev)
{
	struct report out[REPORT_KINDS];
	size_t n;

	if (!btpolicy_is_ready(&policy)) {
		return;
	}
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
		on_disconnected();
		break;
	case BLE_FAILED:
		on_failed(ev);
		break;
	case BLE_PAIRED:
		if (btpolicy_paired(&policy, ev->passkey_used) == BTPOLICY_PAIRING_FORGOTTEN) {
			printk("bluetooth pairing refused: no passkey typed\n");
			ble_forget_connected();
			ble_disconnect();
			break;
		}
		printk("bluetooth paired\n");
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

static void on_request(const struct diag_request *req)
{
	switch (req->command) {
	case DIAG_PAIR:
		open_pairing(req);
		break;
	default:
		diag_reply_error(req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		break;
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
		}
		if (!pairwin_is_open(&window, now)) {
			passkey_asked = false;
			ble_offer_passkey(false, 0);
		}
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
