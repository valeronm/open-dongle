#include <string.h>
#include "protocol.h"

#define FRAME_LONG 24
#define FRAME_SHORT 8
#define FRAME_OVERHEAD 4
#define LONG_MARKER_AT (FRAME_LONG - 2)
#define LONG_SUM_AT (FRAME_LONG - 1)
#define MARKER 0x5a

#define TYPE_STATUS 0x30
#define TYPE_PAIR_REPLY 0x32
#define TYPE_SUSPEND 0x34
#define TYPE_HELLO 0x35
#define TYPE_KEEPALIVE 0x57
#define TYPE_KEYBOARD 0x61
#define TYPE_CONSUMER 0x63
#define TYPE_SYSTEM 0x64
#define TYPE_PAIR_REQUEST 0x80
#define TYPE_PAIR_CONFIRM 0x81

static uint8_t sum8(const uint8_t *p, size_t n)
{
	uint8_t s = 0;

	while (n--) {
		s += *p++;
	}
	return s;
}

struct proto_msg proto_parse(const uint8_t *buf, size_t len)
{
	struct proto_msg m = {.kind = PROTO_INVALID};

	if (len == FRAME_SHORT) {
		if (buf[1] == TYPE_KEEPALIVE && buf[2] == MARKER && sum8(buf, 3) == buf[3]) {
			m.kind = PROTO_KEEPALIVE;
			m.seq = buf[0];
		}
		return m;
	}
	if (len != FRAME_LONG || buf[LONG_MARKER_AT] != MARKER ||
	    sum8(buf, LONG_SUM_AT) != buf[LONG_SUM_AT]) {
		return m;
	}

	m.seq = buf[0];
	switch (buf[1]) {
	case TYPE_KEYBOARD:
		m.kind = PROTO_KEYBOARD;
		memcpy(m.keyboard, &buf[2], PROTO_KEYBOARD_LEN);
		break;
	case TYPE_CONSUMER:
	case TYPE_SYSTEM:
		m.kind = buf[1] == TYPE_CONSUMER ? PROTO_CONSUMER : PROTO_SYSTEM;
		m.usage = (uint16_t)(buf[2] | (buf[3] << 8));
		break;
	case TYPE_PAIR_REQUEST:
	case TYPE_PAIR_CONFIRM:
		m.kind = buf[1] == TYPE_PAIR_REQUEST ? PROTO_PAIR_REQUEST : PROTO_PAIR_CONFIRM;
		memcpy(m.ids.keyboard, &buf[2], PROTO_ID_LEN);
		memcpy(m.ids.dongle, &buf[6], PROTO_ID_LEN);
		break;
	case TYPE_HELLO:
		m.kind = PROTO_HELLO;
		break;
	default:
		m.kind = PROTO_IGNORED;
		break;
	}
	return m;
}

/* The keyboard drops an ACK payload whose sequence byte is 0. */
uint8_t proto_next_seq(uint8_t seq)
{
	seq++;
	return seq == 0 ? 1 : seq;
}

static size_t frame(uint8_t *out, uint8_t seq, uint8_t type, const uint8_t *data, size_t len)
{
	out[0] = seq;
	out[1] = type;
	memcpy(&out[2], data, len);
	out[len + 2] = MARKER;
	out[len + 3] = sum8(out, len + 3);
	return len + FRAME_OVERHEAD;
}

size_t proto_pair_reply(uint8_t *out, uint8_t seq, const uint8_t dongle_id[PROTO_ID_LEN],
			const uint8_t keyboard_id[PROTO_ID_LEN], const uint8_t channels[3])
{
	uint8_t data[2 * PROTO_ID_LEN + 3];

	_Static_assert(sizeof(data) + FRAME_OVERHEAD <= PROTO_FRAME_MAX,
		       "the pair reply must fit PROTO_FRAME_MAX");
	memcpy(data, dongle_id, PROTO_ID_LEN);
	memcpy(data + PROTO_ID_LEN, keyboard_id, PROTO_ID_LEN);
	memcpy(data + 2 * PROTO_ID_LEN, channels, 3);
	return frame(out, seq, TYPE_PAIR_REPLY, data, sizeof(data));
}

size_t proto_status(uint8_t *out, uint8_t seq, const uint8_t dongle_id[PROTO_ID_LEN],
		    uint8_t host_leds)
{
	uint8_t data[8] = {0};

	memcpy(data, dongle_id, PROTO_ID_LEN);
	/* The keyboard ignores the LED byte when any bit above Scroll Lock is set. */
	data[PROTO_ID_LEN] = host_leds & 0x07;
	return frame(out, seq, TYPE_STATUS, data, sizeof(data));
}

size_t proto_suspend(uint8_t *out, uint8_t seq)
{
	static const uint8_t data[4];

	return frame(out, seq, TYPE_SUSPEND, data, sizeof(data));
}
