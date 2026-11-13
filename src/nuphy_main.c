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
static struct history history;
static struct chan_survey survey;
/* Pipe 1 is addressed to this dongle, so only its paired keyboard sends there. */
static int8_t keyboard_rssi;
static uint32_t link_down_ms;
static bool link_was_lost;
static uint32_t reset_cause;
static bool heard;
static uint32_t last_packet_ms;

static struct {
	uint32_t packets;
	uint32_t invalid;
	uint32_t channel_changes;
	uint32_t link_losses;
	uint32_t gaps;
	uint32_t surveys;
} counters;

static struct history_totals totals(void)
{
	return (struct history_totals){
		.packets = counters.packets,
		.invalid = counters.invalid,
		.dropped = core_rx_dropped(),
		.channel_changes = counters.channel_changes,
		.link_losses = counters.link_losses,
		.usb_retries = core_usb_retries(),
		.gaps = counters.gaps,
	};
}

static void log_event(uint8_t type, uint8_t a, uint8_t b, uint8_t c, uint32_t value)
{
	history_event(&history, &(struct history_event){.time_ms = k_uptime_get(),
							.type = type,
							.a = a,
							.b = b,
							.c = c,
							.value = value});
}

static void take_noise(void)
{
	struct noise_tally t;
	uint8_t channel = radio_noise_take(&t);

	history_noise(&history, &t, link.own_known && channel == link.own[0]);
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
	/* radio_set_channel flushes queued replies and drops the noise readings not yet taken. */
	if (out->set_channel) {
		take_noise();
		if (radio_set_channel(out->channel) == 0) {
			counters.channel_changes++;
		}
	}
	if (out->reply) {
		radio_reply(out->reply_pipe, out->reply_buf, out->reply_len);
	}
	if (out->save_channels) {
		int err = store_save_channels(link.own);

		printk("paired, channels %u %u %u saved: %d\n", link.own[0], link.own[1],
		       link.own[2], err);
		log_event(HISTORY_PAIRED, link.own[0], link.own[1], link.own[2], (uint32_t)err);
	}
	if (out->lost) {
		uint32_t gap = k_uptime_get_32() - link.last_rx_ms;

		counters.link_losses++;
		printk("link lost after %u ms, last rssi %d\n", gap, keyboard_rssi);
		log_event(HISTORY_LINK_LOST, (uint8_t)keyboard_rssi, 0, 0, gap);
		link_down_ms = k_uptime_get_32();
		link_was_lost = true;
		core_release_keys();
	}
	if (out->up) {
		log_event(HISTORY_LINK_UP, link.channel, (uint8_t)keyboard_rssi, link_was_lost,
			  k_uptime_get_32() - link_down_ms);
	}
	if (out->moved) {
		log_event(HISTORY_LINK_MOVED, out->moved_from, out->channel, 0, 0);
	}
}

/* The survey blocks the main loop, which otherwise updates the LED. */
static int survey_radio(void)
{
	core_watchdog_feed();
	show_state();
	return radio_survey(&survey);
}

