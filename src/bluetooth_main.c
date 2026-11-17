#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "ble.h"
#include "core.h"
#include "diag.h"
#include "indicator.h"

#define TICK_MS 10
#define REPORT_PERIOD_MS 10000

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

FUNC_NORETURN void bluetooth_run(int store_err)
{
	int64_t reported_ms = 0;
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
		int64_t now;

		k_msleep(TICK_MS);
		now = k_uptime_get();
		core_poll();
		while (core_take_request(&req)) {
			diag_reply_error(&req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		}
		if (IS_ENABLED(CONFIG_COUNTERS) && err == 0 && now - reported_ms >= REPORT_PERIOD_MS) {
			reported_ms = now;
			printk("adverts heard: %u\n", ble_adverts_heard());
		}
		core_show_state(err ? IND_ERROR : IND_IDLE);
		core_watchdog_feed();
	}
}
