#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>
#include "core.h"
#include "diag.h"
#include "indicator.h"
#include "radio.h"
#include "store.h"
#include "unifying.h"
#include "usb.h"

#define TICK_MS 10

static struct unifying_session session;
static uint8_t next_nonce[UNIFYING_NONCE_LEN];

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

static void on_paired(void)
{
	uint8_t record[UNIFYING_RECORD_LEN];

	unifying_record_of(&session.pairing, record);
	printk("pairing saved: %d\n", store_save_unifying(record, sizeof(record)));
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
		core_submit(&out->report[i]);
	}
	if (out->release_all) {
		core_release_keys();
	}
}

static void apply_tick(const struct unifying_out *out)
{
	if (out->release_all) {
		core_release_keys();
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

static void show_state(uint64_t now)
{
	static const enum indicator_mode by_status[] = {
		[UNIFYING_NOT_PAIRED] = IND_IDLE,
		[UNIFYING_PAIRING] = IND_PAIRING,
		[UNIFYING_CONNECTED] = IND_CONNECTED,
		[UNIFYING_SILENT] = IND_CONNECTED,
	};

	core_show_state(by_status[unifying_session_status(&session, now)]);
}

static void fill_status(struct diag_status *st, uint64_t now)
{
	core_fill_status(st);
	st->link_state = unifying_session_status(&session, now);
	st->channel = session.channel;
	st->last_rssi = session.rssi;
	st->last_rx_age_ms = session.heard ? (uint32_t)MIN(now - session.heard_ms, UINT32_MAX - 1)
					   : UINT32_MAX;
	if (unifying_has_device(&session.pairing)) {
		memcpy(st->keyboard_id, session.pairing.device.wpid, UNIFYING_WPID_LEN);
	}
}

static void serve_diag(const struct diag_request *req, uint64_t now)
{
	static uint8_t payload[DIAG_PAYLOAD_MAX];
	struct diag_status st;

	switch (req->command) {
	case DIAG_STATUS:
		fill_status(&st, now);
		diag_reply(req, payload, diag_encode_status(payload, &st), DIAG_AFTER_NONE);
		break;
	case DIAG_PAIR:
		unifying_session_open_pairing(&session, now);
		printk("pairing open for %u s\n", UNIFYING_PAIRING_WINDOW_MS / 1000);
		diag_reply(req, NULL, 0, DIAG_AFTER_NONE);
		break;
	default:
		diag_reply_error(req, DIAG_ERR_UNKNOWN_COMMAND, 0);
		break;
	}
}

void unifying_run(int store_err)
{
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	struct radio_config rc;
	struct unifying_receiver receiver = {.device_octet = 0x07, .wpid = {0x88, 0x02}};
	uint8_t device_address[UNIFYING_ADDRESS_LEN];
	uint8_t record[UNIFYING_RECORD_LEN];
	uint32_t reported_drops = 0;
	bool stored;

	memcpy(receiver.base, &raw, sizeof(receiver.base));
	unifying_device_address(&receiver, device_address);
	stored = store_err == 0 && store_load_unifying(record, sizeof(record));
	unifying_session_init(&session, &receiver, stored ? record : NULL,
			      (uint64_t)k_uptime_get());
	if (stored) {
		printk("restored paired keyboard\n");
	}
	sys_rand_get(next_nonce, sizeof(next_nonce));

	rc = unifying_radio(device_address, session.channel);
	core_start_radio(&rc);
	print_hex("unifying receiver, assigned device address", device_address, 5);

	for (;;) {
		struct unifying_out out;
		struct diag_request req;
		struct rx_item item;
		uint32_t dropped;
		uint64_t now;
		uint8_t leds;
		bool changed;
		bool got;

		got = core_take_rx(&item, K_MSEC(TICK_MS));
		now = (uint64_t)k_uptime_get();
		if (got) {
			unifying_session_frame(&session, item.pipe, item.data, item.len, item.rssi, now,
					       next_nonce, aes_ecb, &out);
			apply_frame(&out, &item);
		}
		changed = core_take_leds(&leds);
		unifying_session_tick(&session, now, changed, leds, radio_replies_taken(), &out);
		apply_tick(&out);
		while (core_take_request(&req)) {
			serve_diag(&req, now);
		}
		dropped = core_rx_dropped();
		if (dropped != reported_drops) {
			printk("receive queue full, %u frames dropped\n", dropped - reported_drops);
			reported_drops = dropped;
		}
		show_state(now);
		core_watchdog_feed();
	}
}
