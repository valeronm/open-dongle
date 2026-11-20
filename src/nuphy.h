#ifndef NUPHY_H
#define NUPHY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "reports.h"

#define NUPHY_ID_LEN 4
#define NUPHY_KEYBOARD_LEN 8
#define NUPHY_NKRO_LEN 14
#define NUPHY_FRAME_MAX 16
#define NUPHY_NAME_CHUNK 20
#define NUPHY_NAME_MAX 24
/* A USB string descriptor is a header, then UTF-16. */
#define NUPHY_NAME_HEADER 2
#define NUPHY_NAME_BLOCK_MAX (NUPHY_NAME_HEADER + 2 * NUPHY_NAME_MAX)

enum nuphy_kind {
	NUPHY_INVALID,
	NUPHY_KEEPALIVE,
	NUPHY_KEYBOARD,
	NUPHY_MOUSE,
	NUPHY_NKRO,
	NUPHY_CONSUMER,
	NUPHY_SYSTEM,
	NUPHY_PAIR_REQUEST,
	NUPHY_PAIR_CONFIRM,
	NUPHY_HELLO,
	NUPHY_NAME,
	NUPHY_IGNORED,
};

/* Keys past the six in the keyboard's 0x61 report, usages 0x00-0x6f. */
struct nuphy_nkro {
	uint8_t modifiers;
	uint8_t keys[NUPHY_NKRO_LEN];
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
		struct report_mouse mouse;
		struct nuphy_nkro nkro;
		struct nuphy_ids ids;
		uint8_t name_chunk[NUPHY_NAME_CHUNK];
	};
};

/*
 * Parses one ESB payload from the keyboard. The kind is NUPHY_INVALID when the length, marker or
 * checksum is wrong, and NUPHY_IGNORED for a valid frame this firmware does not act on; only the
 * union member matching the kind is set.
 */
struct nuphy_msg nuphy_parse(const uint8_t *buf, size_t len);

/* The keyboard splits its held keys between its 0x61 report and the 0x62 overflow. */
struct nuphy_keys {
	uint8_t boot[NUPHY_KEYBOARD_LEN];
	struct nuphy_nkro nkro;
};

/* Returns false for a message that carries no report. A keyboard or overflow frame updates k, which the caller zeroes when the host's keys are released. */
bool nuphy_report(struct nuphy_keys *k, const struct nuphy_msg *m, struct report *out);

uint8_t nuphy_next_seq(uint8_t seq);

/* The keyboard sends its name as a USB string descriptor cut into chunks that carry no position. */
struct nuphy_name {
	/* ASCII padded with zeros; zeros until a whole descriptor has arrived. */
	uint8_t name[NUPHY_NAME_MAX];
	uint8_t block[(NUPHY_NAME_BLOCK_MAX + NUPHY_NAME_CHUNK - 1) / NUPHY_NAME_CHUNK *
		      NUPHY_NAME_CHUNK];
	uint8_t held;
};

void nuphy_name_add(struct nuphy_name *n, const uint8_t chunk[NUPHY_NAME_CHUNK]);

/* Each builder writes one ACK-payload frame to out (NUPHY_FRAME_MAX bytes) and returns its length. */
size_t nuphy_pair_reply(uint8_t *out, uint8_t seq, const uint8_t dongle_id[NUPHY_ID_LEN],
			const uint8_t keyboard_id[NUPHY_ID_LEN], const uint8_t channels[3]);
size_t nuphy_status(uint8_t *out, uint8_t seq, const uint8_t dongle_id[NUPHY_ID_LEN],
		    uint8_t host_leds);
size_t nuphy_suspend(uint8_t *out, uint8_t seq);

#endif
