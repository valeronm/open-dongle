#include <string.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>
#include "indicator.h"
#include "radio.h"
#include "unifying.h"
#include "usb.h"

#define PIPE_DEVICE 1
#define TICK_MS 10
#define WATCHDOG_TIMEOUT_MS 3000

static struct unifying_receiver receiver = {.device_octet = 0x07, .wpid = {0x88, 0x02}};
static struct unifying_pairing pairing;
static uint8_t next_nonce[UNIFYING_NONCE_LEN];
static atomic_t rx_dropped;

struct rx_item {
	uint8_t len;
	uint8_t pipe;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_item), 64, 4);

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

static void on_radio_rx(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi)
{
	struct rx_item item = {.len = len, .pipe = pipe};

	ARG_UNUSED(rssi);

	memcpy(item.data, data, MIN(len, sizeof(item.data)));
	if (k_msgq_put(&rx_queue, &item, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	}
}

static void report_pairing(void)
{
	switch (pairing.state) {
	case UNIFYING_ANSWERED_1:
		printk("request 1: id %02x wpid %02x%02x type %02x caps %02x\n", pairing.id,
		       pairing.device_wpid[0], pairing.device_wpid[1], pairing.device_type,
		       pairing.caps);
		break;
	case UNIFYING_ANSWERED_2:
		print_hex("request 2: device nonce", pairing.device_nonce, UNIFYING_NONCE_LEN);
		print_hex("dongle nonce", pairing.dongle_nonce, UNIFYING_NONCE_LEN);
		print_hex("key", pairing.key, UNIFYING_KEY_LEN);
		break;
	case UNIFYING_PAIRED:
		printk("request 3: name \"%.*s\", paired\n", pairing.name_len, pairing.name);
		indicator_set(IND_CONNECTED);
		break;
	default:
		break;
	}
}

static void handle_pairing(const struct rx_item *item)
{
	uint8_t reply[UNIFYING_FRAME_MAX];
	bool refused;
	size_t n;
	int err;

	n = unifying_pairing_rx(&pairing, &receiver, next_nonce, item->data, item->len,
				k_uptime_get_32(), reply, &refused);
	if (n != 0 && pairing.state == UNIFYING_ANSWERED_1) {
		sys_rand_get(next_nonce, sizeof(next_nonce));
	}
	if (refused) {
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
	report_pairing();
	print_hex("reply", reply, n);
}

static void no_live(void)
{
}

static void no_leds(uint8_t leds)
{
	ARG_UNUSED(leds);
}

static void no_diag(const uint8_t *buf, size_t len)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
}

int main(void)
{
	static const struct usb_callbacks usb_cb = {
		.on_live = no_live,
		.on_leds = no_leds,
		.on_diag_request = no_diag,
	};
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	struct unifying_sweep sweep = {0};
	uint8_t device_address[UNIFYING_ADDRESS_LEN];
	uint8_t channel;
	int err;

	memcpy(receiver.base, &raw, sizeof(receiver.base));
	unifying_device_address(&receiver, device_address);
	err = usb_start(&usb_cb);
	if (err) {
		printk("usb start failed: %d\n", err);
	}
	watchdog_start();
	sys_rand_get(next_nonce, sizeof(next_nonce));

	channel = unifying_sweep_channel(&sweep, &pairing, k_uptime_get_32());
	while ((err = radio_start_unifying(unifying_pairing_address, device_address, channel,
					   on_radio_rx)) != 0) {
		indicator_set(IND_ERROR);
		printk("radio start failed: %d\n", err);
		k_sleep(K_SECONDS(1));
		watchdog_feed();
	}
	indicator_set(IND_PAIRING);
	print_hex("unifying poc sweeping pairing channels, assigned device address", device_address, 5);

	for (;;) {
		struct rx_item item;
		uint32_t now;
		uint8_t next;

		if (k_msgq_get(&rx_queue, &item, K_MSEC(TICK_MS)) == 0) {
			handle_pairing(&item);
			if (item.pipe == PIPE_DEVICE &&
			    unifying_is_encrypted_keyboard(item.data, item.len)) {
				print_hex("encrypted keyboard frame, counter", &item.data[10], 4);
			}
		}
		now = k_uptime_get_32();
		if (unifying_pairing_expired(&pairing, now)) {
			printk("pairing timed out\n");
		}
		next = unifying_sweep_channel(&sweep, &pairing, now);
		if (next != channel) {
			radio_set_channel(next);
			channel = next;
		}
		if (atomic_get(&rx_dropped) != 0) {
			printk("receive queue full, %ld frames dropped\n", atomic_clear(&rx_dropped));
		}
		watchdog_feed();
	}
}
