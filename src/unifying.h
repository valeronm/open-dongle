#ifndef UNIFYING_H
#define UNIFYING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNIFYING_FRAME_MAX 22
#define UNIFYING_KEY_LEN 16
#define UNIFYING_NAME_MAX 16
#define UNIFYING_NONCE_LEN 4
#define UNIFYING_PAIRING_TIMEOUT_MS 5000
#define UNIFYING_ADDRESS_LEN 5
#define UNIFYING_PAIRING_CHANNELS 11
#define UNIFYING_SWEEP_DWELL_MS 20
#define UNIFYING_PAIRING_WINDOW_MS 60000

/* Addresses are written in the order the Unifying sources print them, the device octet last. */
extern const uint8_t unifying_pairing_address[UNIFYING_ADDRESS_LEN];
extern const uint8_t unifying_pairing_channels[UNIFYING_PAIRING_CHANNELS];

/* What the receiver hands out in Pairing Response 1; base and wpid also enter the key. */
struct unifying_receiver {
	uint8_t base[4];
	uint8_t device_octet;
	uint8_t wpid[2];
};

enum unifying_state {
	UNIFYING_IDLE,
	UNIFYING_ANSWERED_1,
	UNIFYING_ANSWERED_2,
	UNIFYING_PAIRED,
};

struct unifying_pairing {
	enum unifying_state state;
	uint8_t id;
	uint32_t last_ms;
	uint8_t device_wpid[2];
	uint8_t device_type;
	uint8_t caps;
	uint8_t device_nonce[UNIFYING_NONCE_LEN];
	uint8_t dongle_nonce[UNIFYING_NONCE_LEN];
	uint8_t serial[4];
	uint8_t name[UNIFYING_NAME_MAX];
	uint8_t name_len;
	uint8_t key[UNIFYING_KEY_LEN];
};

struct unifying_device {
	uint8_t wpid[2];
	uint8_t key[UNIFYING_KEY_LEN];
};

struct unifying_sweep {
	uint8_t index;
	uint32_t last_ms;
};

uint8_t unifying_checksum(const uint8_t *buf, size_t len);

/* The last byte is the checksum of the rest; keyboards sometimes send its low bit wrong. */
bool unifying_checksum_ok(const uint8_t *frame, size_t len);

void unifying_derive_key(uint8_t key[UNIFYING_KEY_LEN], const uint8_t base[4],
			 const uint8_t device_wpid[2], const uint8_t dongle_wpid[2],
			 const uint8_t device_nonce[UNIFYING_NONCE_LEN],
			 const uint8_t dongle_nonce[UNIFYING_NONCE_LEN]);

void unifying_device_address(const struct unifying_receiver *r, uint8_t out[UNIFYING_ADDRESS_LEN]);

/*
 * Feeds one frame received on the pairing or device address, since Requests 2 and 3 arrive on the
 * device address. Returns the length of the response to queue as the next ACK payload, written to
 * reply (UNIFYING_FRAME_MAX bytes), or 0 when there is none. nonce is copied in when Request 1
 * starts an attempt, so it must be one no earlier attempt used. A repeat of an answered request
 * gets 0, since ESB keeps its reply queued until the keyboard moves on; a Request 1 with a new id
 * restarts pairing from any state. *refused is set when a Request 1 lacks the Unifying-compatible
 * capability. A Request 1 is ignored from UNIFYING_PAIRING_WINDOW_MS on; Requests 2 and 3 are not.
 */
size_t unifying_pairing_rx(struct unifying_pairing *p, const struct unifying_receiver *r,
			   const uint8_t nonce[UNIFYING_NONCE_LEN], const uint8_t *frame, size_t len,
			   uint32_t now_ms, uint8_t *reply, bool *refused);

/* Drops an attempt that has heard no request for UNIFYING_PAIRING_TIMEOUT_MS and returns true. */
bool unifying_pairing_expired(struct unifying_pairing *p, uint32_t now_ms);

/*
 * Returns the channel to listen on: until UNIFYING_PAIRING_WINDOW_MS it steps through the pairing
 * channels every UNIFYING_SWEEP_DWELL_MS, since the keyboard hops them too; during an attempt and
 * after the window it holds the current one.
 */
uint8_t unifying_sweep_channel(struct unifying_sweep *s, const struct unifying_pairing *p,
			       uint32_t now_ms);

void unifying_device_of(const struct unifying_pairing *p, struct unifying_device *d);

/* Discards whatever state p holds, an attempt in progress included. */
void unifying_pairing_restore(struct unifying_pairing *p, const struct unifying_device *d);

bool unifying_is_encrypted_keyboard(const uint8_t *frame, size_t len);

#endif
