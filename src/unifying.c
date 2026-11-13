#include <string.h>
#include "unifying.h"

#define MARKER_REQUEST 0x5f
#define MARKER_RESPONSE 0x1f
#define MARKER_FINAL 0x0f
#define STEP_FINAL 0x06
#define CAP_UNIFYING 0x04
#define REPORT_TYPE_MASK 0x1f
#define REPORT_ENCRYPTED_KEYBOARD 0x13
#define REPORT_MEDIA 0x03
#define REPORT_LEDS 0x0e
#define FINAL_LEN 10
#define MEDIA_FRAME_LEN 10
#define PLAINTEXT_MARKER 0xc9
#define REPORT_KEEPALIVE 0x40
#define REPORT_SET_KEEPALIVE 0x0f
#define KEEPALIVE_LEN 5
#define SET_KEEPALIVE_LEN 10

const uint8_t unifying_pairing_address[UNIFYING_ADDRESS_LEN] = {0xbb, 0x0a, 0xdc, 0xa5, 0x75};
const uint8_t unifying_pairing_channels[UNIFYING_PAIRING_CHANNELS] = {5,  8,  17, 32, 35, 41,
								      44, 62, 65, 71, 74};

void unifying_device_address(const struct unifying_receiver *r, uint8_t out[UNIFYING_ADDRESS_LEN])
{
	memcpy(out, r->base, sizeof(r->base));
	out[4] = r->device_octet;
}

static bool attempting(const struct unifying_pairing *p)
{
	return p->attempt.state != UNIFYING_IDLE;
}

static bool window_open(uint64_t now_ms)
{
	return now_ms < UNIFYING_PAIRING_WINDOW_MS;
}

uint8_t unifying_sweep_channel(struct unifying_sweep *s, const struct unifying_pairing *p,
			       uint64_t now_ms)
{
	if (window_open(now_ms) && !attempting(p) && !p->has_device &&
	    now_ms - s->last_ms >= UNIFYING_SWEEP_DWELL_MS) {
		s->index = (s->index + 1) % UNIFYING_PAIRING_CHANNELS;
		s->last_ms = now_ms;
	}
	return unifying_pairing_channels[s->index];
}

uint8_t unifying_checksum(const uint8_t *buf, size_t len)
{
	uint8_t sum = 0;

	for (size_t i = 0; i < len; i++) {
		sum += buf[i];
	}
	return (uint8_t)(0 - sum);
}

bool unifying_checksum_ok(const uint8_t *frame, size_t len)
{
	uint8_t want;

	if (len < 2) {
		return false;
	}
	want = unifying_checksum(frame, len - 1);
	return (frame[len - 1] | 0x01) == (want | 0x01);
}

void unifying_derive_key(uint8_t key[UNIFYING_KEY_LEN], const uint8_t base[4],
			 const uint8_t device_wpid[2], const uint8_t dongle_wpid[2],
			 const uint8_t device_nonce[UNIFYING_NONCE_LEN],
			 const uint8_t dongle_nonce[UNIFYING_NONCE_LEN])
{
	static const uint8_t index[UNIFYING_KEY_LEN] = {7,  1, 0, 3,  10, 2,  9, 14,
							8,  6, 12, 5, 13, 15, 4, 11};
	static const uint8_t flip[UNIFYING_KEY_LEN] = {0x00, 0xff, 0x00, 0x00, 0x00, 0xff,
						       0x55, 0x00, 0x00, 0x00, 0xff, 0x00,
						       0x00, 0x55, 0x00, 0x00};
	uint8_t data[UNIFYING_KEY_LEN];

	memcpy(&data[0], base, 4);
	memcpy(&data[4], device_wpid, 2);
	memcpy(&data[6], dongle_wpid, 2);
	memcpy(&data[8], device_nonce, UNIFYING_NONCE_LEN);
	memcpy(&data[12], dongle_nonce, UNIFYING_NONCE_LEN);
	for (size_t i = 0; i < UNIFYING_KEY_LEN; i++) {
		key[i] = data[index[i]] ^ flip[i];
	}
}

static size_t seal(uint8_t *frame, size_t len)
{
	frame[len - 1] = unifying_checksum(frame, len - 1);
	return len;
}

