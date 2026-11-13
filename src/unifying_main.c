#include <string.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>
#include "indicator.h"
#include "radio.h"
#include "reports.h"
#include "store.h"
#include "unifying.h"
#include "usb.h"

#define PIPE_DEVICE 1
#define PIPE_RECEIVER 2
#define TICK_MS 10
#define WATCHDOG_TIMEOUT_MS 3000
#define USB_RETRY_MS 2

static struct unifying_receiver receiver = {.device_octet = 0x07, .wpid = {0x88, 0x02}};
static struct unifying_pairing pairing;
static struct unifying_link link;
static struct reports reports;
static atomic_t host_leds;
static atomic_t led_changed;
static struct unifying_led led;
static uint8_t next_nonce[UNIFYING_NONCE_LEN];
static atomic_t rx_dropped;

struct rx_item {
	uint8_t len;
	uint8_t pipe;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_item), 64, 4);
K_MUTEX_DEFINE(reports_lock);
static struct k_sem reports_ready[REPORT_IFACES];

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

static void print_hex(const char *label, const uint8_t *buf, size_t len)
{
	printk("%s", label);
	for (size_t i = 0; i < len; i++) {
		printk(" %02x", buf[i]);
	}
	printk("\n");
}

static struct {
	uint8_t key[16];
	uint8_t clear[16];
	uint8_t cipher[16];
} ecb_block;

static void aes_ecb(const uint8_t key[UNIFYING_KEY_LEN], const uint8_t in[UNIFYING_AES_BLOCK_LEN],
		    uint8_t out[UNIFYING_AES_BLOCK_LEN])
{
	memcpy(ecb_block.key, key, sizeof(ecb_block.key));
	memcpy(ecb_block.clear, in, sizeof(ecb_block.clear));
	NRF_ECB->EVENTS_ENDECB = 0;
	NRF_ECB->EVENTS_ERRORECB = 0;
	NRF_ECB->ECBDATAPTR = (uint32_t)(uintptr_t)&ecb_block;
	NRF_ECB->TASKS_STARTECB = 1;
	while (NRF_ECB->EVENTS_ENDECB == 0 && NRF_ECB->EVENTS_ERRORECB == 0) {
	}
	memcpy(out, ecb_block.cipher, sizeof(ecb_block.cipher));
}

static void on_radio_rx(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi)
{
	struct rx_item item = {.len = len, .pipe = pipe};

	ARG_UNUSED(rssi);

	memcpy(item.data, data, MIN(len, sizeof(item.data)));
	if (k_msgq_put(&rx_queue, &item, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	}
}

static void on_paired(void)
{
	uint8_t record[UNIFYING_RECORD_LEN];

	unifying_link_reset(&link);
	unifying_record_of(&pairing, record);
	printk("pairing saved: %d\n", store_save_unifying(record, sizeof(record)));
	indicator_set(IND_CONNECTED);
}

static void report_pairing(enum unifying_rx_event event, const uint8_t *reply, size_t n)
{
	const struct unifying_attempt *a = &pairing.attempt;

	switch (event) {
	case UNIFYING_RX_ANSWERED_1:
		printk("request 1: id %02x wpid %02x%02x type %02x caps %02x\n", a->id,
		       a->dev.wpid[0], a->dev.wpid[1], a->device_type, a->caps);
		print_hex("reply", reply, n);
		break;
	case UNIFYING_RX_ANSWERED_2:
		printk("request 2: id %02x\n", a->id);
		break;
	case UNIFYING_RX_PAIRED:
		printk("request 3: name \"%.*s\", paired\n", a->name_len, a->name);
		on_paired();
		break;
	default:
		break;
	}
}

static void handle_pairing(const struct rx_item *item, uint64_t now)
{
	uint8_t reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event event;
	size_t n;
	int err;

	n = unifying_pairing_rx(&pairing, &receiver, next_nonce, item->data, item->len,
				now, reply, &event);
	if (event == UNIFYING_RX_ANSWERED_1) {
		sys_rand_get(next_nonce, sizeof(next_nonce));
	}
	if (event == UNIFYING_RX_REFUSED) {
		print_hex("request 1 refused, not Unifying-compatible:", item->data, item->len);
		return;
	}
	if (n == 0) {
		return;
	}
	err = radio_reply(item->pipe, reply, n);
	if (err) {
		printk("queueing reply failed: %d\n", err);
	}
	report_pairing(event, reply, n);
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

static void submit(const struct report *r)
{
	bool changed;

	lock_reports();
	changed = reports_update(&reports, r);
	unlock_reports(changed);
	if (changed && usb_suspended()) {
		usb_wakeup();
	}
}

static void handle_keyboard(const struct rx_item *item, uint64_t now)
{
	struct report typed[UNIFYING_KEYSTROKE_REPORTS];
	size_t n;

	unifying_link_heard(&link, item->data, item->len, now);
	if (!unifying_has_device(&pairing)) {
		return;
	}
	n = unifying_keystroke(&link, pairing.device.key, item->data, item->len, now, aes_ecb,
			       typed);
	if (n == 0 && unifying_media_report(&link, item->data, item->len, &typed[0])) {
		n = 1;
	}
	for (size_t i = 0; i < n; i++) {
		submit(&typed[i]);
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
			k_msleep(USB_RETRY_MS);
		}
	}
}

K_THREAD_DEFINE(keyboard_tid, 1024, usb_thread, (void *)REPORT_IFACE_KEYBOARD, NULL, NULL, 5, 0,
		SYS_FOREVER_MS);
K_THREAD_DEFINE(extra_tid, 1024, usb_thread, (void *)REPORT_IFACE_EXTRA, NULL, NULL, 5, 0,
		SYS_FOREVER_MS);

static void on_live(void)
{
	lock_reports();
	reports_set_live(&reports, usb_live());
	unlock_reports(true);
}

static void on_leds(uint8_t leds)
{
	atomic_set(&host_leds, leds);
	atomic_set(&led_changed, 1);
}

static void send_leds(void)
{
	uint8_t frame[UNIFYING_LED_FRAME_LEN];
	uint8_t leds;
	uint32_t taken;
	size_t n;

	if (atomic_clear(&led_changed) != 0) {
		unifying_led_want(&led, (uint8_t)atomic_get(&host_leds));
	}
	if (!unifying_led_due(&led, unifying_has_device(&pairing), radio_replies_taken(),
			      &leds)) {
		return;
	}
	n = unifying_led_frame(leds, frame);
	taken = radio_replies_taken();
	if (radio_reply(PIPE_DEVICE, frame, n) == 0) {
		unifying_led_sent(&led, leds, taken);
	}
}

static void no_diag(const uint8_t *buf, size_t len)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
}