static void run_survey(void)
{
	uint8_t picked[3];
	struct link_out out = {0};

	counters.surveys++;
	if (survey_radio() != 0) {
		link_survey_failed(&link, k_uptime_get_32(), &out);
		apply(&out);
		return;
	}
	chan_pick(&survey, picked);
	printk("survey picked %u %u %u\n", picked[0], picked[1], picked[2]);
	log_event(HISTORY_SURVEY, picked[0], picked[1], picked[2], 0);
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
		counters.invalid++;
		return;
	}
	counters.packets++;
	if (item->pipe == 1) {
		keyboard_rssi = item->rssi;
	}
	heard = true;
	last_packet_ms = k_uptime_get_32();
	link_on_message(&link, &m, item->pipe, k_uptime_get_32(), &out);
	take_noise();
	if (out.gap_ms > 0) {
		counters.gaps++;
		log_event(HISTORY_LINK_GAP, out.gap_channel, (uint8_t)rssi_before,
			  (uint8_t)item->rssi, out.gap_ms);
	}
	if (item->pipe == 1) {
		history_rssi(&history, item->rssi);
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

static void print_counters(void)
{
	printk("counters: packets %u invalid %u dropped %u channels %u losses %u gaps %u "
	       "usb-retries %u merges %u surveys %u, ch%u state %d\n",
	       counters.packets, counters.invalid, core_rx_dropped(), counters.channel_changes,
	       counters.link_losses, counters.gaps, core_usb_retries(), core_merges(),
	       counters.surveys,
	       link.channel, link.state);
}

static void fill_status(struct diag_status *s)
{
	const uint8_t *forced;

	*s = (struct diag_status){
		.uptime_ms = k_uptime_get(),
		.reset_cause = reset_cause,
		.link_state = link.state,
		.channel = link.channel,
		.last_rssi = keyboard_rssi,
		.last_rx_age_ms = heard ? k_uptime_get_32() - last_packet_ms : UINT32_MAX,
		.usb_live = usb_live(),
		.usb_suspended = usb_suspended(),
		.totals = totals(),
		.merges = core_merges(),
		.surveys = counters.surveys,
		.protocol = PROTOCOL_NUPHY,
	};
	memcpy(s->firmware_version, core_firmware_version, sizeof(s->firmware_version));
	if (link.own_known) {
		memcpy(s->own, link.own, sizeof(s->own));
	}
	forced = link_forced_channels(&link);
	if (forced != NULL) {
		memcpy(s->forced, forced, sizeof(s->forced));
	}
	memcpy(s->keyboard_id, link.keyboard_id, sizeof(s->keyboard_id));
	s->minute_next = history.minute_next;
	s->minute_oldest = history.minute_oldest;
	s->events_total = history.events_total;
	s->events_oldest = history.events_oldest;
}

static void run_noise(const struct diag_request *req, uint8_t *payload)
{
	int err;

	/* The receiver goes deaf during the survey, so a key held now stays down on the host. */
	core_release_keys();
	err = survey_radio();

	if (err) {
		diag_reply_error(req, DIAG_ERR_RADIO, 0);
		return;
	}
	link_radio_resumed(&link, k_uptime_get_32());
	log_event(HISTORY_NOISE, 0, 0, 0, 0);
	diag_reply(req, payload, diag_encode_noise(payload, &survey), DIAG_AFTER_NONE);
}

static void clear_history(const struct diag_request *req)
{
	struct history_totals none = {0};

	memset(&counters, 0, sizeof(counters));
	core_clear_counters();
	history_clear(&history, k_uptime_get(), &none);
	log_event(HISTORY_CLEAR, 0, 0, 0, 0);
	diag_reply(req, NULL, 0, DIAG_AFTER_NONE);
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
	static uint8_t payload[DIAG_PAYLOAD_MAX];
	struct diag_status s;

	switch (req->command) {
	case DIAG_STATUS:
		fill_status(&s);
		diag_reply(req, payload, diag_encode_status(payload, &s), DIAG_AFTER_NONE);
		break;
	case DIAG_HISTORY: {
		static struct history_minute page[DIAG_MINUTES_PER_PAGE];
		uint32_t first;
		size_t n = history_minutes(&history, req->arg, &first, page, ARRAY_SIZE(page));

		diag_reply(req, payload, diag_encode_minutes(payload, first, page, n),
			   DIAG_AFTER_NONE);
		break;
	}
	case DIAG_EVENTS: {
		static struct history_event page[DIAG_EVENTS_PER_PAGE];
		uint32_t first;
		size_t n = history_events(&history, req->arg, &first, page, ARRAY_SIZE(page));

		diag_reply(req, payload, diag_encode_events(payload, first, page, n),
			   DIAG_AFTER_NONE);
		break;
	}
	case DIAG_NOISE:
		run_noise(req, payload);
		break;
	case DIAG_CLEAR:
		clear_history(req);
		break;
	case DIAG_FORCE_CHANNELS:
		force_channels(req);
		break;
	case DIAG_PAIR:
		diag_reply_error(req, DIAG_ERR_WRONG_PROTOCOL, PROTOCOL_NUPHY);
		break;
	default:
		diag_reply_error(req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		break;
	}
}

void nuphy_run(int store_err, uint32_t reset_cause_at_boot)
{
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	uint32_t last_counters = 0;
	bool was_suspended = false;
	bool usb_seen_live = false;
	uint8_t id[4];
	uint8_t own[3];
	uint8_t forced[3];
	struct link_out out = {0};
	bool paired;

	reset_cause = reset_cause_at_boot;
	history_init(&history, k_uptime_get(), &(struct history_totals){0});
	log_event(HISTORY_BOOT, 0, 0, 0, reset_cause);
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
	       id[2], id[3], paired ? "paired" : "never paired", store_err, reset_cause);

	for (;;) {
		struct rx_item item;
		struct diag_request req;
		struct history_totals t;
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
		usb_seen_live |= usb_live();
		if (suspended != was_suspended) {
			if (usb_seen_live) {
				log_event(suspended ? HISTORY_USB_SUSPEND : HISTORY_USB_RESUME,
					  0, 0, 0, 0);
			}
			was_suspended = suspended;
		}
		take_noise();
		t = totals();
		history_tick(&history, k_uptime_get(), &t);
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