static size_t response_1(const struct unifying_attempt *a, const struct unifying_receiver *r,
			 uint8_t *out)
{
	memset(out, 0, UNIFYING_FRAME_MAX);
	out[0] = a->id;
	out[1] = MARKER_RESPONSE;
	out[2] = 0x01;
	memcpy(&out[3], r->base, 4);
	out[7] = r->device_octet;
	out[8] = 0x08;
	memcpy(&out[9], r->wpid, 2);
	out[13] = a->device_type;
	out[14] = a->caps;
	return seal(out, UNIFYING_FRAME_MAX);
}

static size_t response_2(const struct unifying_attempt *a, const uint8_t *request, uint8_t *out)
{
	memset(out, 0, UNIFYING_FRAME_MAX);
	out[0] = a->id;
	out[1] = MARKER_RESPONSE;
	out[2] = 0x02;
	memcpy(&out[3], a->dongle_nonce, UNIFYING_NONCE_LEN);
	memcpy(&out[7], a->serial, 4);
	memcpy(&out[11], &request[11], 2);
	return seal(out, UNIFYING_FRAME_MAX);
}

static size_t response_final(const struct unifying_attempt *a, uint8_t *out)
{
	memset(out, 0, FINAL_LEN);
	out[0] = a->id;
	out[1] = MARKER_FINAL;
	out[2] = STEP_FINAL;
	memcpy(&out[3], a->device_nonce, UNIFYING_NONCE_LEN);
	memcpy(&out[7], a->dongle_nonce, 2);
	return seal(out, FINAL_LEN);
}

static uint8_t request_step(const uint8_t *frame, size_t len)
{
	if (len != UNIFYING_FRAME_MAX || (frame[1] & 0x7f) != MARKER_REQUEST ||
	    !unifying_checksum_ok(frame, len) || frame[2] < 1 || frame[2] > 3) {
		return 0;
	}
	return frame[2];
}

static size_t start(struct unifying_attempt *a, const struct unifying_receiver *r,
		    const uint8_t nonce[UNIFYING_NONCE_LEN], const uint8_t *frame, uint64_t now_ms,
		    uint8_t *reply, enum unifying_rx_event *event)
{
	if (!(frame[14] & CAP_UNIFYING)) {
		*a = (struct unifying_attempt){0};
		*event = UNIFYING_RX_REFUSED;
		return 0;
	}
	*a = (struct unifying_attempt){
		.state = UNIFYING_ANSWERED_1,
		.id = frame[0],
		.last_ms = now_ms,
		.dev.wpid = {frame[9], frame[10]},
		.device_type = frame[13],
		.caps = frame[14],
	};
	memcpy(a->dongle_nonce, nonce, UNIFYING_NONCE_LEN);
	*event = UNIFYING_RX_ANSWERED_1;
	return response_1(a, r, reply);
}

size_t unifying_pairing_rx(struct unifying_pairing *p, const struct unifying_receiver *r,
			   const uint8_t nonce[UNIFYING_NONCE_LEN], const uint8_t *frame, size_t len,
			   uint64_t now_ms, uint8_t *reply, enum unifying_rx_event *event)
{
	struct unifying_attempt *a = &p->attempt;
	uint8_t step = request_step(frame, len);

	*event = UNIFYING_RX_NONE;
	if (step == 1) {
		bool repeat = attempting(p) && frame[0] == a->id;

		return !window_open(now_ms) || repeat ? 0
						       : start(a, r, nonce, frame, now_ms, reply, event);
	}
	if (step == 2 && a->state == UNIFYING_ANSWERED_1 && frame[0] == a->id) {
		memcpy(a->device_nonce, &frame[3], UNIFYING_NONCE_LEN);
		memcpy(a->serial, &frame[7], 4);
		unifying_derive_key(a->dev.key, r->base, a->dev.wpid, r->wpid, a->device_nonce,
				    a->dongle_nonce);
		a->state = UNIFYING_ANSWERED_2;
		a->last_ms = now_ms;
		*event = UNIFYING_RX_ANSWERED_2;
		return response_2(a, frame, reply);
	}
	/* Request 3 carries 0x00 in the id byte, not the attempt's id. */
	if (step == 3 && a->state == UNIFYING_ANSWERED_2) {
		a->name_len = frame[4] < UNIFYING_NAME_MAX ? frame[4] : UNIFYING_NAME_MAX;
		memcpy(a->name, &frame[5], a->name_len);
		a->state = UNIFYING_IDLE;
		p->has_device = true;
		p->device = a->dev;
		*event = UNIFYING_RX_PAIRED;
		return response_final(a, reply);
	}
	return 0;
}

