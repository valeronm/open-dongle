#ifndef NUPHY_H
#define NUPHY_H

#include <stddef.h>
#include <stdint.h>

#define NUPHY_ID_LEN 4
#define NUPHY_KEYBOARD_LEN 8
#define NUPHY_FRAME_MAX 16

enum nuphy_kind {
	NUPHY_INVALID,
	NUPHY_KEEPALIVE,
	NUPHY_KEYBOARD,
	NUPHY_CONSUMER,
	NUPHY_SYSTEM,
	NUPHY_PAIR_REQUEST,
	NUPHY_PAIR_CONFIRM,
	NUPHY_HELLO,
	NUPHY_IGNORED,
};

struct nuphy_ids {
	uint8_t keyboard[NUPHY_ID_LEN];
	uint8_t dongle[NUPHY_ID_LEN];
};

struct nuphy_msg {
	enum nuphy_kind kind;
	uint8_t seq;
	union {
		uint8_t keyboard[NUPHY_KEYBOARD_LEN];
		uint16_t usage;
		struct nuphy_ids ids;
	};
};

/*
 * Parses one ESB payload from the keyboard. The kind is NUPHY_INVALID when the length, marker or
 * checksum is wrong, and NUPHY_IGNORED for a valid frame this firmware does not act on; only the
 * union member matching the kind is set.
 */
struct nuphy_msg nuphy_parse(const uint8_t *buf, size_t len);

uint8_t nuphy_next_seq(uint8_t seq);

/* Each builder writes one ACK-payload frame to out (NUPHY_FRAME_MAX bytes) and returns its length. */
size_t nuphy_pair_reply(uint8_t *out, uint8_t seq, const uint8_t dongle_id[NUPHY_ID_LEN],
			const uint8_t keyboard_id[NUPHY_ID_LEN], const uint8_t channels[3]);
size_t nuphy_status(uint8_t *out, uint8_t seq, const uint8_t dongle_id[NUPHY_ID_LEN],
		    uint8_t host_leds);
size_t nuphy_suspend(uint8_t *out, uint8_t seq);

#endif
