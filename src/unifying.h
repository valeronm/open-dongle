#ifndef UNIFYING_H
#define UNIFYING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "reports.h"

#define UNIFYING_FRAME_MAX 22
#define UNIFYING_KEY_LEN 16
#define UNIFYING_NAME_MAX 16
#define UNIFYING_NONCE_LEN 4
#define UNIFYING_PAIRING_TIMEOUT_MS 5000
#define UNIFYING_ADDRESS_LEN 5
#define UNIFYING_PAIRING_CHANNELS 11
#define UNIFYING_SWEEP_DWELL_MS 20
#define UNIFYING_PAIRING_WINDOW_MS 30000
#define UNIFYING_AES_BLOCK_LEN 16
#define UNIFYING_KEEPALIVE_DEFAULT_MS 1100
#define UNIFYING_LED_FRAME_LEN 10
#define UNIFYING_COUNTER_WINDOW 16
#define UNIFYING_ATTACK_MS 5000
#define UNIFYING_KEYSTROKE_REPORTS 2
#define UNIFYING_WPID_LEN 2
#define UNIFYING_RECORD_LEN (UNIFYING_WPID_LEN + UNIFYING_KEY_LEN)
#define UNIFYING_COUNTER_LIMIT (0x100000000ULL - (1ULL << 20))

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
};

struct unifying_device {
	uint8_t wpid[2];
	uint8_t key[UNIFYING_KEY_LEN];
};

struct unifying_attempt {
	enum unifying_state state;
	uint8_t id;
	uint64_t last_ms;
	struct unifying_device dev;
	uint8_t device_type;
	uint8_t caps;
	uint8_t device_nonce[UNIFYING_NONCE_LEN];
	uint8_t dongle_nonce[UNIFYING_NONCE_LEN];
	uint8_t serial[4];
	uint8_t name[UNIFYING_NAME_MAX];
	uint8_t name_len;
};

struct unifying_pairing {
	bool has_device;
	struct unifying_device device;
	struct unifying_attempt attempt;
	uint64_t window_until_ms;
};

enum unifying_rx_event {
	UNIFYING_RX_NONE,
	UNIFYING_RX_REFUSED,
	UNIFYING_RX_ANSWERED_1,
	UNIFYING_RX_ANSWERED_2,
	UNIFYING_RX_PAIRED,
};