bool unifying_pairing_expired(struct unifying_pairing *p, uint64_t now_ms)
{
	if (attempting(p) && now_ms - p->attempt.last_ms > UNIFYING_PAIRING_TIMEOUT_MS) {
		p->attempt = (struct unifying_attempt){0};
		return true;
	}
	return false;
}

bool unifying_has_device(const struct unifying_pairing *p)
{
	return p->has_device;
}

void unifying_pairing_restore(struct unifying_pairing *p, const struct unifying_device *d)
{
	*p = (struct unifying_pairing){.has_device = true, .device = *d};
}

static bool frame_is(const uint8_t *frame, size_t len, size_t want_len, uint8_t want_type)
{
	return len == want_len && (frame[1] & REPORT_TYPE_MASK) == want_type &&
	       unifying_checksum_ok(frame, len);
}

bool unifying_is_encrypted_keyboard(const uint8_t *frame, size_t len)
{
	return frame_is(frame, len, UNIFYING_FRAME_MAX, REPORT_ENCRYPTED_KEYBOARD);
}

void unifying_link_reset(struct unifying_link *l)
{
	*l = (struct unifying_link){.keepalive_ms = UNIFYING_KEEPALIVE_DEFAULT_MS};
}

void unifying_link_set_floor(struct unifying_link *l, uint64_t next_counter)
{
	l->synced = next_counter != 0;
	l->next_counter = next_counter;
}

static void counter_block(const uint8_t *frame, uint8_t block[UNIFYING_AES_BLOCK_LEN])
{
	static const uint8_t head[7] = {0x04, 0x14, 0x1d, 0x1f, 0x27, 0x28, 0x0d};
	static const uint8_t tail[5] = {0x0a, 0x0d, 0x13, 0x26, 0x0e};

	memcpy(block, head, sizeof(head));
	memcpy(&block[sizeof(head)], &frame[10], 4);
	memcpy(&block[sizeof(head) + 4], tail, sizeof(tail));
}

static bool is_release(const struct report *r)
{
	uint8_t keys = 0;

	for (size_t i = 0; i < sizeof(r->keyboard); i++) {
		keys |= r->keyboard[i];
	}
	return keys == 0;
}

static bool beyond_window(const struct unifying_link *l, uint64_t counter)
{
	return counter >= l->next_counter + UNIFYING_COUNTER_WINDOW;
}

enum verdict {
	REFUSE,
	HOLD,
	TYPE,
};

struct classified {
	enum verdict verdict;
	bool types_candidate;
};

/* A held jump is never typed itself; only the frame that confirms it is. */
static struct classified classify(const struct unifying_link *l, uint64_t counter, bool release,
				  bool attack)
{
	bool confirms = l->has_candidate && counter == l->candidate.counter + 1;

	if (l->synced && !attack) {
		return (struct classified){
			.verdict = confirms || !beyond_window(l, counter) ? TYPE : HOLD};
	}
	if (attack && (l->synced ? !confirms && beyond_window(l, counter) : !release)) {
		return (struct classified){.verdict = REFUSE};
	}
	return (struct classified){
		.verdict = release || (confirms && !attack) ? TYPE : HOLD,
		.types_candidate = confirms,
	};
}

static void type(struct unifying_link *l, const struct report *r, uint64_t next_counter,
		 struct report *out)
{
	l->synced = true;
	l->next_counter = next_counter;
	l->unsaved = true;
	l->keyboard_down = !is_release(r);
	*out = *r;
}

size_t unifying_keystroke(struct unifying_link *l, const uint8_t key[UNIFYING_KEY_LEN],
			  const uint8_t *frame, size_t len, uint64_t now_ms, unifying_aes_fn aes,
			  struct report out[UNIFYING_KEYSTROKE_REPORTS])
{
	uint8_t block[UNIFYING_AES_BLOCK_LEN], stream[UNIFYING_AES_BLOCK_LEN];
	uint8_t p[REPORT_KEYBOARD_LEN];
	struct report r = {.kind = REPORT_KEYBOARD};
	bool attack = now_ms < l->attack_until_ms;
	struct classified c;
	uint64_t counter;
	size_t n = 0;

