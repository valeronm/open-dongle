#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_ID_LEN 4
#define PROTO_KEYBOARD_LEN 8
#define PROTO_FRAME_MAX 16

enum proto_kind {
	PROTO_INVALID,
	PROTO_KEEPALIVE,
	PROTO_KEYBOARD,
	PROTO_CONSUMER,
	PROTO_SYSTEM,
	PROTO_PAIR_REQUEST,
	PROTO_PAIR_CONFIRM,
	PROTO_HELLO,
	PROTO_IGNORED,
};

struct proto_ids {
	uint8_t keyboard[PROTO_ID_LEN];
	uint8_t dongle[PROTO_ID_LEN];
};

struct proto_msg {
	enum proto_kind kind;
	uint8_t seq;
	union {
		uint8_t keyboard[PROTO_KEYBOARD_LEN];
		uint16_t usage;
		struct proto_ids ids;
	};
};

/*
 * Parses one ESB payload from the keyboard. The kind is PROTO_INVALID when the length, marker or
 * checksum is wrong, and PROTO_IGNORED for a valid frame this firmware does not act on; only the
 * union member matching the kind is set.
 */
struct proto_msg proto_parse(const uint8_t *buf, size_t len);

uint8_t proto_next_seq(uint8_t seq);

/* Each builder writes one ACK-payload frame to out (PROTO_FRAME_MAX bytes) and returns its length. */
size_t proto_pair_reply(uint8_t *out, uint8_t seq, const uint8_t dongle_id[PROTO_ID_LEN],
			const uint8_t keyboard_id[PROTO_ID_LEN], const uint8_t channels[3]);
size_t proto_status(uint8_t *out, uint8_t seq, const uint8_t dongle_id[PROTO_ID_LEN],
		    uint8_t host_leds);
size_t proto_suspend(uint8_t *out, uint8_t seq);

#endif
