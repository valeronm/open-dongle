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
#define CONNECT_SHORT_LEN 10

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

void unifying_pairing_open(struct unifying_pairing *p, enum pairwin_cause cause, uint64_t now_ms)
{
	pairwin_open(&p->window, cause, p->has_device, (uint32_t)now_ms);
}

bool unifying_pairing_window_open(const struct unifying_pairing *p, uint64_t now_ms)
{
	return pairwin_is_open(&p->window, (uint32_t)now_ms);
}

uint8_t unifying_sweep_channel(struct unifying_sweep *s, const struct unifying_pairing *p,
			       uint64_t now_ms)
{
	if (unifying_pairing_window_open(p, now_ms) && !attempting(p) &&
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

		return !unifying_pairing_window_open(p, now_ms) || repeat
			       ? 0
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
		pairwin_paired(&p->window);
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

bool unifying_is_connect_frame(const uint8_t *frame, size_t len, uint8_t device_octet)
{
	bool shape = (len == UNIFYING_FRAME_MAX && frame[1] == 0x51 && frame[3] == 0x04) ||
		     (len == CONNECT_SHORT_LEN && frame[1] == 0x4f && frame[3] == 0x00);

	return shape && frame[2] == 0x07 && frame[0] == device_octet &&
	       unifying_checksum_ok(frame, len);
}

bool unifying_link_reconnect(struct unifying_link *l, uint8_t device_octet, const uint8_t *frame,
			     size_t len)
{
	bool held = l->keyboard_down || l->media_down;

	if (!unifying_is_connect_frame(frame, len, device_octet)) {
		return false;
	}
	l->synced = false;
	l->next_counter = 0;
	l->has_candidate = false;
	l->keyboard_down = false;
	l->media_down = false;
	return held;
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
		l->refused++;
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
		l->refused++;
		return 0;
	}
	r.keyboard[0] = p[0];
	memcpy(&r.keyboard[2], &p[1], sizeof(p) - 2);
	c = classify(l, counter, is_release(&r), attack);
	if (c.verdict == REFUSE) {
		l->refused++;
	}
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

void unifying_led_want(struct unifying_led *led, uint8_t leds)
{
	led->wanted = leds;
	if (!led->has_sent) {
		led->resend = true;
	}
}

void unifying_led_resend(struct unifying_led *led)
{
	led->resend = true;
}

bool unifying_led_due(struct unifying_led *led, bool has_device, uint32_t taken_count,
		      uint8_t *leds)
{
	if (taken_count != led->taken_count) {
		led->outstanding = false;
	}
	if (!has_device || led->outstanding || !(led->resend || led->wanted != led->sent)) {
		return false;
	}
	*leds = led->wanted;
	return true;
}

void unifying_led_sent(struct unifying_led *led, uint8_t leds, uint32_t taken_count)
{
	led->taken_count = taken_count;
	led->sent = leds;
	led->has_sent = true;
	led->resend = false;
	led->outstanding = true;
}

void unifying_led_dropped(struct unifying_led *led)
{
	if (led->outstanding) {
		led->outstanding = false;
		led->resend = true;
	}
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

void unifying_link_heard(struct unifying_link *l, const uint8_t *frame, size_t len,
			 uint64_t now_ms)
{
	uint16_t timeout = keepalive_timeout(frame, len);

	l->last_ms = now_ms;
	if (timeout != 0) {
		l->keepalive_ms = timeout;
	}
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

#define RECORD_KEY_OFFSET UNIFYING_WPID_LEN

_Static_assert(sizeof(((struct unifying_device *)0)->wpid) == UNIFYING_WPID_LEN, "record wpid");
_Static_assert(sizeof(((struct unifying_device *)0)->key) == UNIFYING_KEY_LEN, "record key");

void unifying_record_of(const struct unifying_pairing *p, uint8_t out[UNIFYING_RECORD_LEN])
{
	memcpy(out, p->device.wpid, UNIFYING_WPID_LEN);
	memcpy(&out[RECORD_KEY_OFFSET], p->device.key, UNIFYING_KEY_LEN);
}

void unifying_record_restore(struct unifying_pairing *p, const uint8_t in[UNIFYING_RECORD_LEN])
{
	struct unifying_device d;

	memcpy(d.wpid, in, UNIFYING_WPID_LEN);
	memcpy(d.key, &in[RECORD_KEY_OFFSET], UNIFYING_KEY_LEN);
	unifying_pairing_restore(p, &d);
}

static bool quiet_too_long(const struct unifying_session *s, uint64_t now_ms)
{
	uint64_t since = s->heard_ms > s->deaf_until_ms ? s->heard_ms : s->deaf_until_ms;

	/* deaf_until_ms can be later than a caller's earlier clock reading. */
	if (now_ms <= since) {
		return false;
	}
	return now_ms - since >= 2 * (uint64_t)s->link.keepalive_ms;
}

void unifying_session_init(struct unifying_session *s, const struct unifying_receiver *r,
			   const uint8_t *record, uint64_t now_ms)
{
	memset(s, 0, sizeof(*s));
	s->receiver = *r;
	s->started_ms = now_ms;
	unifying_link_reset(&s->link);
	if (record != NULL) {
		unifying_record_restore(&s->pairing, record);
	}
	s->channel = unifying_sweep_channel(&s->sweep, &s->pairing, now_ms);
}

void unifying_session_frame(struct unifying_session *s, uint8_t pipe, const uint8_t *frame,
			    size_t len, int8_t rssi, uint64_t now_ms,
			    const uint8_t nonce[UNIFYING_NONCE_LEN], unifying_aes_fn aes,
			    struct unifying_out *out)
{
	*out = (struct unifying_out){0};
	out->reply_len = unifying_pairing_rx(&s->pairing, &s->receiver, nonce, frame, len, now_ms,
					     out->reply, &out->event);
	out->reply_pipe = pipe;
	if (out->event == UNIFYING_RX_PAIRED) {
		unifying_link_reset(&s->link);
	}
	if (pipe == UNIFYING_PIPE_DEVICE) {
		uint32_t refused = s->link.refused;
		bool attack = now_ms < s->link.attack_until_ms;

		if (!s->heard) {
			out->heard = UNIFYING_HEARD_FIRST;
			out->down_ms = (uint32_t)(now_ms - s->started_ms);
		} else if (s->silent) {
			out->heard = s->connect_in_silence ? UNIFYING_HEARD_WOKE : UNIFYING_HEARD_AGAIN;
			out->down_ms = (uint32_t)(now_ms - s->silent_since_ms);
		}
		/* Requests 2 and 3 of a pairing attempt arrive on this pipe too. */
		if (!attempting(&s->pairing) && unifying_checksum_ok(frame, len)) {
			pairwin_link_up(&s->pairing.window);
		}
		s->silent = false;
		s->connect_in_silence = false;
		s->heard = true;
		s->heard_ms = now_ms;
		s->rssi = rssi;
		unifying_link_heard(&s->link, frame, len, now_ms);
		if (unifying_has_device(&s->pairing)) {
			out->reports = unifying_keystroke(&s->link, s->pairing.device.key, frame, len,
							  now_ms, aes, out->report);
			if (out->reports == 0 &&
			    unifying_media_report(&s->link, frame, len, &out->report[0])) {
				out->reports = 1;
			}
		}
		out->invalid = !unifying_checksum_ok(frame, len) || s->link.refused != refused;
		out->attack_started = !attack && now_ms < s->link.attack_until_ms;
	} else if (pipe == UNIFYING_PIPE_RECEIVER) {
		out->reconnected = unifying_is_connect_frame(frame, len, s->receiver.device_octet);
		s->connect_in_silence |= out->reconnected && s->silent;
		out->release_all = unifying_link_reconnect(&s->link, s->receiver.device_octet, frame,
							   len);
		unifying_led_resend(&s->led);
	}
	/* A frame can close the window early, while the tick sets deaf_until_ms to its full end. */
	if (!unifying_pairing_window_open(&s->pairing, now_ms) && s->deaf_until_ms > now_ms) {
		s->deaf_until_ms = now_ms;
	}
}

void unifying_session_tick(struct unifying_session *s, uint64_t now_ms, bool leds_changed,
			   uint8_t host_leds, uint32_t taken_count, struct unifying_out *out)
{
	uint8_t next;
	uint8_t step = (uint8_t)s->pairing.attempt.state;
	bool open = unifying_pairing_window_open(&s->pairing, now_ms);
	bool pending;

	*out = (struct unifying_out){0};
	/* The pairing sweep leaves the keyboard's channel, so the window is deaf time. */
	if (open && !s->silent) {
		s->deaf_until_ms = now_ms + pairwin_left_ms(&s->pairing.window, (uint32_t)now_ms);
	}
	if (s->heard && !s->silent && unifying_has_device(&s->pairing) &&
	    quiet_too_long(s, now_ms)) {
		s->silent = true;
		s->silent_since_ms = now_ms;
		out->went_silent = true;
		out->silent_ms = (uint32_t)(now_ms - s->heard_ms);
	}
	out->release_all = unifying_link_silent(&s->link, now_ms);
	if (leds_changed) {
		unifying_led_want(&s->led, host_leds);
	}
	out->send_led = unifying_led_due(&s->led, unifying_has_device(&s->pairing), taken_count,
					 &out->leds);
	out->attempt_expired = unifying_pairing_expired(&s->pairing, now_ms);
	if (out->attempt_expired) {
		_Static_assert(UNIFYING_ANSWERED_1 == 1 && UNIFYING_ANSWERED_2 == 2,
			       "expired_step is the state");
		out->expired_step = step;
	}
	pending = step != UNIFYING_IDLE && !out->attempt_expired;
	out->window_closed = !pending && pairwin_ran_out(&s->pairing.window, (uint32_t)now_ms);
	next = unifying_sweep_channel(&s->sweep, &s->pairing, now_ms);
	if (next != s->channel) {
		out->set_channel = true;
		out->channel = next;
		s->channel = next;
	}
}

void unifying_session_led_queued(struct unifying_session *s, uint8_t leds, uint32_t taken_count)
{
	unifying_led_sent(&s->led, leds, taken_count);
}

void unifying_session_channel_moved(struct unifying_session *s)
{
	unifying_led_dropped(&s->led);
}

void unifying_session_open_pairing(struct unifying_session *s, enum pairwin_cause cause,
				    uint64_t now_ms)
{
	unifying_pairing_open(&s->pairing, cause, now_ms);
}

enum unifying_status unifying_session_status(const struct unifying_session *s, uint64_t now_ms)
{
	if (unifying_pairing_window_open(&s->pairing, now_ms)) {
		return UNIFYING_PAIRING;
	}
	if (!unifying_has_device(&s->pairing)) {
		return UNIFYING_NOT_PAIRED;
	}
	if (s->heard && !quiet_too_long(s, now_ms)) {
		return UNIFYING_CONNECTED;
	}
	return UNIFYING_SILENT;
}

void unifying_session_radio_resumed(struct unifying_session *s, uint64_t now_ms)
{
	if (!s->silent) {
		s->deaf_until_ms = now_ms;
	}
}
