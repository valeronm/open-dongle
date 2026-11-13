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

#define TICK_MS 10
#define WATCHDOG_TIMEOUT_MS 3000
#define USB_RETRY_MS 2

static struct unifying_session session;
static struct reports reports;
static atomic_t host_leds;
static atomic_t led_changed;
static uint8_t next_nonce[UNIFYING_NONCE_LEN];
static atomic_t rx_dropped;

struct rx_item {
	uint8_t len;
	uint8_t pipe;
	int8_t rssi;
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
	struct rx_item item = {.len = len, .pipe = pipe, .rssi = rssi};

	memcpy(item.data, data, MIN(len, sizeof(item.data)));
	if (k_msgq_put(&rx_queue, &item, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	}
}

static void on_paired(void)
{
	uint8_t record[UNIFYING_RECORD_LEN];

	unifying_record_of(&session.pairing, record);
	printk("pairing saved: %d\n", store_save_unifying(record, sizeof(record)));
	indicator_set(IND_CONNECTED);
}

static void report_pairing(enum unifying_rx_event event, const uint8_t *reply, size_t n)
{
	const struct unifying_attempt *a = &session.pairing.attempt;

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
		break;
	default:
		break;
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

static void release_all_keys(void)
{
	lock_reports();
	reports_release_all(&reports);
	unlock_reports(true);
}

static void queue_leds(uint8_t leds)
{
	uint8_t frame[UNIFYING_LED_FRAME_LEN];
	uint32_t taken;
	size_t n = unifying_led_frame(leds, frame);

	taken = radio_replies_taken();
	if (radio_reply(UNIFYING_PIPE_DEVICE, frame, n) == 0) {
		unifying_session_led_queued(&session, leds, taken);
	}
}

static void apply_frame(const struct unifying_out *out, const struct rx_item *item)
{
	if (out->event == UNIFYING_RX_ANSWERED_1) {
		sys_rand_get(next_nonce, sizeof(next_nonce));
	}
	if (out->event == UNIFYING_RX_REFUSED) {
		print_hex("request 1 refused, not Unifying-compatible:", item->data, item->len);
	}
	if (out->reply_len != 0) {
		int err = radio_reply(out->reply_pipe, out->reply, out->reply_len);

		if (err) {
			printk("queueing reply failed: %d\n", err);
		}
		report_pairing(out->event, out->reply, out->reply_len);
	}
	if (out->event == UNIFYING_RX_PAIRED) {
		on_paired();
	}
	for (size_t i = 0; i < out->reports; i++) {
		submit(&out->report[i]);
	}
	if (out->release_all) {
		release_all_keys();
	}
}

static void apply_tick(const struct unifying_out *out)
{
	if (out->release_all) {
		release_all_keys();
	}
	if (out->send_led) {
		queue_leds(out->leds);
	}
	if (out->attempt_expired) {
		printk("pairing timed out\n");
	}
	if (out->set_channel && radio_set_channel(out->channel) == 0) {
		unifying_session_channel_moved(&session);
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

static void no_diag(const uint8_t *buf, size_t len)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
}

static struct radio_config unifying_radio(const uint8_t device[UNIFYING_ADDRESS_LEN],
					  uint8_t channel)
{
	const uint8_t *pairing = unifying_pairing_address;

	/* The last octet of an address is its prefix. */
	return (struct radio_config){
		.rate = RADIO_2MBPS,
		.crc = RADIO_CRC_16,
		.base0 = {pairing[3], pairing[2], pairing[1], pairing[0]},
		.base1 = {device[3], device[2], device[1], device[0]},
		.prefixes = {pairing[4], device[4], 0x00},
		.pipes = 3,
		.channel = channel,
	};
}

int main(void)
{
	static const struct usb_callbacks usb_cb = {
		.on_live = on_live,
		.on_leds = on_leds,
		.on_diag_request = no_diag,
	};
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	struct radio_config rc;
	struct unifying_receiver receiver = {.device_octet = 0x07, .wpid = {0x88, 0x02}};
	uint8_t device_address[UNIFYING_ADDRESS_LEN];
	uint8_t record[UNIFYING_RECORD_LEN];
	bool stored;
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
	stored = err == 0 && store_load_unifying(record, sizeof(record));
	unifying_session_init(&session, &receiver, stored ? record : NULL,
			      (uint64_t)k_uptime_get());
	if (stored) {
		printk("restored paired keyboard\n");
	}
	unifying_session_open_pairing(&session, 0);
	watchdog_start();
	sys_rand_get(next_nonce, sizeof(next_nonce));

	rc = unifying_radio(device_address, session.channel);
	while ((err = radio_start(&rc, on_radio_rx)) != 0) {
		indicator_set(IND_ERROR);
		printk("radio start failed: %d\n", err);
		k_sleep(K_SECONDS(1));
		watchdog_feed();
	}
	if (unifying_has_device(&session.pairing)) {
		indicator_set(IND_CONNECTED);
	} else {
		indicator_set(IND_PAIRING);
	}
	print_hex("unifying poc sweeping pairing channels, assigned device address", device_address, 5);

	for (;;) {
		struct unifying_out out;
		struct rx_item item;
		uint64_t now;
		bool changed;
		bool got;

		got = k_msgq_get(&rx_queue, &item, K_MSEC(TICK_MS)) == 0;
		now = (uint64_t)k_uptime_get();
		if (got) {
			unifying_session_frame(&session, item.pipe, item.data, item.len, item.rssi, now,
					       next_nonce, aes_ecb, &out);
			apply_frame(&out, &item);
		}
		changed = atomic_clear(&led_changed) != 0;
		unifying_session_tick(&session, now, changed, (uint8_t)atomic_get(&host_leds),
				      radio_replies_taken(), &out);
		apply_tick(&out);
		if (atomic_get(&rx_dropped) != 0) {
			printk("receive queue full, %ld frames dropped\n", atomic_clear(&rx_dropped));
		}
		watchdog_feed();
	}
}
