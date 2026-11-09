#include <string.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>
#include "chanpick.h"
#include "link.h"
#include "protocol.h"
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
static struct chan_survey survey;
static atomic_t host_leds;

static struct {
	uint32_t packets;
	uint32_t invalid;
	uint32_t rx_dropped;
	uint32_t channel_changes;
	uint32_t link_losses;
	uint32_t usb_retries;
	uint32_t surveys;
} counters;

/* Must outlast run_survey, which blocks the main loop for about 1.5 s. */
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

static void apply(const struct link_out *out)
{
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
	}
	if (out->lost) {
		counters.link_losses++;
		lock_reports();
		reports_release_all(&reports);
		unlock_reports(true);
	}
}

static void run_survey(void)
{
	uint8_t picked[3];
	struct link_out out = {0};

	counters.surveys++;
	if (radio_survey(&survey) != 0) {
		link_survey_failed(&link, k_uptime_get_32(), &out);
		apply(&out);
		return;
	}
	chan_pick(&survey, picked);
	printk("survey picked %u %u %u\n", picked[0], picked[1], picked[2]);
	link_on_survey(&link, picked, k_uptime_get_32(), &out);
	apply(&out);
}

static void handle(const struct rx_item *item)
{
	struct proto_msg m = proto_parse(item->data, item->len);
	struct link_out out = {0};
	bool changed;

	if (m.kind == PROTO_INVALID) {
		counters.invalid++;
		return;
	}
	counters.packets++;
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
	if (usb_suspended() && (changed || m.kind == PROTO_HELLO)) {
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

int main(void)
{
	static const struct usb_callbacks usb_cb = {
		.on_live = on_usb_live,
		.on_leds = on_usb_leds,
	};
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	uint32_t last_counters = 0;
	uint8_t id[4];
	uint8_t own[3];
	struct link_out out = {0};
	bool paired;
	int store_err;
	int err;

	memcpy(id, &raw, sizeof(id));
	reports_init(&reports);
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
	while ((err = radio_start(id, out.channel, on_radio_rx)) != 0) {
		printk("radio start failed: %d\n", err);
		k_sleep(K_SECONDS(1));
	}
	printk("nuphy-dongle id %02x%02x%02x%02x %s, store %d\n", id[0], id[1], id[2], id[3],
	       paired ? "paired" : "never paired", store_err);
	watchdog_start();

	for (;;) {
		struct rx_item item;
		uint32_t now;

		if (k_msgq_get(&rx_queue, &item, K_MSEC(TICK_MS)) == 0) {
			handle(&item);
		}
		now = k_uptime_get_32();
		link_set_host_leds(&link, (uint8_t)atomic_get(&host_leds));
		link_set_host_suspended(&link, usb_suspended());
		out = (struct link_out){0};
		link_tick(&link, now, &out);
		apply(&out);
		if (IS_ENABLED(CONFIG_NUPHY_COUNTERS) && now - last_counters >= COUNTERS_EVERY_MS) {
			print_counters();
			last_counters = now;
		}
		if (wdt_channel >= 0) {
			wdt_feed(wdt, wdt_channel);
		}
	}
}
