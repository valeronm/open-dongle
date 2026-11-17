#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "ble.h"
#include "core.h"
#include "diag.h"
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

static void print_hex(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		printk("%02x%s", data[i], i % 32 == 31 ? "\n" : "");
	}
	if (len % 32 != 0 || len == 0) {
		printk("\n");
	}
}

static void print_event(const struct ble_event *ev)
{
	const uint8_t *map;
	size_t len;

	switch (ev->type) {
	case BLE_CONNECTED:
		printk("bluetooth connected: %s\n", ev->address);
		break;
	case BLE_DISCONNECTED:
		printk("bluetooth disconnected: reason 0x%02x\n", ev->reason);
		break;
	case BLE_FAILED:
		printk("bluetooth %s failed: %d\n", ble_stage_name(ev->failed.stage),
		       ev->failed.error);
		break;
	case BLE_MAP_READ:
		len = ble_report_map(&map);
		printk("bluetooth keyboard \"%s\", report map %u bytes:\n", ble_name(),
		       (unsigned int)len);
		print_hex(map, len);
		break;
	case BLE_REPORT:
		printk("bluetooth report %u: ", ev->report.id);
		print_hex(ev->report.data, ev->report.len);
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

		k_msleep(TICK_MS);
		core_poll();
		while (core_take_request(&req)) {
			diag_reply_error(&req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		}
		while (ble_take_event(&ev)) {
			print_event(&ev);
		}
		core_show_state(err ? IND_ERROR : IND_IDLE);
		core_watchdog_feed();
	}
}