struct unifying_sweep {
	uint8_t index;
	uint64_t last_ms;
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
 * reply (UNIFYING_FRAME_MAX bytes), or 0 when there is none, and sets *event to what the frame did.
 * nonce is copied in when Request 1 starts an attempt, so it must be one no earlier attempt used.
 * A repeat of an answered request gets 0, since ESB keeps its reply queued until the keyboard moves
 * on. A Request 1 lacking the Unifying-compatible capability discards any attempt in progress. The
 * paired device changes only when Request 3 completes an attempt. A Request 1 is ignored while the
 * window is closed; Requests 2 and 3 are not.
 */
size_t unifying_pairing_rx(struct unifying_pairing *p, const struct unifying_receiver *r,
			   const uint8_t nonce[UNIFYING_NONCE_LEN], const uint8_t *frame, size_t len,
			   uint64_t now_ms, uint8_t *reply, enum unifying_rx_event *event);

/*
 * Discards an attempt that has heard no request for UNIFYING_PAIRING_TIMEOUT_MS and returns true;
 * the paired device is kept.
 */
bool unifying_pairing_expired(struct unifying_pairing *p, uint64_t now_ms);

/*
 * Returns the channel to listen on: while pairing is open and no attempt runs it steps through the
 * pairing channels every UNIFYING_SWEEP_DWELL_MS, since the keyboard hops them too; otherwise it
 * holds the current one.
 */
uint8_t unifying_sweep_channel(struct unifying_sweep *s, const struct unifying_pairing *p,
			       uint64_t now_ms);

/* Opens the window for UNIFYING_PAIRING_WINDOW_MS; a completed pairing closes it. */
void unifying_pairing_open(struct unifying_pairing *p, uint64_t now_ms);

bool unifying_pairing_window_open(const struct unifying_pairing *p, uint64_t now_ms);

bool unifying_has_device(const struct unifying_pairing *p);

/* Discards whatever state p holds, an attempt in progress included, and pairs d. */
void unifying_pairing_restore(struct unifying_pairing *p, const struct unifying_device *d);

bool unifying_is_encrypted_keyboard(const uint8_t *frame, size_t len);

typedef void (*unifying_aes_fn)(const uint8_t key[UNIFYING_KEY_LEN],
				const uint8_t in[UNIFYING_AES_BLOCK_LEN],
				uint8_t out[UNIFYING_AES_BLOCK_LEN]);

struct unifying_candidate {
	uint64_t counter;
	struct report report;
	bool held_in_attack;
};

struct unifying_link {
	bool synced;
	uint64_t next_counter;
	bool has_candidate;
	struct unifying_candidate candidate;
	uint64_t attack_until_ms;
	uint16_t keepalive_ms;
	uint64_t last_ms;
	bool keyboard_down;
	bool media_down;
	uint32_t refused;
};

void unifying_link_reset(struct unifying_link *l);

/* A connect frame is what a keyboard sends on the receiver's own address after it boots. */
bool unifying_is_connect_frame(const uint8_t *frame, size_t len, uint8_t device_octet);

/*
 * The keyboard picks a new counter on every boot, so a connect frame from the keyboard with
 * device_octet, received on the receiver's own address, forgets the known counter and drops any
 * held frame. Returns true when keys were held, which are then to be released.
 */
bool unifying_link_reconnect(struct unifying_link *l, uint8_t device_octet, const uint8_t *frame,
			     size_t len);

/*
 * Decrypts an encrypted keystroke and writes up to UNIFYING_KEYSTROKE_REPORTS reports to out,
 * returning their number, to be typed in order. A counter below next_counter or from
 * UNIFYING_COUNTER_LIMIT up is refused before aes is called; a plaintext without 0xc9 is refused
 * and starts an attack lasting UNIFYING_ATTACK_MS.
 *
 * A release is typed at once. A press is typed at once only with a counter known, no attack and a
 * counter within UNIFYING_COUNTER_WINDOW of next_counter; otherwise it is held and typed when the
 * next frame carries its counter + 1. Beyond the window with a counter known and no attack, the
 * held press is a jump whose own report is never typed, only the confirming frame's. During an
 * attack a press with no counter known is refused, as is any frame beyond the window that
 * confirms nothing.
 */
size_t unifying_keystroke(struct unifying_link *l, const uint8_t key[UNIFYING_KEY_LEN],
			  const uint8_t *frame, size_t len, uint64_t now_ms, unifying_aes_fn aes,
			  struct report out[UNIFYING_KEYSTROKE_REPORTS]);

/* The usage's high byte (frame byte 3) is unconfirmed: every key seen used a usage below 0x100. */
bool unifying_media_report(struct unifying_link *l, const uint8_t *frame, size_t len,
			   struct report *out);

size_t unifying_led_frame(uint8_t leds, uint8_t out[UNIFYING_LED_FRAME_LEN]);

/* At most one LED frame waits in the ESB payload queue, which pairing replies share. */
struct unifying_led {
	uint8_t wanted;
	uint8_t sent;
	bool has_sent;
	bool resend;
	bool outstanding;
	uint32_t taken_count;
};

void unifying_led_want(struct unifying_led *led, uint8_t leds);

void unifying_led_resend(struct unifying_led *led);

/*
 * Returns true, with *leds to send, while a device is paired, no LED frame is outstanding, and
 * either a resend is requested or the wanted state differs from the last one sent. An outstanding
 * frame counts as taken once taken_count differs from the count given to unifying_led_sent.
 */
bool unifying_led_due(struct unifying_led *led, bool has_device, uint32_t taken_count,
		      uint8_t *leds);

/*
 * Records leds as queued; taken_count is sampled before queueing, as the frame can be taken at
 * once.
 */
void unifying_led_sent(struct unifying_led *led, uint8_t leds, uint32_t taken_count);

/* An outstanding frame that left the queue untaken is sent again. */
void unifying_led_dropped(struct unifying_led *led);

/* The record is the wpid followed by the key. */
void unifying_record_of(const struct unifying_pairing *p, uint8_t out[UNIFYING_RECORD_LEN]);

/* Discards whatever state p holds and pairs the recorded device. */
void unifying_record_restore(struct unifying_pairing *p, const uint8_t in[UNIFYING_RECORD_LEN]);

/*
 * Records any frame from the keyboard; keep-alive and set-keep-alive frames also carry its timeout.
 */
void unifying_link_heard(struct unifying_link *l, const uint8_t *frame, size_t len,
			 uint64_t now_ms);

/*
 * Returns true once when a key is held and nothing was heard for twice the keep-alive timeout; the
 * held state is then cleared.
 */
bool unifying_link_silent(struct unifying_link *l, uint64_t now_ms);

#define UNIFYING_PIPE_PAIRING 0
#define UNIFYING_PIPE_DEVICE 1
#define UNIFYING_PIPE_RECEIVER 2

enum unifying_status {
	UNIFYING_NOT_PAIRED,
	UNIFYING_PAIRING,
	UNIFYING_CONNECTED,
	UNIFYING_SILENT,
};

struct unifying_session {
	struct unifying_receiver receiver;
	struct unifying_pairing pairing;
	struct unifying_link link;
	struct unifying_led led;
	struct unifying_sweep sweep;
	uint8_t channel;
	bool heard;
	uint64_t heard_ms;
	int8_t rssi;
	uint64_t started_ms;
	bool silent;
	uint64_t silent_since_ms;
	bool connect_in_silence;
	bool window_was_open;
	uint64_t deaf_until_ms;
};

enum unifying_heard {
	UNIFYING_HEARD_NONE,
	UNIFYING_HEARD_FIRST,
	UNIFYING_HEARD_AGAIN,
	/* Heard again after a connect frame arrived during the silence. */
	UNIFYING_HEARD_WOKE,
};

/* What the caller must do after a session call; zeroed by the call. */
struct unifying_out {
	enum unifying_rx_event event;
	uint8_t reply_pipe;
	size_t reply_len;
	uint8_t reply[UNIFYING_FRAME_MAX];
	size_t reports;
	struct report report[UNIFYING_KEYSTROKE_REPORTS];
	bool release_all;
	bool send_led;
	uint8_t leds;
	bool attempt_expired;
	bool set_channel;
	uint8_t channel;
	bool went_silent;
	/* Time since the last frame when the silence was noticed. */
	uint32_t silent_ms;
	enum unifying_heard heard;
	/* For UNIFYING_HEARD_FIRST, time since the session started; otherwise since the silence was
	 * noticed. */
	uint32_t down_ms;
	bool reconnected;
	bool attack_started;
	/* A device-address frame that fails its checksum or a refused keystroke. */
	bool invalid;
	bool window_closed;
	/* The handshake step an expired attempt had answered, 1 or 2. */
	uint8_t expired_step;
};

/* record is NULL when no keyboard is stored. */
void unifying_session_init(struct unifying_session *s, const struct unifying_receiver *r,
			   const uint8_t *record, uint64_t now_ms);

void unifying_session_frame(struct unifying_session *s, uint8_t pipe, const uint8_t *frame,
			    size_t len, int8_t rssi, uint64_t now_ms,
			    const uint8_t nonce[UNIFYING_NONCE_LEN], unifying_aes_fn aes,
			    struct unifying_out *out);

void unifying_session_tick(struct unifying_session *s, uint64_t now_ms, bool leds_changed,
			   uint8_t host_leds, uint32_t taken_count, struct unifying_out *out);

/* taken_count is sampled before the LED frame was queued. */
void unifying_session_led_queued(struct unifying_session *s, uint8_t leds, uint32_t taken_count);

/* The radio moved to out->channel, dropping any queued replies. */
void unifying_session_channel_moved(struct unifying_session *s);

void unifying_session_open_pairing(struct unifying_session *s, uint64_t now_ms);

/* The receiver was off until now_ms, so silence before it is not the keyboard's. */
void unifying_session_radio_resumed(struct unifying_session *s, uint64_t now_ms);

enum unifying_status unifying_session_status(const struct unifying_session *s, uint64_t now_ms);

#endif
