#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>
#include "chanpick.h"
#include "link.h"
#include "protocol.h"
#include "radio.h"
#include "store.h"

#define TICK_MS 10

struct rx_item {
	uint8_t len;
	uint8_t pipe;
	int8_t rssi;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_item), 64, 4);

static struct link link;
static struct chan_survey survey;
static uint32_t rx_dropped;

static void on_radio_rx(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi)
{
	struct rx_item item = {.len = len, .pipe = pipe, .rssi = rssi};

	memcpy(item.data, data, MIN(len, sizeof(item.data)));
	if (k_msgq_put(&rx_queue, &item, K_NO_WAIT) != 0) {
		rx_dropped++;
	}
}

static void apply(const struct link_out *out)
{
	if (out->set_channel) {
		radio_set_channel(out->channel);
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
		printk("link lost\n");
	}
}

static void run_survey(void)
{
	uint8_t picked[3];
	struct link_out out = {0};
	uint32_t now;

	if (radio_survey(&survey) != 0) {
		link_survey_failed(&link, k_uptime_get_32(), &out);
		apply(&out);
		return;
	}
	now = k_uptime_get_32();
	chan_pick(&survey, picked);
	printk("survey picked %u %u %u\n", picked[0], picked[1], picked[2]);
	link_on_survey(&link, picked, now, &out);
	apply(&out);
}

static void handle(const struct rx_item *item)
{
	struct proto_msg m = proto_parse(item->data, item->len);
	struct link_out out = {0};

	printk("ch%u pipe%u %d dBm kind %d\n", link.channel, item->pipe, item->rssi, m.kind);
	if (m.kind == PROTO_INVALID) {
		return;
	}
	link_on_message(&link, &m, item->pipe, k_uptime_get_32(), &out);
	apply(&out);
	if (out.start_survey) {
		run_survey();
	}
}

/* Console output written before the host opens the port is dropped. */
static void wait_for_host(void)
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	for (int i = 0; i < 100 && !dtr; i++) {
		uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}
}

int main(void)
{
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	uint8_t id[4];
	uint8_t own[3];
	struct link_out out = {0};
	bool paired;
	int store_err;
	int err;

	wait_for_host();
	memcpy(id, &raw, sizeof(id));
	store_err = store_init();
	paired = store_err == 0 && store_load_channels(own);
	link_init(&link, id, paired ? own : NULL, k_uptime_get_32(), &out);
	err = radio_start(id, out.channel, on_radio_rx);
	printk("nuphy-dongle id %02x%02x%02x%02x %s, store %d, radio %d\n", id[0], id[1], id[2],
	       id[3], paired ? "paired" : "never paired", store_err, err);

	for (;;) {
		struct rx_item item;

		if (k_msgq_get(&rx_queue, &item, K_MSEC(TICK_MS)) == 0) {
			handle(&item);
		}
		out = (struct link_out){0};
		link_tick(&link, k_uptime_get_32(), &out);
		apply(&out);
	}
}
