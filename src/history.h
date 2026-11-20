#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device.h"
#include "noise.h"

#define HISTORY_MINUTES 1440
#define HISTORY_EVENTS 512
#define HISTORY_MINUTE_MS 60000

/* Totals must not decrease except across history_clear; minutes store their differences. */
struct history_totals {
	uint32_t packets[DEVICE_SLOTS];
	uint32_t gaps[DEVICE_SLOTS];
	uint32_t link_losses[DEVICE_SLOTS];
	uint32_t invalid;
	uint32_t dropped;
	uint32_t usb_retries;
};

struct history_rssi_sum {
	int32_t sum;
	uint32_t count;
	int8_t min;
	int8_t max;
};

#define HISTORY_ADDR_LEN DIAG_ADDR_LEN

/* Minute n covers uptime [n * HISTORY_MINUTE_MS, (n + 1) * HISTORY_MINUTE_MS). */
struct history {
	struct history_minute minutes[HISTORY_MINUTES];
	uint32_t minute_next;
	uint32_t minute_oldest;
	struct history_totals at_minute_start;
	struct history_rssi_sum rssi[DEVICE_SLOTS];
	struct history_event events[HISTORY_EVENTS];
	uint32_t events_total;
	uint32_t events_oldest;
	struct noise_tally noise;
	struct noise_tally since_packet[DEVICE_SLOTS];
};

void history_init(struct history *h, uint64_t now_ms, const struct history_totals *t);

/* Drops the held minutes and events; their indexes are not reset. */
void history_clear(struct history *h, uint64_t now_ms, const struct history_totals *t);

/* Closes every minute that ended before now_ms. */
void history_tick(struct history *h, uint64_t now_ms, const struct history_totals *t);

/* A gap's event is logged before the packet that ended the gap comes here. */
void history_rssi(struct history *h, uint8_t slot, int8_t dbm);

/* The slot's weakest reading over the current and the previous minute; false when neither had
 * one. */
bool history_weakest(const struct history *h, uint8_t slot, int8_t *out);

/* A gap or loss event records the readings since its slot's last history_rssi in place of e's
 * noise. */
void history_event(struct history *h, const struct history_event *e);

/* Readings that do not count toward the minute still count toward a gap or loss. */
void history_noise(struct history *h, const struct noise_tally *t, bool counts);

/* Copies up to max held records from index from, or from the oldest held if that is later. */
size_t history_minutes(const struct history *h, uint32_t from, uint32_t *first,
		       struct history_minute *out, size_t max);
size_t history_events(const struct history *h, uint32_t from, uint32_t *first,
		      struct history_event *out, size_t max);

#endif
