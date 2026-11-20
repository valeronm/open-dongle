#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>
#include "chanpick.h"
#include "core.h"
#include "diag.h"
#include "diagwire.h"
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
static int8_t keyboard_rssi = DIAG_NO_RSSI;
static uint32_t link_down_ms;
static bool link_was_lost;
static bool heard;
static uint32_t last_packet_ms;
static struct nuphy_keys keys;

_Static_assert(sizeof(((struct diag_status *)0)->name) >= NUPHY_NAME_MAX, "status name");
_Static_assert(DIAG_NAME_MAX >= NUPHY_NAME_MAX, "device list name");
_Static_assert(DIAG_ADDR_LEN >= NUPHY_ID_LEN, "device list address");

static int home(void)
{
	return link_home(&link);
}

/* Key releases sent while the receiver was off are lost. */
static void radio_resumed(void)
{
	keys = (struct nuphy_keys){0};
	link_radio_resumed(&link, k_uptime_get_32());
}

static void show_state(void)
{
	static const enum indicator_mode by_link[] = {
		[LINK_LED_IDLE] = IND_IDLE,
		[LINK_LED_PAIRING] = IND_PAIRING,
		[LINK_LED_CONNECTED] = IND_CONNECTED,
	};

	core_show_state(by_link[link_led(&link, k_uptime_get_32())]);
}

static int movable(void)
{
	return link_movable(&link, k_uptime_get_32());
}

static size_t candidates(const uint8_t **channels)
{
	return link_channels(&link, channels);
}

static void replies_dropped(void)
{
	link_replies_dropped(&link, k_uptime_get_32());
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
		core_log(0, (struct diag_detail){.tag = DIAG_DETAIL_PAIRED,
						 .PAIRED = {.own = {link.own[0], link.own[1], link.own[2]},
							    .store_err = err}});
	}
	if (out->lost) {
		uint32_t gap = k_uptime_get_32() - link.last_rx_ms;

		core_link_lost(0);
		printk("link lost after %u ms, last rssi %d\n", gap, keyboard_rssi);
		core_log(0, (struct diag_detail){.tag = DIAG_DETAIL_LINK_LOST,
						 .LINK_LOST = {.rssi = (int8_t)keyboard_rssi,
							       .since_input_s = core_since_input_s(0),
							       .gap_ms = gap}});
		link_down_ms = k_uptime_get_32();
		link_was_lost = true;
		core_release_keys();
		keys = (struct nuphy_keys){0};
	}
	if (out->up) {
		uint8_t kind = link_was_lost ? HISTORY_UP_AGAIN : HISTORY_UP_FIRST;

		core_log(0, (struct diag_detail){.tag = DIAG_DETAIL_LINK_UP,
					     .LINK_UP = {.channel = link.channel,
							 .rssi = (int8_t)keyboard_rssi,
							 .up = kind,
							 .down_ms = k_uptime_get_32() - link_down_ms}});
	}
	if (out->moved) {
		core_log(DEVICE_NO_SLOT, (struct diag_detail){.tag = DIAG_DETAIL_LINK_MOVED,
						      .LINK_MOVED = {.from = out->moved_from,
								     .to = out->channel,
								     .reason = HISTORY_MOVED_TO_HOME}});
	}
	if (out->pairing_expired) {
		core_log(DEVICE_NO_SLOT, (struct diag_detail){.tag = DIAG_DETAIL_PAIRING_TIMEOUT});
	}
	if (out->window_closed) {
		core_log_pairing_closed();
	}
}

static void run_survey(void)
{
	const struct chan_survey *survey;
	uint8_t picked[LINK_OWN_CHANNELS];
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
	core_log(DEVICE_NO_SLOT, (struct diag_detail){.tag = DIAG_DETAIL_SURVEY,
					      .SURVEY = {.picked = {picked[0], picked[1], picked[2]}}});
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
	core_packet(0);
	if (item->pipe == 1) {
		keyboard_rssi = item->rssi;
	}
	heard = true;
	last_packet_ms = k_uptime_get_32();
	link_on_message(&link, &m, item->pipe, k_uptime_get_32(), &out);
	if (out.gap_ms > 0) {
		core_gap(0);
		core_log(0, (struct diag_detail){.tag = DIAG_DETAIL_LINK_GAP,
					     .LINK_GAP = {.channel = out.gap_channel,
							  .rssi_before = (int8_t)rssi_before,
							  .rssi_after = (int8_t)item->rssi,
							  .gap_ms = out.gap_ms}});
	}
	if (item->pipe == 1) {
		core_rssi(0, item->rssi);
	}
	apply(&out);
	if (out.start_survey) {
		run_survey();
	}
	if (item->pipe != 1) {
		return;
	}
	if (nuphy_report(&keys, &m, &next)) {
		if (next.kind == REPORT_MOUSE) {
			/* The keyboard counts the wheel in whole notches. */
			int wheel = next.mouse.wheel * usb_mouse_wheel_steps();

			next.mouse.wheel = (int8_t)reports_clamp(wheel, -INT8_MAX, INT8_MAX);
		}
		core_submit(0, &next, &item->received_us);
	}
	if (usb_suspended() && m.kind == NUPHY_HELLO) {
		usb_wakeup();
	}
}

