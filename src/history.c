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
	memset(h->rssi, 0, sizeof(h->rssi));
	h->noise = (struct noise_tally){0};
}

void history_clear(struct history *h, uint64_t now_ms, const struct history_totals *t)
{
	h->minute_next = now_ms / HISTORY_MINUTE_MS;
	h->minute_oldest = h->minute_next;
	h->at_minute_start = *t;
	h->events_oldest = h->events_total;
	memset(h->since_packet, 0, sizeof(h->since_packet));
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
		.invalid = sat8(t->invalid - s->invalid),
		.dropped = sat8(t->dropped - s->dropped),
		.usb_retries = sat8(t->usb_retries - s->usb_retries),
	};

	for (int d = 0; d < DEVICE_SLOTS; d++) {
		const struct history_rssi_sum *r = &h->rssi[d];

		m.packets[d] = sat16(t->packets[d] - s->packets[d]);
		m.gaps[d] = sat8(t->gaps[d] - s->gaps[d]);
		m.link_losses[d] = sat8(t->link_losses[d] - s->link_losses[d]);
		if (r->count > 0) {
			m.rssi[d] = (struct history_rssi){
				.min = r->min,
				.avg = r->sum / (int32_t)r->count,
				.max = r->max,
			};
		}
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

void history_rssi(struct history *h, uint8_t slot, int8_t dbm)
{
	struct history_rssi_sum *r = &h->rssi[slot];

	if (r->count == 0 || dbm < r->min) {
		r->min = dbm;
	}
	if (r->count == 0 || dbm > r->max) {
		r->max = dbm;
	}
	r->sum += dbm;
	r->count++;
	h->since_packet[slot] = (struct noise_tally){0};
}

void history_noise(struct history *h, const struct noise_tally *t, bool home)
{
	for (int d = 0; d < DEVICE_SLOTS; d++) {
		noise_merge(&h->since_packet[d], t);
	}
	if (home) {
		noise_merge(&h->noise, t);
	}
}

void history_event(struct history *h, const struct history_event *e)
{
	struct history_event *stored = &h->events[h->events_total % HISTORY_EVENTS];

	*stored = *e;
	if ((e->type == HISTORY_LINK_GAP || e->type == HISTORY_LINK_LOST) &&
	    e->slot < DEVICE_SLOTS) {
		stored->noise = noise_record(&h->since_packet[e->slot]);
		h->since_packet[e->slot] = (struct noise_tally){0};
	}
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

bool history_weakest(const struct history *h, uint8_t slot, int8_t *out)
{
	bool found = h->rssi[slot].count > 0;

	if (found) {
		*out = h->rssi[slot].min;
	}
	if (h->minute_next > h->minute_oldest) {
		const struct history_minute *m =
			&h->minutes[(h->minute_next - 1) % HISTORY_MINUTES];

		int8_t min = m->rssi[slot].min;

		if (min != 0 && (!found || min < *out)) {
			*out = min;
			found = true;
		}
	}
	return found;
}

struct history_event history_keyboard_event(uint8_t type, uint8_t slot,
					    const uint8_t addr[HISTORY_ADDR_LEN], uint8_t c)
{
	return (struct history_event){
		.type = type,
		.slot = slot,
		.a = addr[6],
		.b = addr[5],
		.c = c,
		.value = (uint32_t)addr[1] | (uint32_t)addr[2] << 8 | (uint32_t)addr[3] << 16 |
			 (uint32_t)addr[4] << 24,
	};
}

void history_since_input(int64_t elapsed_ms, uint8_t *low, uint8_t *high)
{
	int64_t whole = elapsed_ms / 1000;
	uint16_t seconds = elapsed_ms < 0 || whole > UINT16_MAX ? UINT16_MAX : (uint16_t)whole;

	*low = (uint8_t)seconds;
	*high = (uint8_t)(seconds >> 8);
}
