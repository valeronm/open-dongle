#include <string.h>
#include <zephyr/kernel.h>
#include <esb.h>
#include <hal/nrf_radio.h>
#include "radio.h"

#define SURVEY_ROUND_GAP_MS 35
#define ACK_WAIT_MAX_US 1000
#define ACK_POLL_US 10
#define NOISE_TICK_US 500

static radio_rx_cb rx_cb;
static uint8_t current_channel;
static struct noise_tally noise;
static volatile bool listening;
static volatile uint32_t listen_generation;
static volatile uint32_t payloads_seen;
static atomic_t replies_taken;

static void on_esb_event(const struct esb_evt *evt)
{
	struct esb_payload rx;

	if (evt->evt_id == ESB_EVENT_TX_SUCCESS) {
		atomic_inc(&replies_taken);
		return;
	}
	if (evt->evt_id != ESB_EVENT_RX_RECEIVED) {
		return;
	}
	while (esb_read_rx_payload(&rx) == 0) {
		payloads_seen++;
		rx_cb(rx.data, rx.length, rx.pipe, -(int8_t)rx.rssi);
	}
}

static bool transmitting(void)
{
	switch (nrf_radio_state_get(NRF_RADIO)) {
	case NRF_RADIO_STATE_TXRU:
	case NRF_RADIO_STATE_TXIDLE:
	case NRF_RADIO_STATE_TX:
	case NRF_RADIO_STATE_TXDISABLE:
		return true;
	default:
		return false;
	}
}

/* ESB reports a packet before sending its acknowledgment, which may carry a queued reply. */
static int stop_rx(void)
{
	unsigned int key;

	for (int i = 0; i < ACK_WAIT_MAX_US / ACK_POLL_US && transmitting(); i++) {
		k_busy_wait(ACK_POLL_US);
	}
	key = irq_lock();
	listening = false;
	listen_generation++;
	noise = (struct noise_tally){0};
	irq_unlock(key);
	return esb_stop_rx();
}

static void rssi_start(void)
{
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
}

static int8_t rssi_dbm(void)
{
	return -(int8_t)nrf_radio_rssi_sample_get(NRF_RADIO);
}

/* A reading starts only after a tick that saw no packet, and is kept only when the two ticks
 * after its start saw none either; a restart of reception counts as a packet. */
static void noise_tick(struct k_timer *timer)
{
	static bool started;
	static bool held;
	static int8_t held_dbm;
	static uint32_t last_payloads;
	static uint32_t last_generation;
	uint32_t payloads = payloads_seen;
	uint32_t generation = listen_generation;
	bool address = nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	bool packet = address || payloads != last_payloads || generation != last_generation;
	bool rx;

	ARG_UNUSED(timer);
	if (address) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	}
	last_payloads = payloads;
	last_generation = generation;
	if (held && !packet) {
		noise_add(&noise, held_dbm);
	}
	rx = nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RX;
	held = started && !packet && rx &&
	       nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
	if (held) {
		held_dbm = rssi_dbm();
	}
	/* While reception is stopped the radio may be sampled by hand, which waits on RSSIEND. */
	started = listening && rx && !packet;
	if (started) {
		rssi_start();
	}
}

K_TIMER_DEFINE(noise_timer, noise_tick, NULL);

int radio_start(const struct radio_config *c, radio_rx_cb cb)
{
	struct esb_config config = ESB_DEFAULT_CONFIG;
	int err;

	rx_cb = cb;
	config.protocol = ESB_PROTOCOL_ESB_DPL;
	config.mode = ESB_MODE_PRX;
	config.event_handler = on_esb_event;
	config.bitrate = c->rate == RADIO_2MBPS ? ESB_BITRATE_2MBPS : ESB_BITRATE_1MBPS;
	config.crc = c->crc == RADIO_CRC_16 ? ESB_CRC_16BIT : ESB_CRC_8BIT;
	config.tx_output_power = 8;
	config.payload_length = RADIO_MAX_PAYLOAD;
	config.selective_auto_ack = false;

	err = esb_init(&config);
	if (!err) {
		err = esb_set_base_address_0(c->base0);
	}
	if (!err) {
		err = esb_set_base_address_1(c->base1);
	}
	if (!err) {
		err = esb_set_prefixes(c->prefixes, c->pipes);
	}
	if (!err) {
		err = radio_set_channel(c->channel);
	}
	if (!err && c->noise) {
		k_timer_start(&noise_timer, K_USEC(NOISE_TICK_US), K_USEC(NOISE_TICK_US));
	}
	return err;
}

int radio_set_channel(uint8_t channel)
{
	int err;

	stop_rx();
	esb_flush_tx();
	esb_flush_rx();
	err = esb_set_rf_channel(channel);
	if (err) {
		return err;
	}
	current_channel = channel;
	err = esb_start_rx();
	if (!err) {
		listening = true;
	}
	return err;
}

uint8_t radio_noise_take(struct noise_tally *t)
{
	unsigned int key = irq_lock();
	uint8_t channel = current_channel;

	*t = noise;
	noise = (struct noise_tally){0};
	irq_unlock(key);
	return channel;
}

uint32_t radio_replies_taken(void)
{
	return (uint32_t)atomic_get(&replies_taken);
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
	rssi_start();
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND)) {
	}
	dbm = rssi_dbm();
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
	}
	return dbm;
}

/* sample_dbm drives the radio by hand. */
static int receiver_off(void)
{
	int err = stop_rx();

	if (!err) {
		irq_disable(RADIO_IRQn);
	}
	return err;
}

static int receiver_on(void)
{
	NVIC_ClearPendingIRQ(RADIO_IRQn);
	irq_enable(RADIO_IRQn);
	return radio_set_channel(current_channel);
}

int radio_sample(const uint8_t *channels, size_t n, int8_t *dbm)
{
	int err = receiver_off();

	if (err) {
		return err;
	}
	for (size_t i = 0; i < n; i++) {
		dbm[i] = sample_dbm(channels[i]);
	}
	return receiver_on();
}

int radio_survey(struct chan_survey *s)
{
	int err = receiver_off();

	if (err) {
		return err;
	}
	for (int round = 0; round < CHAN_SAMPLES; round++) {
		for (int i = 0; i < CHAN_COUNT; i++) {
			s->dbm[i][round] = sample_dbm(CHAN_FIRST + i);
		}
		k_msleep(SURVEY_ROUND_GAP_MS);
	}
	return receiver_on();
}
