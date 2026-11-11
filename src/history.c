#include <string.h>
#include "history.h"

static uint16_t sat16(uint32_t v)
{
	return v > UINT16_MAX ? UINT16_MAX : v;
}

static uint8_t sat8(uint32_t v)
{
	return v > UINT8_MAX ? UINT8_MAX : v;
}

static void reset_minute(struct history *h)
{
	h->rssi_sum = 0;
	h->rssi_count = 0;
	h->rssi_min = 0;
	h->rssi_max = 0;
	h->noise = (struct noise_tally){0};
}

void history_clear(struct history *h, uint64_t now_ms, const struct history_totals *t)
{
	h->minute_next = now_ms / HISTORY_MINUTE_MS;
	h->minute_oldest = h->minute_next;
	h->at_minute_start = *t;
	h->events_oldest = h->events_total;
	reset_minute(h);
}

void history_init(struct history *h, uint64_t now_ms, const struct history_totals *t)
{
	memset(h, 0, sizeof(*h));
	history_clear(h, now_ms, t);
}

static struct history_minute close_minute(const struct history *h, const struct history_totals *t)
{
	const struct history_totals *s = &h->at_minute_start;
	struct history_minute m = {
		.packets = sat16(t->packets - s->packets),
		.invalid = sat8(t->invalid - s->invalid),
		.dropped = sat8(t->dropped - s->dropped),
		.channel_changes = sat8(t->channel_changes - s->channel_changes),
		.link_losses = sat8(t->link_losses - s->link_losses),
		.usb_retries = sat8(t->usb_retries - s->usb_retries),
		.gaps = sat8(t->gaps - s->gaps),
	};

	if (h->rssi_count > 0) {
		m.rssi_min = h->rssi_min;
		m.rssi_max = h->rssi_max;
		m.rssi_avg = h->rssi_sum / (int32_t)h->rssi_count;
	}
	m.noise = noise_record(&h->noise);
	return m;
}

void history_tick(struct history *h, uint64_t now_ms, const struct history_totals *t)
{
	uint32_t minute = now_ms / HISTORY_MINUTE_MS;

	while (h->minute_next < minute) {
		h->minutes[h->minute_next % HISTORY_MINUTES] = close_minute(h, t);
		h->minute_next++;
		if (h->minute_next - h->minute_oldest > HISTORY_MINUTES) {
			h->minute_oldest = h->minute_next - HISTORY_MINUTES;
		}
		h->at_minute_start = *t;
		reset_minute(h);
	}
}

void history_rssi(struct history *h, int8_t dbm)
{
	if (h->rssi_count == 0 || dbm < h->rssi_min) {
		h->rssi_min = dbm;
	}
	if (h->rssi_count == 0 || dbm > h->rssi_max) {
		h->rssi_max = dbm;
	}
	h->rssi_sum += dbm;
	h->rssi_count++;
}

void history_noise(struct history *h, const struct noise_tally *t)
{
	noise_merge(&h->noise, t);
}

void history_event(struct history *h, const struct history_event *e)
{
	h->events[h->events_total % HISTORY_EVENTS] = *e;
	h->events_total++;
	if (h->events_total - h->events_oldest > HISTORY_EVENTS) {
		h->events_oldest = h->events_total - HISTORY_EVENTS;
	}
}

size_t history_minutes(const struct history *h, uint32_t from, uint32_t *first,
		       struct history_minute *out, size_t max)
{
	uint32_t start = from < h->minute_oldest ? h->minute_oldest : from;
	size_t n = 0;

	*first = start;
	while (n < max && start + n < h->minute_next) {
		out[n] = h->minutes[(start + n) % HISTORY_MINUTES];
		n++;
	}
	return n;
}

size_t history_events(const struct history *h, uint32_t from, uint32_t *first,
		      struct history_event *out, size_t max)
{
	uint32_t start = from < h->events_oldest ? h->events_oldest : from;
	size_t n = 0;

	*first = start;
	while (n < max && start + n < h->events_total) {
		out[n] = h->events[(start + n) % HISTORY_EVENTS];
		n++;
	}
	return n;
}
