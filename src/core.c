#include <string.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "core.h"
#include "diag.h"
#include "indicator.h"
#include "store.h"
#include "usb.h"

#define USB_RETRY_MS 2

/* Must outlast one radio_survey call, which blocks for about 1.5 s. */
#define WATCHDOG_TIMEOUT_MS 3000

const uint8_t core_firmware_version[3] = {FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR,
					  FIRMWARE_VERSION_PATCH};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_item), 64, 4);
static K_MUTEX_DEFINE(reports_lock);
static struct k_sem reports_ready[REPORT_IFACES];

static enum protocol active;
static uint32_t boot_reset_cause;
static struct reports reports;
static atomic_t host_leds;
static atomic_t leds_changed;
static atomic_t rx_dropped;
static atomic_t usb_retries;
static struct history history;
static uint32_t counts[CORE_COUNTERS];
static const struct core_loop *loop;
static int64_t last_key_ms = -1;
static bool usb_was_suspended;
static bool usb_seen_live;

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

void core_watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		wdt_feed(wdt, wdt_channel);
	}
}

void core_on_radio_rx(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi)
{
	struct rx_item item = {.len = len, .pipe = pipe, .rssi = rssi};

	memcpy(item.data, data, MIN(len, sizeof(item.data)));
	if (k_msgq_put(&rx_queue, &item, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	}
}

void core_start_radio(const struct radio_config *rc)
{
	int err;

	while ((err = radio_start(rc, core_on_radio_rx)) != 0) {
		indicator_set(IND_ERROR);
		printk("radio start failed: %d\n", err);
		k_sleep(K_SECONDS(1));
		core_watchdog_feed();
	}
}

bool core_take_rx(struct rx_item *out, k_timeout_t wait)
{
	return k_msgq_get(&rx_queue, out, wait) == 0;
}

uint32_t core_rx_dropped(void)
{
	return (uint32_t)atomic_get(&rx_dropped);
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

bool core_submit(const struct report *r)
{
	bool changed;

	lock_reports();
	changed = reports_update(&reports, r);
	unlock_reports(changed);
	if (changed) {
		last_key_ms = k_uptime_get();
	}
	if (changed && usb_suspended()) {
		usb_wakeup();
	}
	return changed;
}

void core_release_keys(void)
{
	lock_reports();
	reports_release_all(&reports);
	unlock_reports(true);
}

uint32_t core_usb_retries(void)
{
	return (uint32_t)atomic_get(&usb_retries);
}

uint32_t core_merges(void)
{
	uint32_t merges;

	lock_reports();
	merges = reports.merges;
	unlock_reports(false);
	return merges;
}

static struct history_totals totals(void)
{
	return (struct history_totals){
		.packets = counts[CORE_PACKETS],
		.invalid = counts[CORE_INVALID],
		.dropped = core_rx_dropped(),
		.channel_changes = counts[CORE_CHANNEL_CHANGES],
		.link_losses = counts[CORE_LINK_LOSSES],
		.usb_retries = core_usb_retries(),
		.gaps = counts[CORE_GAPS],
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
	int home = loop->home();

	history_noise(&history, &t, home != CORE_NO_HOME && channel == home);
}

void core_start_history(const struct core_loop *l)
{
	loop = l;
	history_init(&history, k_uptime_get(), &(struct history_totals){0});
	log_event(HISTORY_BOOT, 0, 0, 0, boot_reset_cause);
}

void core_count(enum core_counter c)
{
	counts[c]++;
}

void core_log(uint8_t type, uint8_t a, uint8_t b, uint8_t c, uint32_t value)
{
	take_noise();
	log_event(type, a, b, c, value);
}

void core_log_pairing_opened(const struct pairwin *w)
{
	core_log(HISTORY_PAIRING_OPENED, w->cause, w->until_paired, 0, 0);
}

void core_rssi(int8_t dbm)
{
	take_noise();
	history_rssi(&history, dbm);
}

uint8_t core_minutes_since_key(void)
{
	int64_t minutes;

	if (last_key_ms < 0) {
		return UINT8_MAX;
	}
	minutes = (k_uptime_get() - last_key_ms) / HISTORY_MINUTE_MS;
	return minutes > UINT8_MAX ? UINT8_MAX : (uint8_t)minutes;
}

void core_poll(void)
{
	bool suspended = usb_suspended();
	struct history_totals t;

	usb_seen_live |= usb_live();
	if (suspended != usb_was_suspended) {
		if (usb_seen_live) {
			core_log(suspended ? HISTORY_USB_SUSPEND : HISTORY_USB_RESUME, 0, 0, 0, 0);
		}
		usb_was_suspended = suspended;
	}
	take_noise();
	t = totals();
	history_tick(&history, k_uptime_get(), &t);
}

int core_set_channel(uint8_t channel)
{
	int err;

	take_noise();
	err = radio_set_channel(channel);
	if (err == 0) {
		core_count(CORE_CHANNEL_CHANGES);
	}
	return err;
}

const struct chan_survey *core_survey(void)
{
	static struct chan_survey survey;

	take_noise();
	core_watchdog_feed();
	return radio_survey(&survey) == 0 ? &survey : NULL;
}

static void clear(const struct diag_request *req)
{
	lock_reports();
	reports.merges = 0;
	unlock_reports(false);
	atomic_clear(&rx_dropped);
	atomic_clear(&usb_retries);
	memset(counts, 0, sizeof(counts));
	history_clear(&history, k_uptime_get(), &(struct history_totals){0});
	log_event(HISTORY_CLEAR, 0, 0, 0, 0);
	diag_reply(req, NULL, 0, DIAG_AFTER_NONE);
}

static void serve_minutes(const struct diag_request *req, uint8_t *payload)
{
	static struct history_minute page[DIAG_MINUTES_PER_PAGE];
	uint32_t first;
	size_t n = history_minutes(&history, req->arg, &first, page, ARRAY_SIZE(page));

	diag_reply(req, payload, diag_encode_minutes(payload, first, page, n), DIAG_AFTER_NONE);
}

static void serve_events(const struct diag_request *req, uint8_t *payload)
{
	static struct history_event page[DIAG_EVENTS_PER_PAGE];
	uint32_t first;
	size_t n = history_events(&history, req->arg, &first, page, ARRAY_SIZE(page));

	diag_reply(req, payload, diag_encode_events(payload, first, page, n), DIAG_AFTER_NONE);
}

static void serve_noise(const struct diag_request *req, uint8_t *payload)
{
	const struct chan_survey *survey;

	/* The receiver goes deaf during the survey, so a key held now stays down on the host. */
	core_release_keys();
	survey = core_survey();
	if (survey == NULL) {
		diag_reply_error(req, DIAG_ERR_RADIO, 0);
		return;
	}
	loop->radio_resumed();
	log_event(HISTORY_NOISE, 0, 0, 0, 0);
	diag_reply(req, payload, diag_encode_noise(payload, survey), DIAG_AFTER_NONE);
}

bool core_take_leds(uint8_t *leds)
{
	bool changed = atomic_clear(&leds_changed) != 0;

	*leds = (uint8_t)atomic_get(&host_leds);
	return changed;
}

void core_show_state(enum indicator_mode mode)
{
	indicator_set(usb_suspended() ? IND_SLEEP : mode);
}

static void usb_thread(void *a, void *b, void *c)
{
	enum report_iface iface = (enum report_iface)(uintptr_t)a;

	for (;;) {
		struct report r;
		bool have;

		lock_reports();
		have = reports_peek(&reports, iface, &r);
		if (have) {
			reports_pop(&reports, iface);
		}
		unlock_reports(false);
		if (!have) {
			k_sem_take(&reports_ready[iface], K_FOREVER);
			continue;
		}
		while (usb_send(&r) != 0 && usb_live()) {
			atomic_inc(&usb_retries);
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
	atomic_set(&leds_changed, 1);
}

void core_start(enum protocol protocol, uint32_t reset_cause)
{
	static const struct usb_callbacks usb_cb = {
		.on_live = on_usb_live,
		.on_leds = on_usb_leds,
		.on_diag_request = diag_on_request,
	};
	int err;

	active = protocol;
	boot_reset_cause = reset_cause;
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
	watchdog_start();
}

uint32_t core_reset_cause(void)
{
	return boot_reset_cause;
}

void core_fill_status(struct diag_status *st)
{
	*st = (struct diag_status){
		.uptime_ms = (uint64_t)k_uptime_get(),
		.reset_cause = boot_reset_cause,
		.protocol = active,
		.usb_live = usb_live(),
		.usb_suspended = usb_suspended(),
	};
	memcpy(st->firmware_version, core_firmware_version, sizeof(st->firmware_version));
	st->totals = totals();
	st->merges = core_merges();
	st->surveys = counts[CORE_SURVEYS];
	st->minute_next = history.minute_next;
	st->minute_oldest = history.minute_oldest;
	st->events_total = history.events_total;
	st->events_oldest = history.events_oldest;
}

static void serve_status(const struct diag_request *req, uint8_t *payload)
{
	struct diag_status st;

	core_fill_status(&st);
	loop->fill_status(&st);
	diag_reply(req, payload, diag_encode_status(payload, &st), DIAG_AFTER_NONE);
}

static void serve_protocol(const struct diag_request *req)
{
	enum protocol want;
	uint8_t reply[2] = {active, 0};
	int err;

	if (req->arg == 0) {
		diag_reply(req, reply, sizeof(reply), DIAG_AFTER_NONE);
		return;
	}
	if (!protocol_from_arg(req->arg, &want)) {
		diag_reply_error(req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		return;
	}
	if (want == active) {
		diag_reply(req, reply, sizeof(reply), DIAG_AFTER_NONE);
		return;
	}
	/* A protocol saved without its rebooting reply takes effect at an unrelated later reboot. */
	if (!diag_reply_fits(sizeof(reply))) {
		return;
	}
	err = store_save_protocol(want);
	if (err) {
		printk("saving the protocol failed: %d\n", err);
		diag_reply_error(req, DIAG_ERR_STORE, 0);
		return;
	}
	reply[1] = 1;
	diag_reply(req, reply, sizeof(reply), DIAG_AFTER_REBOOT);
}

bool core_take_request(struct diag_request *out)
{
	static uint8_t payload[DIAG_PAYLOAD_MAX];

	while (diag_take_request(out)) {
		uint8_t owner = diag_command_protocol(out->command);

		if (owner != 0 && owner != active) {
			diag_reply_error(out, DIAG_ERR_WRONG_PROTOCOL, active);
			continue;
		}
		switch (out->command) {
		case DIAG_STATUS:
			serve_status(out, payload);
			break;
		case DIAG_PROTOCOL:
			serve_protocol(out);
			break;
		case DIAG_REBOOT:
			diag_reply(out, NULL, 0, DIAG_AFTER_REBOOT);
			break;
		case DIAG_BOOTLOADER:
			diag_reply(out, NULL, 0, DIAG_AFTER_BOOTLOADER);
			break;
		case DIAG_HISTORY:
			serve_minutes(out, payload);
			break;
		case DIAG_EVENTS:
			serve_events(out, payload);
			break;
		case DIAG_NOISE:
			serve_noise(out, payload);
			break;
		case DIAG_CLEAR:
			clear(out);
			break;
		default:
			return true;
		}
	}
	return false;
}
