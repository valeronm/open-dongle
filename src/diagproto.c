#include <string.h>
#include "diagproto.h"

static uint8_t *put_le(uint8_t *p, uint64_t v, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++) {
		*p++ = v >> (8 * i);
	}
	return p;
}

bool diag_command_answered(uint8_t command, enum protocol active)
{
	switch (command) {
	case DIAG_FORCE_CHANNELS:
		return active == PROTOCOL_NUPHY;
	case DIAG_NOISE:
	case DIAG_MOVE:
	case DIAG_WATCH:
		return protocol_has_esb(active);
	case DIAG_FORGET:
		return active == PROTOCOL_BLUETOOTH;
	default:
		return true;
	}
}

bool diag_parse_request(const uint8_t *buf, size_t len, struct diag_request *out)
{
	if (len < DIAG_REQUEST_LEN) {
		return false;
	}
	*out = (struct diag_request){0};
	out->command = buf[0];
	out->seq = buf[1];
	out->arg = buf[2] | buf[3] << 8 | buf[4] << 16 | (uint32_t)buf[5] << 24;
	len -= DIAG_REQUEST_LEN;
	memcpy(out->data, &buf[DIAG_REQUEST_LEN], len < sizeof(out->data) ? len : sizeof(out->data));
	return true;
}

bool diag_channels_arg(uint32_t arg, uint8_t out[3])
{
	for (int i = 0; i < 3; i++) {
		out[i] = arg >> (8 * i);
	}
	return arg != 0;
}

size_t diag_part_count(size_t payload_len)
{
	return payload_len == 0 ? 1 : (payload_len + DIAG_CHUNK - 1) / DIAG_CHUNK;
}

void diag_part(uint8_t out[DIAG_REPORT_LEN], const struct diag_request *req,
	       const uint8_t *payload, size_t len, size_t part)
{
	size_t offset = part * DIAG_CHUNK;
	size_t n = len > offset ? len - offset : 0;

	memset(out, 0, DIAG_REPORT_LEN);
	out[0] = req->command;
	out[1] = req->seq;
	out[2] = part;
	out[3] = diag_part_count(len);
	if (n > 0) {
		memcpy(out + DIAG_HEADER_LEN, payload + offset, n < DIAG_CHUNK ? n : DIAG_CHUNK);
	}
}

void diag_error_report(uint8_t out[DIAG_REPORT_LEN], const struct diag_request *req,
		       enum diag_error code, uint8_t detail)
{
	memset(out, 0, DIAG_REPORT_LEN);
	out[0] = req->command | DIAG_ERROR_FLAG;
	out[1] = req->seq;
	out[3] = 1;
	out[DIAG_HEADER_LEN] = code;
	out[DIAG_HEADER_LEN + 1] = detail;
}

size_t diag_encode_status(uint8_t *out, const struct diag_status *s)
{
	uint8_t *p = out;

	*p++ = DIAG_VERSION;
	memcpy(p, s->firmware_version, sizeof(s->firmware_version));
	p += sizeof(s->firmware_version);
	p = put_le(p, s->uptime_ms, 8);
	p = put_le(p, s->reset_cause, 4);
	*p++ = s->link_state;
	*p++ = s->channel;
	memcpy(p, s->own, sizeof(s->own));
	p += sizeof(s->own);
	memcpy(p, s->forced, sizeof(s->forced));
	p += sizeof(s->forced);
	memcpy(p, s->keyboard_id, sizeof(s->keyboard_id));
	p += sizeof(s->keyboard_id);
	*p++ = (s->usb_live ? 1 : 0) | (s->usb_suspended ? 2 : 0);
	p = put_le(p, s->invalid, 4);
	p = put_le(p, s->dropped, 4);
	p = put_le(p, s->channel_changes, 4);
	p = put_le(p, s->usb_retries, 4);
	p = put_le(p, s->merges, 4);
	p = put_le(p, s->surveys, 4);
	p = put_le(p, s->minute_next, 4);
	p = put_le(p, s->minute_oldest, 4);
	p = put_le(p, s->events_total, 4);
	p = put_le(p, s->events_oldest, 4);
	*p++ = s->protocol;
	*p++ = s->pairing_open ? 1 : 0;
	memcpy(p, s->keyboard_serial, sizeof(s->keyboard_serial));
	p += sizeof(s->keyboard_serial);
	*p++ = s->keyboard_type;
	*p++ = s->slot_count;
	for (uint8_t i = 0; i < s->slot_count && i < DEVICE_SLOTS; i++) {
		*p++ = s->slots[i].kind;
		*p++ = (s->slots[i].stored ? DIAG_SLOT_STORED : 0) |
		       (s->slots[i].ready ? DIAG_SLOT_READY : 0);
	}
	*p++ = s->slot;
	*p++ = (uint8_t)s->last_rssi;
	p = put_le(p, s->last_rx_age_ms, 4);
	p = put_le(p, s->packets, 4);
	p = put_le(p, s->gaps, 4);
	p = put_le(p, s->link_losses, 4);
	memcpy(p, s->name, sizeof(s->name));
	p += sizeof(s->name);
	return p - out;
}

