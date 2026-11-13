#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>
#include "chanpick.h"
#include "core.h"
#include "diag.h"
#include "diagproto.h"
#include "history.h"
#include "indicator.h"
#include "link.h"
#include "nuphy.h"
#include "radio.h"
#include "reports.h"
#include "store.h"
#include "usb.h"

#define TICK_MS 10
#define COUNTERS_EVERY_MS 10000

static struct link link;
/* Pipe 1 is addressed to this dongle, so only its paired keyboard sends there. */
static int8_t keyboard_rssi;
static uint32_t link_down_ms;
static bool link_was_lost;
static bool heard;
static uint32_t last_packet_ms;

static int home(void)
{
	return link.own_known ? link.own[0] : CORE_NO_HOME;
}

static void radio_resumed(void)
{
	link_radio_resumed(&link, k_uptime_get_32());
}

static void show_state(void)
{
	static const enum indicator_mode by_link[] = {
		[LINK_LED_IDLE] = IND_IDLE,
		[LINK_LED_PAIRING] = IND_PAIRING,
		[LINK_LED_CONNECTED] = IND_CONNECTED,
	};

	core_show_state(by_link[link_led(&link)]);
}

static void apply(const struct link_out *out)
{
	/* A channel change flushes queued replies. */
	if (out->set_channel) {
		core_set_channel(out->channel);
	}
	if (out->reply) {
		radio_reply(out->reply_pipe, out->reply_buf, out->reply_len);
	}
	if (out->save_channels) {
		int err = store_save_channels(link.own);

		printk("paired, channels %u %u %u saved: %d\n", link.own[0], link.own[1],
		       link.own[2], err);
		core_log(HISTORY_PAIRED, link.own[0], link.own[1], link.own[2], (uint32_t)err);
	}
	if (out->lost) {
		uint32_t gap = k_uptime_get_32() - link.last_rx_ms;

		core_count(CORE_LINK_LOSSES);
		printk("link lost after %u ms, last rssi %d\n", gap, keyboard_rssi);
		core_log(HISTORY_LINK_LOST, (uint8_t)keyboard_rssi, 0, 0, gap);
		link_down_ms = k_uptime_get_32();
		link_was_lost = true;
		core_release_keys();
	}
	if (out->up) {
		core_log(HISTORY_LINK_UP, link.channel, (uint8_t)keyboard_rssi, link_was_lost,
			 k_uptime_get_32() - link_down_ms);
	}
	if (out->moved) {
		core_log(HISTORY_LINK_MOVED, out->moved_from, out->channel, 0, 0);
	}
}

static void run_survey(void)
{
	const struct chan_survey *survey;
	uint8_t picked[3];
	struct link_out out = {0};

	core_count(CORE_SURVEYS);
	/* The survey blocks the main loop, which otherwise updates the LED. */
	show_state();
	survey = core_survey();
	if (survey == NULL) {
		link_survey_failed(&link, k_uptime_get_32(), &out);
		apply(&out);
		return;
	}
	chan_pick(survey, picked);
	printk("survey picked %u %u %u\n", picked[0], picked[1], picked[2]);
	core_log(HISTORY_SURVEY, picked[0], picked[1], picked[2], 0);
	link_on_survey(&link, picked, k_uptime_get_32(), &out);
	apply(&out);
}

static void handle(const struct rx_item *item)
{
	struct nuphy_msg m = nuphy_parse(item->data, item->len);
	struct link_out out = {0};
	int8_t rssi_before = keyboard_rssi;
	struct report next;

	if (m.kind == NUPHY_INVALID) {
		core_count(CORE_INVALID);
		return;
	}
	core_count(CORE_PACKETS);
	if (item->pipe == 1) {
		keyboard_rssi = item->rssi;
	}
	heard = true;
	last_packet_ms = k_uptime_get_32();
	link_on_message(&link, &m, item->pipe, k_uptime_get_32(), &out);
	if (out.gap_ms > 0) {
		core_count(CORE_GAPS);
		core_log(HISTORY_LINK_GAP, out.gap_channel, (uint8_t)rssi_before,
			 (uint8_t)item->rssi, out.gap_ms);
	}
	if (item->pipe == 1) {
		core_rssi(item->rssi);
	}
	apply(&out);
	if (out.start_survey) {
		run_survey();
	}
	if (item->pipe != 1) {
		return;
	}
	if (nuphy_report(&m, &next)) {
		core_submit(&next);
	}
	if (usb_suspended() && m.kind == NUPHY_HELLO) {
		usb_wakeup();
	}
}

