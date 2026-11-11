#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>
#include "chanpick.h"
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
#define USB_RETRY_MS 2

struct rx_item {
	uint8_t len;
	uint8_t pipe;
	int8_t rssi;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_item), 64, 4);
K_MUTEX_DEFINE(reports_lock);
static struct k_sem reports_ready[REPORT_IFACES];

static struct link link;
static struct reports reports;
static struct history history;
static struct chan_survey survey;
static atomic_t host_leds;
static int8_t last_rssi;
static uint32_t link_down_ms;
static bool link_was_lost;
static uint32_t reset_cause;
static bool heard;
static uint32_t last_packet_ms;

static struct {
	uint32_t packets;
	uint32_t invalid;
	uint32_t rx_dropped;
	uint32_t channel_changes;
	uint32_t link_losses;
	uint32_t usb_retries;
	uint32_t surveys;
} counters;

/* Must outlast one survey_radio call, which blocks for about 1.5 s. */
#define WATCHDOG_TIMEOUT_MS 3000

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

static void watchdog_start(void)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = WATCHDOG_TIMEOUT_MS},
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0 || wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
		printk("watchdog not running\n");
		wdt_channel = -1;
	}
}

static void watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		wdt_feed(wdt, wdt_channel);
	}
}

static struct history_totals totals(void)
{
	return (struct history_totals){
		.packets = counters.packets,
		.invalid = counters.invalid,
		.dropped = counters.rx_dropped,
		.channel_changes = counters.channel_changes,
		.link_losses = counters.link_losses,
		.usb_retries = counters.usb_retries,
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

static void show_state(void)
{
	static const enum indicator_mode by_link[] = {
		[LINK_LED_IDLE] = IND_IDLE,
		[LINK_LED_PAIRING] = IND_PAIRING,
		[LINK_LED_CONNECTED] = IND_CONNECTED,
	};

	indicator_set(usb_suspended() ? IND_SLEEP : by_link[link_led(&link)]);
}

static void on_radio_rx(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi)
{
	struct rx_item item = {.len = len, .pipe = pipe, .rssi = rssi};

	memcpy(item.data, data, MIN(len, sizeof(item.data)));
	if (k_msgq_put(&rx_queue, &item, K_NO_WAIT) != 0) {
		counters.rx_dropped++;
	}
}

static void lock_reports(void)
{
	k_mutex_lock(&reports_lock, K_FOREVER);
}

static void unlock_reports(bool wake_senders)
{
	k_mutex_unlock(&reports_lock);
	if (wake_senders) {
		for (int iface = 0; iface < REPORT_IFACES; iface++) {
			k_sem_give(&reports_ready[iface]);
		}
	}
}

static void release_keys(void)
{
	lock_reports();
	reports_release_all(&reports);
	unlock_reports(true);
}

static void apply(const struct link_out *out)
{
	/* radio_set_channel flushes queued replies. */
	if (out->set_channel && radio_set_channel(out->channel) == 0) {
		counters.channel_changes++;
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
		printk("link lost after %u ms, last rssi %d\n", gap, last_rssi);
		log_event(HISTORY_LINK_LOST, (uint8_t)last_rssi, 0, 0, gap);
		link_down_ms = k_uptime_get_32();
		link_was_lost = true;
		release_keys();
	}
	if (out->up) {
		log_event(HISTORY_LINK_UP, link.channel, (uint8_t)last_rssi, link_was_lost,
			  k_uptime_get_32() - link_down_ms);
	}
	if (out->moved) {
		log_event(HISTORY_LINK_MOVED, out->moved_from, out->channel, 0, 0);
	}
}

/* The survey blocks the main loop, which otherwise updates the LED. */
static int survey_radio(void)
{
	watchdog_feed();
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
	bool changed;

	if (m.kind == NUPHY_INVALID) {
		counters.invalid++;
		return;
	}
	counters.packets++;
	last_rssi = item->rssi;
	heard = true;
	last_packet_ms = k_uptime_get_32();
	history_rssi(&history, item->rssi);
	link_on_message(&link, &m, item->pipe, k_uptime_get_32(), &out);
	apply(&out);
	if (out.start_survey) {
		run_survey();
	}
	if (item->pipe != 1) {
		return;
	}
	lock_reports();
	changed = reports_on_message(&reports, &m);
	unlock_reports(changed);
	if (usb_suspended() && (changed || m.kind == NUPHY_HELLO)) {
		usb_wakeup();
	}
}

static void usb_thread(void *a, void *b, void *c)
{
	enum report_iface iface = (enum report_iface)(uintptr_t)a;

	for (;;) {
		struct report r;
		bool have;

		k_mutex_lock(&reports_lock, K_FOREVER);
		have = reports_peek(&reports, iface, &r);
		if (have) {
			reports_pop(&reports, iface);
		}
		k_mutex_unlock(&reports_lock);
		if (!have) {
			k_sem_take(&reports_ready[iface], K_FOREVER);
			continue;
		}
		while (usb_send(&r) != 0 && usb_live()) {
			counters.usb_retries++;
			k_msleep(USB_RETRY_MS);
		}
	}
}

K_THREAD_DEFINE(keyboard_tid, 1024, usb_thread, (void *)REPORT_IFACE_KEYBOARD, NULL, NULL, 5, 0,
		SYS_FOREVER_MS);
K_THREAD_DEFINE(extra_tid, 1024, usb_thread, (void *)REPORT_IFACE_EXTRA, NULL, NULL, 5, 0,
		SYS_FOREVER_MS);

static void on_usb_live(void)
{
	lock_reports();
	reports_set_live(&reports, usb_live());
	unlock_reports(true);
}

static void on_usb_leds(uint8_t leds)
{
	atomic_set(&host_leds, leds);
}

static void print_counters(void)
{
	printk("counters: packets %u invalid %u dropped %u channels %u losses %u usb-retries %u "
	       "merges %u surveys %u, ch%u state %d\n",
	       counters.packets, counters.invalid, counters.rx_dropped, counters.channel_changes,
	       counters.link_losses, counters.usb_retries, reports.merges, counters.surveys,
	       link.channel, link.state);
}

static void fill_status(struct diag_status *s)
{
	*s = (struct diag_status){
		.firmware_version = {FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR,
				     FIRMWARE_VERSION_PATCH},
		.uptime_ms = k_uptime_get(),
		.reset_cause = reset_cause,
		.link_state = link.state,
		.channel = link.channel,
		.last_rssi = last_rssi,
		.last_rx_age_ms = heard ? k_uptime_get_32() - last_packet_ms : UINT32_MAX,
		.usb_live = usb_live(),
		.usb_suspended = usb_suspended(),
		.totals = totals(),
		.merges = reports.merges,
		.surveys = counters.surveys,
	};
	if (link.own_known) {
		memcpy(s->own, link.own, sizeof(s->own));
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
	release_keys();
	err = survey_radio();

	if (err) {
		diag_reply_error(req, DIAG_ERR_RADIO);
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
	lock_reports();
	reports.merges = 0;
	unlock_reports(false);
	history_clear(&history, k_uptime_get(), &none);
	log_event(HISTORY_CLEAR, 0, 0, 0, 0);
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
	case DIAG_REBOOT:
		diag_reply(req, NULL, 0, DIAG_AFTER_REBOOT);
		break;
	case DIAG_BOOTLOADER:
		diag_reply(req, NULL, 0, DIAG_AFTER_BOOTLOADER);
		break;
	case DIAG_CLEAR:
		clear_history(req);
		break;
	default:
		diag_reply_error(req, DIAG_ERR_UNKNOWN_COMMAND);
		break;
	}
}

int main(void)
{
	static const struct usb_callbacks usb_cb = {
		.on_live = on_usb_live,
		.on_leds = on_usb_leds,
		.on_diag_request = diag_on_request,
	};
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	uint32_t last_counters = 0;
	bool was_suspended = false;
	bool usb_seen_live = false;
	uint8_t id[4];
	uint8_t own[3];
	struct link_out out = {0};
	bool paired;
	int store_err;
	int err;

	memcpy(id, &raw, sizeof(id));
	hwinfo_get_reset_cause(&reset_cause);
	hwinfo_clear_reset_cause();
	reports_init(&reports);
	history_init(&history, k_uptime_get(), &(struct history_totals){0});
	log_event(HISTORY_BOOT, 0, 0, 0, reset_cause);
	for (int iface = 0; iface < REPORT_IFACES; iface++) {
		k_sem_init(&reports_ready[iface], 0, 1);
	}
	err = usb_start(&usb_cb);
	if (err) {
		printk("usb start failed: %d\n", err);
	}
	k_thread_start(keyboard_tid);
	k_thread_start(extra_tid);

	store_err = store_init();
	paired = store_err == 0 && store_load_channels(own);
	link_init(&link, id, paired ? own : NULL, k_uptime_get_32(), &out);
	watchdog_start();
	while ((err = radio_start(id, out.channel, on_radio_rx)) != 0) {
		indicator_set(IND_ERROR);
		printk("radio start failed: %d\n", err);
		k_sleep(K_SECONDS(1));
		watchdog_feed();
	}
	printk("open-dongle id %02x%02x%02x%02x %s, store %d, reset cause 0x%x\n", id[0], id[1],
	       id[2], id[3], paired ? "paired" : "never paired", store_err, reset_cause);

	for (;;) {
		struct rx_item item;
		struct diag_request req;
		struct history_totals t;
		bool suspended;
		uint32_t now;

		if (k_msgq_get(&rx_queue, &item, K_MSEC(TICK_MS)) == 0) {
			handle(&item);
		}
		now = k_uptime_get_32();
		link_set_host_leds(&link, (uint8_t)atomic_get(&host_leds));
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
		t = totals();
		history_tick(&history, k_uptime_get(), &t);
		out = (struct link_out){0};
		link_tick(&link, now, &out);
		apply(&out);
		while (diag_take_request(&req)) {
			serve_diag(&req);
		}
		if (IS_ENABLED(CONFIG_COUNTERS) && now - last_counters >= COUNTERS_EVERY_MS) {
			print_counters();
			last_counters = now;
		}
		show_state();
		watchdog_feed();
	}
}
