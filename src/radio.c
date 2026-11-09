#include <string.h>
#include <zephyr/kernel.h>
#include <esb.h>
#include <hal/nrf_radio.h>
#include "radio.h"

#define SURVEY_ROUND_GAP_MS 35

static radio_rx_cb rx_cb;
static uint8_t current_channel;

static void on_esb_event(const struct esb_evt *evt)
{
	struct esb_payload rx;

	if (evt->evt_id != ESB_EVENT_RX_RECEIVED) {
		return;
	}
	while (esb_read_rx_payload(&rx) == 0) {
		rx_cb(rx.data, rx.length, rx.pipe, -(int8_t)rx.rssi);
	}
}

int radio_start(const uint8_t dongle_id[4], uint8_t channel, radio_rx_cb cb)
{
	static const uint8_t base0[4] = {0x56, 0x56, 0x56, 0x56};
	static const uint8_t prefixes[8] = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58};
	struct esb_config config = ESB_DEFAULT_CONFIG;
	int err;

	rx_cb = cb;
	config.protocol = ESB_PROTOCOL_ESB_DPL;
	config.mode = ESB_MODE_PRX;
	config.event_handler = on_esb_event;
	config.bitrate = ESB_BITRATE_1MBPS;
	config.crc = ESB_CRC_8BIT;
	config.tx_output_power = 4;
	config.payload_length = RADIO_MAX_PAYLOAD;
	config.selective_auto_ack = false;

	err = esb_init(&config);
	if (!err) {
		err = esb_set_base_address_0(base0);
	}
	/* The keyboard addresses pipe 1 with the dongle ID it was given at pairing. */
	if (!err) {
		err = esb_set_base_address_1(dongle_id);
	}
	if (!err) {
		err = esb_set_prefixes(prefixes, ARRAY_SIZE(prefixes));
	}
	if (!err) {
		err = radio_set_channel(channel);
	}
	return err;
}

int radio_set_channel(uint8_t channel)
{
	int err;

	esb_stop_rx();
	esb_flush_tx();
	esb_flush_rx();
	err = esb_set_rf_channel(channel);
	if (err) {
		return err;
	}
	current_channel = channel;
	return esb_start_rx();
}

int radio_reply(uint8_t pipe, const uint8_t *frame, size_t len)
{
	struct esb_payload p = {.pipe = pipe, .length = len};

	memcpy(p.data, frame, len);
	return esb_write_payload(&p);
}

static int8_t sample_dbm(uint8_t channel)
{
	int8_t dbm;

	nrf_radio_frequency_set(NRF_RADIO, 2400 + channel);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_READY)) {
	}
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
	/* The RSSI filter needs about 15 us in RX before a sample is valid. */
	k_busy_wait(15);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND)) {
	}
	dbm = -(int8_t)nrf_radio_rssi_sample_get(NRF_RADIO);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
	}
	return dbm;
}

int radio_survey(struct chan_survey *s)
{
	int err = esb_stop_rx();

	if (err) {
		return err;
	}
	irq_disable(RADIO_IRQn);
	for (int round = 0; round < CHAN_SAMPLES; round++) {
		for (int i = 0; i < CHAN_COUNT; i++) {
			s->dbm[i][round] = sample_dbm(CHAN_FIRST + i);
		}
		k_msleep(SURVEY_ROUND_GAP_MS);
	}
	NVIC_ClearPendingIRQ(RADIO_IRQn);
	irq_enable(RADIO_IRQn);
	return radio_set_channel(current_channel);
}
