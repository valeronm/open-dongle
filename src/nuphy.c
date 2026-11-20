#include <string.h>
#include "nuphy.h"

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
#define TYPE_NAME 0x56
#define TYPE_KEEPALIVE 0x57
#define TYPE_MOUSE 0x60
#define TYPE_KEYBOARD 0x61
#define TYPE_NKRO 0x62
#define TYPE_CONSUMER 0x63
#define TYPE_SYSTEM 0x64
#define TYPE_PAIR_REQUEST 0x80
#define TYPE_PAIR_CONFIRM 0x81

#define DESCRIPTOR_STRING 3

static uint8_t sum8(const uint8_t *p, size_t n)
{
	uint8_t s = 0;

	while (n--) {
		s += *p++;
	}
	return s;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

struct nuphy_msg nuphy_parse(const uint8_t *buf, size_t len)
{
	struct nuphy_msg m = {.kind = NUPHY_INVALID};

	if (len == FRAME_SHORT) {
		if (buf[1] == TYPE_KEEPALIVE && buf[2] == MARKER && sum8(buf, 3) == buf[3]) {
			m.kind = NUPHY_KEEPALIVE;
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
		m.kind = NUPHY_KEYBOARD;
		memcpy(m.keyboard, &buf[2], NUPHY_KEYBOARD_LEN);
		break;
	case TYPE_NKRO:
		m.kind = NUPHY_NKRO;
		m.nkro.modifiers = buf[2];
		memcpy(m.nkro.keys, &buf[3], NUPHY_NKRO_LEN);
		break;
	case TYPE_MOUSE:
		m.kind = NUPHY_MOUSE;
		m.mouse = (struct report_mouse){
			.buttons = buf[2],
			.x = (int8_t)buf[3],
			.y = (int8_t)buf[4],
			.wheel = (int8_t)buf[5],
			.pan = (int8_t)buf[6],
		};
		break;
	case TYPE_CONSUMER:
	case TYPE_SYSTEM:
		m.kind = buf[1] == TYPE_CONSUMER ? NUPHY_CONSUMER : NUPHY_SYSTEM;
		m.usage = le16(&buf[2]);
		break;
	case TYPE_PAIR_REQUEST:
	case TYPE_PAIR_CONFIRM:
		m.kind = buf[1] == TYPE_PAIR_REQUEST ? NUPHY_PAIR_REQUEST : NUPHY_PAIR_CONFIRM;
		memcpy(m.ids.keyboard, &buf[2], NUPHY_ID_LEN);
		memcpy(m.ids.dongle, &buf[6], NUPHY_ID_LEN);
		break;
	case TYPE_HELLO:
		m.kind = NUPHY_HELLO;
		break;
	case TYPE_NAME:
		m.kind = NUPHY_NAME;
		memcpy(m.name_chunk, &buf[2], NUPHY_NAME_CHUNK);
		break;
	default:
		m.kind = NUPHY_IGNORED;
		break;
	}
	return m;
}

_Static_assert(NUPHY_KEYBOARD_LEN == REPORT_KEYBOARD_LEN,
	       "a NuPhy keyboard frame is a boot report");

static void held_keys(const struct nuphy_keys *k, struct report *out)
{
	*out = reports_held_keys(k->boot[0], &k->boot[2]);
	out->keys[REPORT_MODIFIER_FIRST / 8] |= k->nkro.modifiers;
	for (size_t i = 0; i < NUPHY_NKRO_LEN; i++) {
		out->keys[i] |= k->nkro.keys[i];
	}
	out->keys[0] &= (uint8_t)~((1u << REPORT_KEY_FIRST) - 1);
}

bool nuphy_report(struct nuphy_keys *k, const struct nuphy_msg *m, struct report *out)
{
	switch (m->kind) {
	case NUPHY_KEYBOARD:
		memcpy(k->boot, m->keyboard, sizeof(k->boot));
		held_keys(k, out);
		return true;
	case NUPHY_NKRO:
		k->nkro = m->nkro;
		held_keys(k, out);
		return true;
	case NUPHY_MOUSE:
		*out = (struct report){.kind = REPORT_MOUSE, .mouse = m->mouse};
		return true;
	case NUPHY_CONSUMER:
		*out = (struct report){.kind = REPORT_CONSUMER, .usage = m->usage};
		return true;
	case NUPHY_SYSTEM:
		*out = (struct report){.kind = REPORT_SYSTEM, .usage = m->usage};
		return true;
	default:
		return false;
	}
}

static bool starts_descriptor(const uint8_t *chunk)
{
	return chunk[1] == DESCRIPTOR_STRING && chunk[0] >= NUPHY_NAME_HEADER &&
	       chunk[0] <= NUPHY_NAME_BLOCK_MAX && chunk[0] % 2 == 0;
}

/* The keyboard sends the name its dongle shows on USB. */
static void trim_dongle(uint8_t *name, size_t len)
{
	static const char suffix[] = " Dongle";
	size_t n = sizeof(suffix) - 1;

	if (len >= n && memcmp(&name[len - n], suffix, n) == 0) {
		memset(&name[len - n], 0, n);
	}
}

bool nuphy_name_add(struct nuphy_name *n, const uint8_t chunk[NUPHY_NAME_CHUNK])
{
	uint8_t name[NUPHY_NAME_MAX] = {0};
	uint8_t *out = name;

	if (starts_descriptor(chunk)) {
		n->held = 0;
	} else if (n->held == 0) {
		return false;
	}
	memcpy(&n->block[n->held], chunk, NUPHY_NAME_CHUNK);
	n->held += NUPHY_NAME_CHUNK;
	if (n->held < n->block[0]) {
		return false;
	}

	for (size_t i = NUPHY_NAME_HEADER; i < n->block[0]; i += 2) {
		uint16_t unit = le16(&n->block[i]);

		*out++ = unit >= 0x20 && unit <= 0x7e ? (uint8_t)unit : '?';
	}
	trim_dongle(name, (size_t)(out - name));
	n->held = 0;
	if (memcmp(name, n->name, sizeof(name)) == 0) {
		return false;
	}
	memcpy(n->name, name, sizeof(name));
	return true;
}

/* The keyboard drops an ACK payload whose sequence byte is 0. */
uint8_t nuphy_next_seq(uint8_t seq)
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

size_t nuphy_pair_reply(uint8_t *out, uint8_t seq, const uint8_t dongle_id[NUPHY_ID_LEN],
			const uint8_t keyboard_id[NUPHY_ID_LEN], const uint8_t channels[3])
{
	uint8_t data[2 * NUPHY_ID_LEN + 3];

	_Static_assert(sizeof(data) + FRAME_OVERHEAD <= NUPHY_FRAME_MAX,
		       "the pair reply must fit NUPHY_FRAME_MAX");
	memcpy(data, dongle_id, NUPHY_ID_LEN);
	memcpy(data + NUPHY_ID_LEN, keyboard_id, NUPHY_ID_LEN);
	memcpy(data + 2 * NUPHY_ID_LEN, channels, 3);
	return frame(out, seq, TYPE_PAIR_REPLY, data, sizeof(data));
}

size_t nuphy_status(uint8_t *out, uint8_t seq, const uint8_t dongle_id[NUPHY_ID_LEN],
		    uint8_t host_leds)
{
	uint8_t data[8] = {0};

	memcpy(data, dongle_id, NUPHY_ID_LEN);
	/* The keyboard ignores the LED byte when any bit above Scroll Lock is set. */
	data[NUPHY_ID_LEN] = host_leds & 0x07;
	return frame(out, seq, TYPE_STATUS, data, sizeof(data));
}

size_t nuphy_suspend(uint8_t *out, uint8_t seq)
{
	static const uint8_t data[4];

	return frame(out, seq, TYPE_SUSPEND, data, sizeof(data));
}