	if (!unifying_is_encrypted_keyboard(frame, len)) {
		return 0;
	}
	if (!attack && l->candidate.held_in_attack) {
		l->has_candidate = false;
	}
	counter = (uint64_t)frame[10] << 24 | (uint64_t)frame[11] << 16 | (uint64_t)frame[12] << 8 |
		  frame[13];
	if (counter < l->next_counter || counter >= UNIFYING_COUNTER_LIMIT) {
		/* Only a jump, held with a counter known and no attack, survives a refused frame. */
		if (!l->synced || attack) {
			l->has_candidate = false;
		}
		return 0;
	}
	counter_block(frame, block);
	aes(key, block, stream);
	for (size_t i = 0; i < sizeof(p); i++) {
		p[i] = frame[2 + i] ^ stream[i];
	}
	if (p[7] != PLAINTEXT_MARKER) {
		l->has_candidate = false;
		l->attack_until_ms = now_ms + UNIFYING_ATTACK_MS;
		return 0;
	}
	r.keyboard[0] = p[0];
	memcpy(&r.keyboard[2], &p[1], sizeof(p) - 2);
	c = classify(l, counter, is_release(&r), attack);
	if (c.types_candidate) {
		type(l, &l->candidate.report, counter, &out[n++]);
	}
	l->has_candidate = c.verdict == HOLD;
	if (c.verdict == HOLD) {
		l->candidate = (struct unifying_candidate){
			.counter = counter, .report = r, .held_in_attack = attack};
	} else if (c.verdict == TYPE) {
		type(l, &r, counter + 1, &out[n++]);
	}
	return n;
}

bool unifying_media_report(struct unifying_link *l, const uint8_t *frame, size_t len,
			   struct report *out)
{
	if (!frame_is(frame, len, MEDIA_FRAME_LEN, REPORT_MEDIA)) {
		return false;
	}
	*out = (struct report){.kind = REPORT_CONSUMER, .usage = frame[2] | frame[3] << 8};
	l->media_down = out->usage != 0;
	return true;
}

size_t unifying_led_frame(uint8_t leds, uint8_t out[UNIFYING_LED_FRAME_LEN])
{
	memset(out, 0, UNIFYING_LED_FRAME_LEN);
	out[1] = REPORT_LEDS;
	out[2] = leds;
	return seal(out, UNIFYING_LED_FRAME_LEN);
}

void unifying_led_request(struct unifying_led *led)
{
	led->pending = true;
}

bool unifying_led_due(const struct unifying_led *led, bool has_device, uint64_t now_ms)
{
	return has_device && led->pending &&
	       (!led->sent || now_ms - led->last_ms >= UNIFYING_LED_INTERVAL_MS);
}

void unifying_led_sent(struct unifying_led *led, uint64_t now_ms)
{
	*led = (struct unifying_led){.sent = true, .last_ms = now_ms};
}

static uint16_t keepalive_timeout(const uint8_t *frame, size_t len)
{
	if (frame_is(frame, len, KEEPALIVE_LEN, REPORT_KEEPALIVE & REPORT_TYPE_MASK) &&
	    frame[1] == REPORT_KEEPALIVE) {
		return (uint16_t)(frame[2] << 8 | frame[3]);
	}
	if (frame_is(frame, len, SET_KEEPALIVE_LEN, REPORT_SET_KEEPALIVE) && frame[2] == 0x00) {
		return (uint16_t)(frame[3] << 8 | frame[4]);
	}
	return 0;
}

bool unifying_link_heard(struct unifying_link *l, const uint8_t *frame, size_t len,
			 uint64_t now_ms, uint64_t *floor)
{
	uint16_t timeout = keepalive_timeout(frame, len);

	l->last_ms = now_ms;
	if (timeout != 0) {
		l->keepalive_ms = timeout;
	}
	if (timeout < UNIFYING_IDLE_KEEPALIVE_MS || !l->unsaved) {
		return false;
	}
	l->unsaved = false;
	*floor = l->next_counter;
	return true;
}

bool unifying_link_silent(struct unifying_link *l, uint64_t now_ms)
{
	bool silent = (l->keyboard_down || l->media_down) &&
		      now_ms - l->last_ms > 2 * (uint64_t)l->keepalive_ms;

	if (silent) {
		l->keyboard_down = false;
		l->media_down = false;
	}
	return silent;
}
