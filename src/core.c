#include <string.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "clock.h"
#include "core.h"
#include "diag.h"
#include "indicator.h"
#include "store.h"
#include "usb.h"

#define USB_RETRY_MS 2
#define SCAN_PERIOD_MS 250

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
static uint32_t packets[DEVICE_SLOTS];
static uint32_t gaps[DEVICE_SLOTS];
static uint32_t link_losses[DEVICE_SLOTS];
/* Below 0 for a slot that has sent nothing since boot. */
static int64_t last_input_ms[DEVICE_SLOTS];
static bool usb_was_suspended;
static bool usb_seen_live;
static struct chanwatch watch;
static int64_t last_scan_ms;
/* The keyboard sends a release on the channel a move leaves, so a key down at a move stays down on
 * the host until the keyboard finds the new one. */
#define MOVE_KEY_WAIT_MS 1000

static bool request_waiting;
static struct diag_request waiting_request;
static int64_t request_wait_until;

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
	struct rx_item item = {.received_us = clock_now_us(), .len = len, .pipe = pipe, .rssi = rssi};

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

bool core_submit(uint8_t slot, const struct report *r, const uint32_t *received_us)
{
	const struct report_stamp stamp =
		received_us ? (struct report_stamp){.slot = slot, .received_us = *received_us}
			    : REPORT_UNSTAMPED;
	bool changed;

	lock_reports();
	changed = reports_update(&reports, r, stamp);
	unlock_reports(false);
	if (changed) {
		k_sem_give(&reports_ready[reports_iface(r->kind)]);
		last_input_ms[slot] = k_uptime_get();
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

static struct history_totals totals(void)
{
	struct history_totals t = {
		.invalid = counts[CORE_INVALID],
		.dropped = core_rx_dropped(),
		.channel_changes = counts[CORE_CHANNEL_CHANGES],
		.usb_retries = core_usb_retries(),
	};

	memcpy(t.packets, packets, sizeof(t.packets));
	memcpy(t.gaps, gaps, sizeof(t.gaps));
	memcpy(t.link_losses, link_losses, sizeof(t.link_losses));
	return t;
}

static void log_event(uint8_t type, uint8_t slot, uint8_t a, uint8_t b, uint8_t c,
		      uint32_t value)
{
	history_event(&history, &(struct history_event){.time_ms = k_uptime_get(),
							.type = type,
							.slot = slot,
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
	bool on_home = home != CHANMOVE_NO_HOME && channel == home;

	history_noise(&history, &t, on_home);
	if (on_home) {
		chanwatch_home(&watch, channel, &t);
	}
}

void core_start_history(const struct core_loop *l)
{
	loop = l;
	for (int s = 0; s < DEVICE_SLOTS; s++) {
		last_input_ms[s] = -1;
	}
	history_init(&history, k_uptime_get(), &(struct history_totals){0});
	log_event(HISTORY_BOOT, DEVICE_NO_SLOT, 0, 0, 0, boot_reset_cause);
}

void core_count(enum core_counter c)
{
	counts[c]++;
}

void core_packet(uint8_t slot)
{
	packets[slot]++;
}

void core_gap(uint8_t slot)
{
	gaps[slot]++;
}

void core_link_lost(uint8_t slot)
{
	link_losses[slot]++;
}

void core_log(uint8_t type, uint8_t slot, uint8_t a, uint8_t b, uint8_t c, uint32_t value)
{
	take_noise();
	log_event(type, slot, a, b, c, value);
}

void core_log_pairing_opened(const struct pairwin *w)
{
	core_log(HISTORY_PAIRING_OPENED, DEVICE_NO_SLOT, w->cause, w->until_paired, 0, 0);
}

void core_rssi(uint8_t slot, int8_t dbm)
{
	take_noise();
	history_rssi(&history, slot, dbm);
}

uint8_t core_minutes_since_input(uint8_t slot)
{
	int64_t minutes;

	if (last_input_ms[slot] < 0) {
		return UINT8_MAX;
	}
	minutes = (k_uptime_get() - last_input_ms[slot]) / HISTORY_MINUTE_MS;
	return minutes > UINT8_MAX ? UINT8_MAX : (uint8_t)minutes;
}

/* The noise watch runs only under ESB, whose protocols have one slot. */
static uint8_t threshold_bucket(void)
{
	int8_t weakest;

	return history_weakest(&history, 0, &weakest) ? chanwatch_threshold_bucket(weakest)
						      : CHANWATCH_NO_THRESHOLD;
}

static void scan(void)
{
	int64_t now = k_uptime_get();
	const uint8_t *channels;
	int8_t dbm[CHANWATCH_MAX];
	size_t n;
	int home;

	if (!protocol_has_esb(active) || now - last_scan_ms < SCAN_PERIOD_MS) {
		return;
	}
	last_scan_ms = now;
	n = loop->candidates(&channels);
	chanwatch_set_channels(&watch, channels, n, (uint64_t)now);
	home = loop->movable();
	if (home == CHANMOVE_NO_HOME || watch.count == 0) {
		return;
	}
	/* Stopping the receiver drops the noise readings not yet taken. */
	take_noise();
	if (radio_sample(watch.channels, watch.count, dbm) != 0) {
		return;
	}
	loop->replies_dropped();
	chanwatch_round(&watch, dbm, (uint8_t)home, (uint64_t)now);
	chanwatch_decide(&watch, (uint8_t)home, threshold_bucket(), (uint64_t)now);
}

static bool keys_held(void)
{
	bool held;

	lock_reports();
	held = reports_any_held(&reports);
	unlock_reports(false);
	return held;
}

static enum diag_error apply_move(uint8_t to, uint8_t reason, uint32_t value, uint8_t *from)
{
	enum chanmove_result result = loop->move(to, from);

	if (result == CHANMOVE_NOT_A_CHANNEL) {
		return DIAG_ERR_BAD_CHANNELS;
	}
	if (result == CHANMOVE_NOT_MOVABLE) {
		return DIAG_ERR_NOT_MOVABLE;
	}
	if (to == *from) {
		return 0;
	}
	if (core_set_channel(to) != 0) {
		return DIAG_ERR_RADIO;
	}
	chanwatch_hold(&watch, (uint64_t)k_uptime_get());
	core_log(HISTORY_LINK_MOVED, DEVICE_NO_SLOT, *from, to, reason, value);
	return 0;
}

static void poll_noise_move(void)
{
	struct chanwatch_move m;
	uint8_t from;

	if (!watch.move_waiting || keys_held() || !chanwatch_take_move(&watch, &m)) {
		return;
	}
	apply_move(m.to, HISTORY_MOVED_FOR_NOISE, (uint32_t)m.home_share << 16 | m.to_share,
		   &from);
}

static void poll_move_request(void)
{
	enum diag_error err;
	uint8_t from;

	if (!request_waiting) {
		return;
	}
	if (keys_held()) {
		if (k_uptime_get() >= request_wait_until) {
			request_waiting = false;
			diag_reply_error(&waiting_request, DIAG_ERR_KEY_HELD, 0);
		}
		return;
	}
	request_waiting = false;
	err = apply_move((uint8_t)waiting_request.arg, HISTORY_MOVED_ON_REQUEST, 0, &from);
	if (err != 0) {
		diag_reply_error(&waiting_request, err, 0);
	} else {
		diag_reply(&waiting_request, &from, 1, DIAG_AFTER_NONE);
	}
}

void core_poll(void)
{
	bool suspended = usb_suspended();
	struct history_totals t;

	usb_seen_live |= usb_live();
	if (suspended != usb_was_suspended) {
		if (usb_seen_live) {
			core_log(suspended ? HISTORY_USB_SUSPEND : HISTORY_USB_RESUME,
				 DEVICE_NO_SLOT, 0, 0, 0, 0);
		}
		usb_was_suspended = suspended;
	}
	take_noise();
	scan();
	poll_noise_move();
	poll_move_request();
	t = totals();
	history_tick(&history, k_uptime_get(), &t);
}

int core_set_channel(uint8_t channel)
{
	int err;

	if (!protocol_has_esb(active)) {
		return -ENOTSUP;
	}
	take_noise();
	err = radio_set_channel(channel);
	if (err == 0) {
		core_count(CORE_CHANNEL_CHANGES);
		loop->replies_dropped();
	}
	return err;
}

const struct chan_survey *core_survey(void)
{
	static struct chan_survey survey;

	if (!protocol_has_esb(active)) {
		return NULL;
	}
	take_noise();
	core_watchdog_feed();
	if (radio_survey(&survey) != 0) {
		return NULL;
	}
	loop->replies_dropped();
	return &survey;
}

static void clear(const struct diag_request *req)
{
	lock_reports();
	reports.merges = 0;
	memset(reports.latency, 0, sizeof(reports.latency));
	unlock_reports(false);
	atomic_clear(&rx_dropped);
	atomic_clear(&usb_retries);
	memset(counts, 0, sizeof(counts));
	memset(packets, 0, sizeof(packets));
	memset(gaps, 0, sizeof(gaps));
	memset(link_losses, 0, sizeof(link_losses));
	history_clear(&history, k_uptime_get(), &(struct history_totals){0});
	log_event(HISTORY_CLEAR, DEVICE_NO_SLOT, 0, 0, 0, 0);
	diag_reply(req, NULL, 0, DIAG_AFTER_NONE);
}

static bool slot_asked(const struct diag_request *req, uint8_t *slot)
{
	*slot = req->data[0];
	if (*slot >= loop->slot_count) {
		diag_reply_error(req, DIAG_ERR_NO_SUCH_DEVICE, 0);
		return false;
	}
	return true;
}

static void serve_minutes(const struct diag_request *req, uint8_t *payload)
{
	static struct history_minute page[DIAG_MINUTES_PER_PAGE];
	uint32_t first;
	uint8_t slot;
	size_t n;

	if (!slot_asked(req, &slot)) {
		return;
	}
	n = history_minutes(&history, req->arg, &first, page, ARRAY_SIZE(page));
	diag_reply(req, payload, diag_encode_minutes(payload, first, page, n, slot),
		   DIAG_AFTER_NONE);
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
	log_event(HISTORY_NOISE, DEVICE_NO_SLOT, 0, 0, 0, 0);
	diag_reply(req, payload, diag_encode_noise(payload, survey), DIAG_AFTER_NONE);
}

static void serve_watch(const struct diag_request *req, uint8_t *payload)
{
	int home = loop->home();

	diag_reply(req, payload,
		   diag_encode_watch(payload, &watch,
				     home == CHANMOVE_NO_HOME ? 0 : (uint8_t)home, threshold_bucket(),
				     loop->movable() == CHANMOVE_NO_HOME,
				     (uint64_t)k_uptime_get()),
		   DIAG_AFTER_NONE);
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
		struct report_entry e;
		bool have;
		int err;

		lock_reports();
		have = reports_peek(&reports, iface, &e);
		if (have) {
			reports_pop(&reports, iface);
		}
		unlock_reports(false);
		if (!have) {
			k_sem_take(&reports_ready[iface], K_FOREVER);
			continue;
		}
		while ((err = usb_send(&e.report)) != 0 && usb_live()) {
			atomic_inc(&usb_retries);
			k_msleep(USB_RETRY_MS);
		}
		if (err == 0) {
			lock_reports();
			reports_delivered(&reports, &e, clock_now_us());
			unlock_reports(false);
		}
	}
}

K_THREAD_DEFINE(keyboard_tid, 1024, usb_thread, (void *)REPORT_IFACE_KEYBOARD, NULL, NULL, 5, 0,
		SYS_FOREVER_MS);
K_THREAD_DEFINE(extra_tid, 1024, usb_thread, (void *)REPORT_IFACE_EXTRA, NULL, NULL, 5, 0,
		SYS_FOREVER_MS);
K_THREAD_DEFINE(mouse_tid, 1024, usb_thread, (void *)REPORT_IFACE_MOUSE, NULL, NULL, 5, 0,
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
	k_thread_start(mouse_tid);
	watchdog_start();
}

uint32_t core_reset_cause(void)
{
	return boot_reset_cause;
}

void core_fill_status(struct diag_status *st, uint8_t slot)
{
	struct history_totals t = totals();
	struct report_latency l;
	uint32_t merges;

	lock_reports();
	merges = reports.merges;
	l = reports.latency[slot];
	unlock_reports(false);
	*st = (struct diag_status){
		.uptime_ms = (uint64_t)k_uptime_get(),
		.reset_cause = boot_reset_cause,
		.protocol = active,
		.usb_live = usb_live(),
		.usb_suspended = usb_suspended(),
		.invalid = t.invalid,
		.dropped = t.dropped,
		.channel_changes = t.channel_changes,
		.usb_retries = t.usb_retries,
		.slot_count = loop->slot_count,
		.slot = slot,
		.last_rssi = DIAG_NO_RSSI,
		.last_rx_age_ms = UINT32_MAX,
		.packets = t.packets[slot],
		.gaps = t.gaps[slot],
		.link_losses = t.link_losses[slot],
		.latency_average_us = reports_latency_average_us(&l),
		.latency_max_us = l.max_us,
	};
	memcpy(st->firmware_version, core_firmware_version, sizeof(st->firmware_version));
	loop->slots(st->slots);
	st->merges = merges;
	st->surveys = counts[CORE_SURVEYS];
	st->minute_next = history.minute_next;
	st->minute_oldest = history.minute_oldest;
	st->events_total = history.events_total;
	st->events_oldest = history.events_oldest;
}

static void serve_status(const struct diag_request *req, uint8_t *payload)
{
	struct diag_status st;
	uint8_t slot;

	if (!slot_asked(req, &slot)) {
		return;
	}
	core_fill_status(&st, slot);
	loop->fill_status(&st, slot);
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

static void serve_move(const struct diag_request *req)
{
	if (req->arg > UINT8_MAX) {
		diag_reply_error(req, DIAG_ERR_BAD_CHANNELS, 0);
		return;
	}
	waiting_request = *req;
	request_waiting = true;
	request_wait_until = k_uptime_get() + MOVE_KEY_WAIT_MS;
	poll_move_request();
}

bool core_take_request(struct diag_request *out)
{
	static uint8_t payload[DIAG_PAYLOAD_MAX];

	while (diag_take_request(out)) {
		if (!diag_command_answered(out->command, active)) {
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
		case DIAG_MOVE:
			serve_move(out);
			break;
		case DIAG_WATCH:
			serve_watch(out, payload);
			break;
		default:
			return true;
		}
	}
	return false;
}