static uint8_t *put_noise(uint8_t *p, const struct noise_record *n)
{
	*p++ = (uint8_t)n->peak;
	p = put_le(p, n->busy, 2);
	return put_le(p, n->strong, 2);
}

size_t diag_encode_minutes(uint8_t *out, uint32_t first, const struct history_minute *m, size_t n,
			   uint8_t slot)
{
	uint8_t *p = put_le(out, first, 4);

	*p++ = n;
	for (size_t i = 0; i < n; i++) {
		const struct history_rssi *rssi = &m[i].rssi[slot];

		*p++ = m[i].invalid;
		*p++ = m[i].dropped;
		*p++ = m[i].channel_changes;
		*p++ = m[i].usb_retries;
		p = put_noise(p, &m[i].noise);
		p = put_le(p, m[i].packets[slot], 2);
		*p++ = m[i].gaps[slot];
		*p++ = m[i].link_losses[slot];
		*p++ = (uint8_t)rssi->min;
		*p++ = (uint8_t)rssi->avg;
		*p++ = (uint8_t)rssi->max;
	}
	return p - out;
}

size_t diag_encode_events(uint8_t *out, uint32_t first, const struct history_event *e, size_t n)
{
	uint8_t *p = put_le(out, first, 4);

	*p++ = n;
	for (size_t i = 0; i < n; i++) {
		p = put_le(p, e[i].time_ms, 8);
		*p++ = e[i].type;
		*p++ = e[i].slot;
		*p++ = e[i].a;
		*p++ = e[i].b;
		*p++ = e[i].c;
		p = put_le(p, e[i].value, 4);
		p = put_noise(p, &e[i].noise);
	}
	return p - out;
}

size_t diag_encode_noise(uint8_t *out, const struct chan_survey *s)
{
	uint8_t *p = out;

	*p++ = CHAN_FIRST;
	*p++ = CHAN_COUNT;
	for (int i = 0; i < CHAN_COUNT; i++) {
		int8_t loudest = s->dbm[i][0];
		int32_t sum = 0;

		for (int k = 0; k < CHAN_SAMPLES; k++) {
			if (s->dbm[i][k] > loudest) {
				loudest = s->dbm[i][k];
			}
			sum += s->dbm[i][k];
		}
		*p++ = (uint8_t)loudest;
		*p++ = (uint8_t)(int8_t)(sum / CHAN_SAMPLES);
	}
	return p - out;
}

size_t diag_encode_watch(uint8_t *out, const struct chanwatch *w, uint8_t home,
			 uint8_t threshold_bucket, bool paused, uint64_t now_ms)
{
	uint8_t *p = out;

	*p++ = home;
	*p++ = threshold_bucket;
	*p++ = paused;
	p = put_le(p, w->hold_until_ms > now_ms ? w->hold_until_ms - now_ms : 0, 4);
	*p++ = w->count;
	for (size_t i = 0; i < w->count; i++) {
		uint16_t counted = threshold_bucket == CHANWATCH_NO_THRESHOLD
					   ? 0
					   : chanwatch_share_hundredths(
						     chanwatch_share(w, i, threshold_bucket));

		*p++ = w->channels[i];
		*p++ = chanwatch_enough(w, i);
		p = put_le(p, counted, 2);
		for (int b = 0; b < NOISE_BUCKETS; b++) {
			p = put_le(p, (uint32_t)(w->weight[i][b] + 0.5f), 4);
		}
	}
	return p - out;
}

_Static_assert(BTLIST_NAME_LEN == DIAG_NAME_MAX, "a stored device's name is sent whole");

size_t diag_encode_devices(uint8_t *out, const struct btlist *l,
			   const uint8_t *const connected[DEVICE_SLOTS])
{
	uint8_t *p = out;

	*p++ = l->count;
	for (uint8_t i = 0; i < l->count; i++) {
		const struct btlist_entry *e = &l->entries[i];
		uint8_t slot = DEVICE_NO_SLOT;

		for (uint8_t s = 0; s < DEVICE_SLOTS; s++) {
			if (connected[s] && memcmp(e->addr, connected[s], sizeof(e->addr)) == 0) {
				slot = s;
			}
		}
		memcpy(p, e->addr, sizeof(e->addr));
		p += sizeof(e->addr);
		*p++ = e->kind;
		*p++ = e->no_passkey ? DIAG_DEVICE_NO_PASSKEY : 0;
		*p++ = slot;
		memcpy(p, e->name, sizeof(e->name));
		p += sizeof(e->name);
	}
	return (size_t)(p - out);
}