static void fill_status(struct diag_status *s)
{
	const uint8_t *forced;

	s->link_state = link.state;
	s->channel = link.channel;
	s->last_rssi = keyboard_rssi;
	s->last_rx_age_ms = heard ? k_uptime_get_32() - last_packet_ms : UINT32_MAX;
	if (link.own_known) {
		memcpy(s->own, link.own, sizeof(s->own));
	}
	forced = link_forced_channels(&link);
	if (forced != NULL) {
		memcpy(s->forced, forced, sizeof(s->forced));
	}
	memcpy(s->keyboard_id, link.keyboard_id, sizeof(s->keyboard_id));
}

static void print_counters(void)
{
	struct diag_status s;

	core_fill_status(&s);
	fill_status(&s);
	printk("counters: packets %u invalid %u dropped %u channels %u losses %u gaps %u "
	       "usb-retries %u merges %u surveys %u, ch%u state %d\n",
	       s.totals.packets, s.totals.invalid, s.totals.dropped, s.totals.channel_changes,
	       s.totals.link_losses, s.totals.gaps, s.totals.usb_retries, s.merges, s.surveys,
	       link.channel, link.state);
}

static void force_channels(const struct diag_request *req)
{
	uint8_t channels[3];
	const uint8_t *forced = diag_channels_arg(req->arg, channels) ? channels : NULL;
	int err;

	if (forced != NULL && !link_channels_allowed(forced)) {
		diag_reply_error(req, DIAG_ERR_BAD_CHANNELS, 0);
		return;
	}
	err = store_save_forced(forced);
	if (err) {
		printk("saving forced channels failed: %d\n", err);
		diag_reply_error(req, DIAG_ERR_STORE, 0);
		return;
	}
	link_force_channels(&link, forced);
	if (forced != NULL) {
		printk("pairing will offer %u %u %u\n", forced[0], forced[1], forced[2]);
	} else {
		printk("pairing will survey\n");
	}
	diag_reply(req, NULL, 0, DIAG_AFTER_NONE);
}

static void serve_diag(const struct diag_request *req)
{
	switch (req->command) {
	case DIAG_FORCE_CHANNELS:
		force_channels(req);
		break;
	default:
		diag_reply_error(req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		break;
	}
}

static const struct core_loop loop = {
	.home = home,
	.radio_resumed = radio_resumed,
	.fill_status = fill_status,
};

FUNC_NORETURN void nuphy_run(int store_err)
{
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	uint32_t last_counters = 0;
	uint8_t id[4];
	uint8_t own[3];
	uint8_t forced[3];
	struct link_out out = {0};
	bool paired;

	core_start_history(&loop);
	memcpy(id, &raw, sizeof(id));
	paired = store_err == 0 && store_load_channels(own);
	link_init(&link, id, paired ? own : NULL, k_uptime_get_32(), &out);
	if (store_err == 0 && store_load_forced(forced) && !link_force_channels(&link, forced)) {
		printk("stored forced channels refused\n");
	}
	struct radio_config rc = {
		.rate = RADIO_1MBPS,
		.crc = RADIO_CRC_8,
		.base0 = {0x56, 0x56, 0x56, 0x56},
		.prefixes = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58},
		.pipes = 8,
		.channel = out.channel,
		.noise = true,
	};

	/* The keyboard addresses pipe 1 with the dongle ID it was given at pairing. */
	memcpy(rc.base1, id, sizeof(rc.base1));
	core_start_radio(&rc);
	printk("open-dongle id %02x%02x%02x%02x %s, store %d, reset cause 0x%x\n", id[0], id[1],
	       id[2], id[3], paired ? "paired" : "never paired", store_err, core_reset_cause());

	for (;;) {
		struct rx_item item;
		struct diag_request req;
		bool suspended;
		uint32_t now;
		uint8_t leds;

		if (core_take_rx(&item, K_MSEC(TICK_MS))) {
			handle(&item);
		}
		now = k_uptime_get_32();
		core_take_leds(&leds);
		link_set_host_leds(&link, leds);
		suspended = usb_suspended();
		link_set_host_suspended(&link, suspended);
		core_poll();
		out = (struct link_out){0};
		link_tick(&link, now, &out);
		apply(&out);
		while (core_take_request(&req)) {
			serve_diag(&req);
		}
		if (IS_ENABLED(CONFIG_COUNTERS) && now - last_counters >= COUNTERS_EVERY_MS) {
			print_counters();
			last_counters = now;
		}
		show_state();
		core_watchdog_feed();
	}
}
