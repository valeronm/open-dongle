#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "ble.h"
#include "core.h"
#include "diag.h"
#include "hidkeys.h"
#include "hidmap.h"
#include "indicator.h"

#define TICK_MS 10

static int no_home(void)
{
	return CHANMOVE_NO_HOME;
}

static void nothing(void)
{
}

static void fill_status(struct diag_status *st)
{
	ARG_UNUSED(st);
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

static uint8_t map_bytes[BLE_MAP_MAX];
static struct hidmap map;
static struct hidkeys keys;
static bool ready;

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

	if (!ready || !map.has_leds) {
		return;
	}
	err = ble_set_leds(map.led_report_id, leds);
	if (err) {
		printk("bluetooth leds not sent: %d\n", err);
	}
}

static void on_map_read(void)
{
	char name[BLE_NAME_MAX + 1];
	size_t len = ble_copy_map(map_bytes, sizeof(map_bytes));
	enum hidmap_result result = hidmap_parse(map_bytes, len, &map);
	uint8_t leds;

	ble_copy_name(name);
	if (result != HIDMAP_OK) {
		printk("bluetooth keyboard \"%s\" refused: report map %s\n", name,
		       map_fault(result));
		ble_disconnect();
		return;
	}
	printk("bluetooth keyboard \"%s\" ready\n", name);
	ready = true;
	core_take_leds(&leds);
	send_leds(leds);
}

static void on_disconnected(void)
{
	if (ready) {
		core_release_keys();
	}
	ready = false;
	hidkeys_reset(&keys);
}

static void on_report(const struct ble_event *ev)
{
	struct report out[REPORT_KINDS];
	size_t n;

	if (!ready) {
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
	case BLE_CONNECTED:
		printk("bluetooth connected: %s\n", ev->address);
		break;
	case BLE_DISCONNECTED:
		printk("bluetooth disconnected: reason 0x%02x\n", ev->reason);
		on_disconnected();
		break;
	case BLE_FAILED:
		printk("bluetooth %s failed: %d\n", ble_stage_name(ev->failed.stage),
		       ev->failed.error);
		break;
	case BLE_MAP_READ:
		on_map_read();
		break;
	case BLE_REPORT:
		on_report(ev);
		break;
	}
}

FUNC_NORETURN void bluetooth_run(int store_err)
{
	int err;

	ARG_UNUSED(store_err);
	core_start_history(&loop);
	err = ble_start();
	if (err) {
		printk("bluetooth start failed: %d\n", err);
	} else {
		printk("bluetooth started\n");
	}

	for (;;) {
		struct diag_request req;
		struct ble_event ev;
		uint8_t leds;

		k_msleep(TICK_MS);
		core_poll();
		while (core_take_request(&req)) {
			diag_reply_error(&req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		}
		while (ble_take_event(&ev)) {
			on_event(&ev);
		}
		if (core_take_leds(&leds)) {
			send_leds(leds);
		}
		core_show_state(err ? IND_ERROR : ready ? IND_CONNECTED : IND_IDLE);
		core_watchdog_feed();
	}
}
