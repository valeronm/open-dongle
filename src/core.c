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

void core_clear_counters(void)
{
	lock_reports();
	reports.merges = 0;
	unlock_reports(false);
	atomic_clear(&rx_dropped);
	atomic_clear(&usb_retries);
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
	while (diag_take_request(out)) {
		uint8_t owner = diag_command_protocol(out->command);

		if (owner != 0 && owner != active) {
			diag_reply_error(out, DIAG_ERR_WRONG_PROTOCOL, active);
			continue;
		}
		switch (out->command) {
		case DIAG_PROTOCOL:
			serve_protocol(out);
			break;
		case DIAG_REBOOT:
			diag_reply(out, NULL, 0, DIAG_AFTER_REBOOT);
			break;
		case DIAG_BOOTLOADER:
			diag_reply(out, NULL, 0, DIAG_AFTER_BOOTLOADER);
			break;
		default:
			return true;
		}
	}
	return false;
}