static void slots(struct diag_slot *out)
{
	out[0] = (struct diag_slot){
		.kind = DEVICE_KIND_KEYBOARD,
		.stored = link.own_known,
		.ready = link_connected(&link),
	};
}

static void fill_status(struct diag_status *s, uint8_t slot)
{
	const uint8_t *forced;

	ARG_UNUSED(slot);
	s->link_state = link.state;
	s->pairing_window =
		link_pairing_open(&link, k_uptime_get_32()) ? DIAG_WINDOW_OPEN : DIAG_WINDOW_CLOSED;
	s->channel = link.channel;
	s->last_rssi = keyboard_rssi;
	if (heard) {
		s->last_rx_age_ms = k_uptime_get_32() - last_packet_ms;
	}
	if (link.own_known) {
		memcpy(s->own, link.own, sizeof(s->own));
	}
	forced = link_forced_channels(&link);
	if (forced != NULL) {
		memcpy(s->forced, forced, sizeof(s->forced));
	}
	memcpy(s->keyboard_id, link.keyboard_id, sizeof(s->keyboard_id));
	memcpy(s->name, link.keyboard_name.name, sizeof(link.keyboard_name.name));
}

static void print_counters(void)
{
	struct diag_status s;

	core_fill_status(&s, 0);
	fill_status(&s, 0);
	printk("counters: packets %u invalid %u dropped %u losses %u gaps %u "
	       "usb-retries %u merges %u surveys %u, ch%u state %d\n",
	       s.packets, s.invalid, s.dropped, s.link_losses, s.gaps, s.usb_retries, s.merges,
	       s.surveys, link.channel, link.state);
}

static void force_channels(const struct diag_request *req)
{
	uint8_t channels[LINK_OWN_CHANNELS];
	const uint8_t *forced = diag_channels_arg(req->arg, channels) ? channels : NULL;
	int err;

	if (forced != NULL && !link_channels_allowed(forced)) {
		diag_reply_error(req, DIAG_ERROR_BAD_CHANNELS, 0);
		return;
	}
	err = store_save_forced(forced);
	if (err) {
		printk("saving forced channels failed: %d\n", err);
		diag_reply_error(req, DIAG_ERROR_STORE, 0);
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

static void send_devices(const struct diag_request *req)
{
	struct diag_device keyboard = {.kind = DEVICE_KIND_KEYBOARD, .slot = DEVICE_NO_SLOT};

	if (link.own_known) {
		memcpy(keyboard.addr, link.keyboard_id, sizeof(link.keyboard_id));
		memcpy(keyboard.name, link.keyboard_name.name, sizeof(link.keyboard_name.name));
		if (link_connected(&link)) {
			keyboard.slot = 0;
		}
	}
	core_reply_devices(req, &keyboard, link.own_known ? 1 : 0);
}

static void serve_diag(const struct diag_request *req)
{
	switch (req->command) {
	case DIAG_COMMAND_DEVICES:
		send_devices(req);
		break;
	case DIAG_COMMAND_FORCE_CHANNELS:
		force_channels(req);
		break;
	case DIAG_COMMAND_PAIR:
		link_open_pairing(&link, PAIRWIN_REQUEST, k_uptime_get_32());
		printk("pairing open for %u s\n", PAIRWIN_MS / 1000);
		core_log_pairing_opened(&link.window);
		diag_reply(req, NULL, 0, DIAG_AFTER_NONE);
		break;
	default:
		diag_reply_error(req, DIAG_ERROR_UNKNOWN_COMMAND, 0);
		break;
	}
}

static enum chanmove_result move_on_request(uint8_t channel, uint8_t *from)
{
	*from = link.channel;
	return link_move(&link, channel, k_uptime_get_32());
}

static const struct core_loop loop = {
	.home = home,
	.take_noise = core_take_radio_noise,
	.radio_resumed = radio_resumed,
	.slot_count = 1,
	.slots = slots,
	.fill_status = fill_status,
	.movable = movable,
	.candidates = candidates,
	.replies_dropped = replies_dropped,
	.move = move_on_request,
};

FUNC_NORETURN void nuphy_run(int store_err)
{
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	uint32_t last_counters = 0;
	uint8_t id[4];
	uint8_t own[LINK_OWN_CHANNELS];
	uint8_t forced[LINK_OWN_CHANNELS];
	struct link_out out = {0};
	bool paired;

	core_start_history(&loop);
	memcpy(id, &raw, sizeof(id));
	paired = store_err == 0 && store_load_channels(own);
	link_init(&link, id, paired ? own : NULL, k_uptime_get_32(), &out);
	link_open_pairing(&link, PAIRWIN_BOOT, k_uptime_get_32());
	core_log_pairing_opened(&link.window);
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
	printk("spare-dongle id %02x%02x%02x%02x %s, store %d, reset cause 0x%x\n", id[0], id[1],
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
