#include <string.h>
#include "diagproto.h"

static uint8_t *put_le(uint8_t *p, uint64_t v, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++) {
		*p++ = v >> (8 * i);
	}
	return p;
}

bool diag_parse_request(const uint8_t *buf, size_t len, struct diag_request *out)
{
	if (len < DIAG_REQUEST_LEN) {
		return false;
	}
	out->command = buf[0];
	out->seq = buf[1];
	out->arg = buf[2] | buf[3] << 8 | buf[4] << 16 | (uint32_t)buf[5] << 24;
	return true;
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
		       enum diag_error code)
{
	memset(out, 0, DIAG_REPORT_LEN);
	out[0] = req->command | DIAG_ERROR_FLAG;
	out[1] = req->seq;
	out[3] = 1;
	out[DIAG_HEADER_LEN] = code;
}

size_t diag_encode_status(uint8_t *out, const struct diag_status *s)
{
	uint8_t *p = out;

	*p++ = DIAG_VERSION;
	p = put_le(p, s->uptime_ms, 8);
	p = put_le(p, s->reset_cause, 4);
	*p++ = s->link_state;
	*p++ = s->channel;
	memcpy(p, s->own, sizeof(s->own));
	p += sizeof(s->own);
	memcpy(p, s->keyboard_id, sizeof(s->keyboard_id));
	p += sizeof(s->keyboard_id);
	*p++ = (uint8_t)s->last_rssi;
	p = put_le(p, s->last_rx_age_ms, 4);
	*p++ = (s->usb_live ? 1 : 0) | (s->usb_suspended ? 2 : 0);
	p = put_le(p, s->packets, 4);
	p = put_le(p, s->invalid, 4);
	p = put_le(p, s->dropped, 4);
	p = put_le(p, s->channel_changes, 4);
	p = put_le(p, s->link_losses, 4);
	p = put_le(p, s->usb_retries, 4);
	p = put_le(p, s->merges, 4);
	p = put_le(p, s->surveys, 4);
	p = put_le(p, s->minute_next, 4);
	p = put_le(p, s->minute_oldest, 4);
	p = put_le(p, s->events_total, 4);
	p = put_le(p, s->events_oldest, 4);
	return p - out;
}

size_t diag_encode_minutes(uint8_t *out, uint32_t first, const struct history_minute *m, size_t n)
{
	uint8_t *p = put_le(out, first, 4);

	*p++ = n;
	for (size_t i = 0; i < n; i++) {
		p = put_le(p, m[i].packets, 2);
		*p++ = m[i].invalid;
		*p++ = m[i].dropped;
		*p++ = m[i].channel_changes;
		*p++ = m[i].link_losses;
		*p++ = m[i].usb_retries;
		*p++ = (uint8_t)m[i].rssi_min;
		*p++ = (uint8_t)m[i].rssi_avg;
		*p++ = (uint8_t)m[i].rssi_max;
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
		*p++ = e[i].a;
		*p++ = e[i].b;
		*p++ = e[i].c;
		p = put_le(p, e[i].value, 4);
	}
	return p - out;
}