int main(void)
{
	static const struct usb_callbacks usb_cb = {
		.on_live = on_live,
		.on_leds = on_leds,
		.on_diag_request = no_diag,
	};
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	struct unifying_sweep sweep = {0};
	uint8_t device_address[UNIFYING_ADDRESS_LEN];
	uint8_t record[UNIFYING_RECORD_LEN];
	uint8_t channel;
	int err;

	memcpy(receiver.base, &raw, sizeof(receiver.base));
	unifying_device_address(&receiver, device_address);
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
	err = store_init();
	if (err) {
		printk("store init failed: %d\n", err);
	}
	unifying_link_reset(&link);
	if (err == 0 && store_load_unifying(record, sizeof(record))) {
		unifying_record_restore(&pairing, record);
		printk("restored paired keyboard\n");
	}
	watchdog_start();
	sys_rand_get(next_nonce, sizeof(next_nonce));

	channel = unifying_sweep_channel(&sweep, &pairing, (uint64_t)k_uptime_get());
	while ((err = radio_start_unifying(unifying_pairing_address, device_address, channel,
					   on_radio_rx)) != 0) {
		indicator_set(IND_ERROR);
		printk("radio start failed: %d\n", err);
		k_sleep(K_SECONDS(1));
		watchdog_feed();
	}
	if (unifying_has_device(&pairing)) {
		indicator_set(IND_CONNECTED);
	} else {
		indicator_set(IND_PAIRING);
	}
	print_hex("unifying poc sweeping pairing channels, assigned device address", device_address, 5);

	for (;;) {
		struct rx_item item;
		uint64_t now;
		uint8_t next;
		bool got;

		got = k_msgq_get(&rx_queue, &item, K_MSEC(TICK_MS)) == 0;
		now = (uint64_t)k_uptime_get();
		if (got) {
			handle_pairing(&item, now);
			if (item.pipe == PIPE_DEVICE) {
				handle_keyboard(&item, now);
			} else if (item.pipe == PIPE_RECEIVER) {
				if (unifying_link_reconnect(&link, receiver.device_octet, item.data,
							    item.len)) {
					lock_reports();
					reports_release_all(&reports);
					unlock_reports(true);
				}
				unifying_led_resend(&led);
			}
		}
		if (unifying_link_silent(&link, now)) {
			lock_reports();
			reports_release_all(&reports);
			unlock_reports(true);
		}
		send_leds();
		if (unifying_pairing_expired(&pairing, now)) {
			printk("pairing timed out\n");
		}
		next = unifying_sweep_channel(&sweep, &pairing, now);
		if (next != channel) {
			if (radio_set_channel(next) == 0) {
				unifying_led_dropped(&led);
			}
			channel = next;
		}
		if (atomic_get(&rx_dropped) != 0) {
			printk("receive queue full, %ld frames dropped\n", atomic_clear(&rx_dropped));
		}
		watchdog_feed();
	}
}
