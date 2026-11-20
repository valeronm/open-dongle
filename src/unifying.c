#include <string.h>
#include "unifying.h"

#define MARKER_REQUEST 0x5f
#define MARKER_RESPONSE 0x1f
#define MARKER_FINAL 0x0f
#define STEP_FINAL 0x06
#define CAP_UNIFYING 0x04
#define TYPE_KEYBOARD 0x01
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
#define REPORT_HIDPP_SHORT 0x10
#define REPORT_HIDPP_LONG 0x11
#define HIDPP_SHORT_LEN 10
#define HIDPP_ROOT 0x00
#define HIDPP_ERROR 0xff
#define HIDPP_REPROG_CONTROLS 0x1b04
#define HIDPP_BATTERY_STATUS 0x1000
/* A function number in the high nibble, the software ID 1 in the low one; events carry ID 0. */
#define HIDPP_GET_FEATURE 0x01
#define HIDPP_SET_CID_REPORTING 0x31
#define HIDPP_GET_BATTERY 0x01
#define HIDPP_BATTERY_EVENT 0x00
#define HIDPP_DIVERTED_EVENT 0x00
/* The divert bit and the bit that says it is valid; the persist bits stay clear. */
#define HIDPP_DIVERT 0x03

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
	out[13] = a->dev.type;
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
	memcpy(&out[7], a->dev.serial, UNIFYING_SERIAL_LEN);
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
	if (!(frame[14] & CAP_UNIFYING) || frame[13] != TYPE_KEYBOARD) {
		*a = (struct unifying_attempt){0};
		*event = UNIFYING_RX_REFUSED;
		return 0;
	}
	*a = (struct unifying_attempt){
		.state = UNIFYING_ANSWERED_1,
		.id = frame[0],
		.last_ms = now_ms,
		.dev = {.wpid = {frame[9], frame[10]}, .type = frame[13]},
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
		memcpy(a->dev.serial, &frame[7], UNIFYING_SERIAL_LEN);
		unifying_derive_key(a->dev.key, r->base, a->dev.wpid, r->wpid, a->device_nonce,
				    a->dongle_nonce);
		a->state = UNIFYING_ANSWERED_2;
		a->last_ms = now_ms;
		*event = UNIFYING_RX_ANSWERED_2;
		return response_2(a, frame, reply);
	}
	/* Request 3 carries 0x00 in the id byte, not the attempt's id. */
	if (step == 3 && a->state == UNIFYING_ANSWERED_2) {
		memcpy(a->dev.name, &frame[5],
		       frame[4] < UNIFYING_NAME_MAX ? frame[4] : UNIFYING_NAME_MAX);
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

static bool shaped(const uint8_t *frame, size_t len, size_t want_len, uint8_t want_type)
{
	return len == want_len && (frame[1] & REPORT_TYPE_MASK) == want_type;
}

static bool frame_is(const uint8_t *frame, size_t len, size_t want_len, uint8_t want_type)
{
	return shaped(frame, len, want_len, want_type) && unifying_checksum_ok(frame, len);
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
	bool held = l->keyboard_down || l->media_usage != 0 || l->divert_usage != 0;

	if (!unifying_is_connect_frame(frame, len, device_octet)) {
		return false;
	}
	l->synced = false;
	l->next_counter = 0;
	l->has_candidate = false;
	l->keyboard_down = false;
	l->media_usage = 0;
	l->divert_usage = 0;
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

/* The usage that changed wins; a released one gives way to the other source's held usage. */
static struct report consumer_report(uint16_t changed, uint16_t other)
{
	return (struct report){.kind = REPORT_CONSUMER, .usage = changed != 0 ? changed : other};
}

bool unifying_media_report(struct unifying_link *l, const uint8_t *frame, size_t len,
			   struct report *out)
{
	if (!frame_is(frame, len, MEDIA_FRAME_LEN, REPORT_MEDIA)) {
		return false;
	}
	l->media_usage = frame[2] | frame[3] << 8;
	*out = consumer_report(l->media_usage, l->divert_usage);
	return true;
}

/* Logitech's control IDs mean the same key on every device; one a keyboard lacks is refused. */
static const struct {
	uint16_t cid;
	uint16_t usage;
} diverted[] = {
	{0x00c7, UNIFYING_BRIGHTNESS_DOWN},
	{0x00c8, UNIFYING_BRIGHTNESS_UP},
};

#define DIVERTED_COUNT (sizeof(diverted) / sizeof(diverted[0]))

static bool is_hidpp(const uint8_t *frame, size_t len)
{
	return shaped(frame, len, HIDPP_SHORT_LEN, REPORT_HIDPP_SHORT) ||
	       shaped(frame, len, UNIFYING_FRAME_MAX, REPORT_HIDPP_LONG);
}

static uint16_t diverted_usage(uint16_t cid)
{
	for (size_t i = 0; i < DIVERTED_COUNT; i++) {
		if (cid != 0 && diverted[i].cid == cid) {
			return diverted[i].usage;
		}
	}
	return 0;
}

static void hidpp_step(struct unifying_hidpp *x, enum unifying_hidpp_step step)
{
	x->step = step;
	x->tries = 0;
	x->awaiting = false;
}

/* A run takes the steps in their enum order and skips each one this keyboard does not need. */
static bool wanted(const struct unifying_hidpp *x, enum unifying_hidpp_step step)
{
	switch (step) {
	case UNIFYING_HIDPP_FIND_CONTROLS:
		return !x->controls_known;
	case UNIFYING_HIDPP_DIVERT:
		return x->controls != 0;
	case UNIFYING_HIDPP_FIND_BATTERY:
		return !x->battery_known;
	case UNIFYING_HIDPP_READ_BATTERY:
		return x->battery_found;
	default:
		return false;
	}
}

static void hidpp_advance(struct unifying_hidpp *x)
{
	enum unifying_hidpp_step next = x->step + 1;

	while (next <= UNIFYING_HIDPP_READ_BATTERY && !wanted(x, next)) {
		next++;
	}
	x->control_index = 0;
	hidpp_step(x, next <= UNIFYING_HIDPP_READ_BATTERY ? next : UNIFYING_HIDPP_IDLE);
}

static void divert_end(struct unifying_hidpp *x, enum history_divert outcome, uint16_t cid)
{
	x->end.ended = true;
	x->end.outcome = outcome;
	x->end.cid = cid;
}

/* One connect brings several frames that each call this; a run on its first request is kept. */
static void hidpp_start(struct unifying_hidpp *x)
{
	if (x->step == UNIFYING_HIDPP_FIND_CONTROLS ||
	    (x->step == UNIFYING_HIDPP_DIVERT && x->control_index == 0)) {
		return;
	}
	x->end = (struct unifying_divert_end){0};
	x->battery_found = false;
	x->step = UNIFYING_HIDPP_IDLE;
	if (x->controls_known && x->controls == 0) {
		divert_end(x, HISTORY_DIVERT_NO_FEATURE, 0);
	}
	hidpp_advance(x);
}

static void hidpp_next(struct unifying_hidpp *x)
{
	if (x->step == UNIFYING_HIDPP_DIVERT && (size_t)x->control_index + 1 < DIVERTED_COUNT) {
		x->control_index++;
		hidpp_step(x, UNIFYING_HIDPP_DIVERT);
		return;
	}
	if (x->step == UNIFYING_HIDPP_DIVERT) {
		divert_end(x, HISTORY_DIVERT_FINISHED, 0);
	} else if (x->step == UNIFYING_HIDPP_FIND_CONTROLS && x->controls == 0) {
		divert_end(x, HISTORY_DIVERT_NO_FEATURE, 0);
	}
	hidpp_advance(x);
}

static void hidpp_give_up(struct unifying_hidpp *x)
{
	if (x->step == UNIFYING_HIDPP_FIND_CONTROLS) {
		divert_end(x, HISTORY_DIVERT_UNANSWERED, 0);
	} else if (x->step == UNIFYING_HIDPP_DIVERT) {
		divert_end(x, HISTORY_DIVERT_UNANSWERED, diverted[x->control_index].cid);
	}
	hidpp_advance(x);
}

static size_t get_feature(uint8_t out[UNIFYING_FRAME_MAX], uint16_t feature)
{
	out[1] = REPORT_HIDPP_SHORT;
	out[3] = HIDPP_ROOT;
	out[4] = HIDPP_GET_FEATURE;
	out[5] = (uint8_t)(feature >> 8);
	out[6] = (uint8_t)feature;
	return HIDPP_SHORT_LEN;
}

static size_t set_cid_reporting(uint8_t out[UNIFYING_FRAME_MAX], uint8_t feature, uint16_t cid)
{
	out[1] = REPORT_HIDPP_LONG;
	out[3] = feature;
	out[4] = HIDPP_SET_CID_REPORTING;
	out[5] = (uint8_t)(cid >> 8);
	out[6] = (uint8_t)cid;
	out[7] = HIDPP_DIVERT;
	return UNIFYING_FRAME_MAX;
}

static size_t get_battery(uint8_t out[UNIFYING_FRAME_MAX], uint8_t feature)
{
	out[1] = REPORT_HIDPP_SHORT;
	out[3] = feature;
	out[4] = HIDPP_GET_BATTERY;
	return HIDPP_SHORT_LEN;
}

/* Call it only while no other frame of the dongle's waits in the queue. */
static size_t hidpp_request(struct unifying_hidpp *x, uint8_t device_index, uint64_t now_ms,
			    uint32_t retry_ms, uint8_t out[UNIFYING_FRAME_MAX])
{
	if (x->step == UNIFYING_HIDPP_IDLE || (x->awaiting && now_ms - x->taken_ms < retry_ms)) {
		return 0;
	}
	if (x->tries >= UNIFYING_HIDPP_TRIES) {
		hidpp_give_up(x);
		return 0;
	}
	memset(out, 0, UNIFYING_FRAME_MAX);
	out[2] = device_index;
	switch (x->step) {
	case UNIFYING_HIDPP_FIND_CONTROLS:
		return seal(out, get_feature(out, HIDPP_REPROG_CONTROLS));
	case UNIFYING_HIDPP_DIVERT:
		return seal(out, set_cid_reporting(out, x->controls, diverted[x->control_index].cid));
	case UNIFYING_HIDPP_FIND_BATTERY:
		return seal(out, get_feature(out, HIDPP_BATTERY_STATUS));
	case UNIFYING_HIDPP_READ_BATTERY:
		return seal(out, get_battery(out, x->battery));
	default:
		return 0;
	}
}

/* Bytes 5 to 7 of a battery reply or event: level, next level, status; a level of 0 is unknown. */
static void read_battery(struct device_battery *b, const uint8_t *frame)
{
	uint8_t status = frame[7];

	b->level = frame[5] != 0 ? frame[5] : DEVICE_NO_BATTERY;
	if (status == 0) {
		b->charging = DEVICE_DISCHARGING;
	} else if (status == 3) {
		b->charging = DEVICE_CHARGED;
	} else if (status == 1 || status == 2 || status == 4) {
		b->charging = DEVICE_CHARGING;
	} else if (status <= 7) {
		b->charging = DEVICE_BATTERY_FAULT;
	} else {
		b->charging = DEVICE_CHARGE_UNKNOWN;
	}
}

static void hidpp_taken(struct unifying_hidpp *x, uint64_t now_ms)
{
	if (x->step != UNIFYING_HIDPP_IDLE) {
		x->tries++;
		x->awaiting = true;
		x->taken_ms = now_ms;
	}
}

/* Returns true for a reply or refusal of the request in flight, and moves the run on. */
static bool hidpp_reply(struct unifying_hidpp *x, struct device_battery *b, const uint8_t *frame)
{
	bool error = frame[3] == HIDPP_ERROR;
	uint8_t feature = error ? frame[4] : frame[3];
	uint8_t function = error ? frame[5] : frame[4];

	bool find = feature == HIDPP_ROOT && function == HIDPP_GET_FEATURE;

	if (x->step == UNIFYING_HIDPP_FIND_CONTROLS && find) {
		x->controls = error ? 0 : frame[5];
		x->controls_known = true;
	} else if (x->step == UNIFYING_HIDPP_FIND_BATTERY && find) {
		x->battery = error ? 0 : frame[5];
		x->battery_known = true;
		x->battery_found = x->battery != 0;
	} else if (x->step == UNIFYING_HIDPP_READ_BATTERY && feature == x->battery &&
		   function == HIDPP_GET_BATTERY) {
		if (!error) {
			read_battery(b, frame);
		}
	} else if (x->step == UNIFYING_HIDPP_DIVERT && feature == x->controls &&
		   function == HIDPP_SET_CID_REPORTING &&
		   (error || (frame[5] << 8 | frame[6]) == diverted[x->control_index].cid)) {
		if (error) {
			x->end.refused++;
		} else {
			x->end.diverted++;
		}
	} else {
		return false;
	}
	hidpp_next(x);
	return true;
}

/* Takes only a frame whose checksum the caller checked. */
static bool hidpp_rx(struct unifying_hidpp *x, struct unifying_link *l, struct device_battery *b,
		     const uint8_t *frame, size_t len, struct report *out)
{
	uint16_t usage = 0;

	if (!is_hidpp(frame, len) || hidpp_reply(x, b, frame)) {
		return false;
	}
	if (x->battery != 0 && frame[3] == x->battery && frame[4] == HIDPP_BATTERY_EVENT) {
		read_battery(b, frame);
		return false;
	}
	if (x->controls == 0 || frame[3] != x->controls || frame[4] != HIDPP_DIVERTED_EVENT ||
	    len != UNIFYING_FRAME_MAX) {
		return false;
	}
	/* The event lists up to four held controls. */
	for (size_t i = 5; i < 13 && usage == 0; i += 2) {
		usage = diverted_usage((uint16_t)(frame[i] << 8 | frame[i + 1]));
	}
	if (usage == l->divert_usage) {
		return false;
	}
	l->divert_usage = usage;
	*out = consumer_report(usage, l->media_usage);
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

bool unifying_led_due(const struct unifying_led *led, bool has_device, uint8_t *leds)
{
	if (!has_device || !(led->resend || led->wanted != led->sent)) {
		return false;
	}
	*leds = led->wanted;
	return true;
}

void unifying_led_sent(struct unifying_led *led, uint8_t leds)
{
	led->sent = leds;
	led->has_sent = true;
	led->resend = false;
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
	bool silent = (l->keyboard_down || l->media_usage != 0 || l->divert_usage != 0) &&
		      now_ms - l->last_ms > 2 * (uint64_t)l->keepalive_ms;

	if (silent) {
		l->keyboard_down = false;
		l->media_usage = 0;
		l->divert_usage = 0;
	}
	return silent;
}

void unifying_record_of(const struct unifying_pairing *p, uint8_t out[UNIFYING_RECORD_LEN])
{
	memcpy(out, &p->device, UNIFYING_RECORD_LEN);
}

void unifying_record_restore(struct unifying_pairing *p, const uint8_t in[UNIFYING_RECORD_LEN])
{
	struct unifying_device d;

	memcpy(&d, in, UNIFYING_RECORD_LEN);
	unifying_pairing_restore(p, &d);
}

static bool quiet_too_long(const struct unifying_session *s, uint64_t now_ms)
{
	uint64_t since = s->heard_ms > s->deaf_until_ms ? s->heard_ms : s->deaf_until_ms;

	if (s->search_until_ms > since) {
		since = s->search_until_ms;
	}

	/* deaf_until_ms and search_until_ms can lie after now_ms. */
	if (now_ms <= since) {
		return false;
	}
	return now_ms - since >= 2 * (uint64_t)s->link.keepalive_ms;
}

static void take_divert_end(struct unifying_session *s, struct unifying_out *out)
{
	if (s->hidpp.end.ended) {
		out->divert = s->hidpp.end;
		s->hidpp.end.ended = false;
	}
}

void unifying_session_init(struct unifying_session *s, const struct unifying_receiver *r,
			   const uint8_t *record, uint64_t now_ms)
{
	memset(s, 0, sizeof(*s));
	s->receiver = *r;
	s->started_ms = now_ms;
	s->battery = DEVICE_BATTERY_UNKNOWN;
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
		s->hidpp = (struct unifying_hidpp){0};
		hidpp_start(&s->hidpp);
		s->battery = DEVICE_BATTERY_UNKNOWN;
	}
	if (pipe == UNIFYING_PIPE_DEVICE) {
		bool checksum_ok = unifying_checksum_ok(frame, len);
		uint32_t refused = s->link.refused;
		bool attack = now_ms < s->link.attack_until_ms;

		if (!s->heard) {
			out->heard = UNIFYING_HEARD_FIRST;
			out->down_ms = (uint32_t)(now_ms - s->started_ms);
		} else if (s->silent) {
			out->heard = s->connect_in_silence ? UNIFYING_HEARD_WOKE : UNIFYING_HEARD_AGAIN;
			out->down_ms = (uint32_t)(now_ms - s->silent_since_ms);
		}
		/* The keyboard forgets a divert whenever it reconnects, which a silence can hide. */
		if (out->heard != UNIFYING_HEARD_NONE) {
			hidpp_start(&s->hidpp);
		}
		/* Requests 2 and 3 of a pairing attempt arrive on this pipe too. */
		if (!attempting(&s->pairing) && checksum_ok) {
			pairwin_link_up(&s->pairing.window);
		}
		s->silent = false;
		s->connect_in_silence = false;
		s->search_until_ms = 0;
		s->heard = true;
		s->heard_ms = now_ms;
		s->rssi = rssi;
		unifying_link_heard(&s->link, frame, len, now_ms);
		if (unifying_has_device(&s->pairing)) {
			out->reports = unifying_keystroke(&s->link, s->pairing.device.key, frame, len,
							  now_ms, aes, out->report);
			if (out->reports == 0 &&
			    (unifying_media_report(&s->link, frame, len, &out->report[0]) ||
			     (checksum_ok &&
			      hidpp_rx(&s->hidpp, &s->link, &s->battery, frame, len,
				       &out->report[0])))) {
				out->reports = 1;
			}
		}
		out->invalid = !checksum_ok || s->link.refused != refused;
		out->attack_started = !attack && now_ms < s->link.attack_until_ms;
	} else if (pipe == UNIFYING_PIPE_RECEIVER) {
		out->reconnected = unifying_is_connect_frame(frame, len, s->receiver.device_octet);
		if (out->reconnected) {
			hidpp_start(&s->hidpp);
		}
		s->connect_in_silence |= out->reconnected && s->silent;
		out->release_all = unifying_link_reconnect(&s->link, s->receiver.device_octet, frame,
							   len);
		unifying_led_resend(&s->led);
	}
	/* A frame can close the window early, while the tick sets deaf_until_ms to its full end. */
	if (!unifying_pairing_window_open(&s->pairing, now_ms) && s->deaf_until_ms > now_ms) {
		s->deaf_until_ms = now_ms;
	}
	take_divert_end(s, out);
}

void unifying_session_tick(struct unifying_session *s, uint64_t now_ms, bool leds_changed,
			   uint8_t host_leds, uint32_t taken_count, struct unifying_out *out)
{
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
	out->release_all = now_ms >= s->search_until_ms && unifying_link_silent(&s->link, now_ms);
	if (leds_changed) {
		unifying_led_want(&s->led, host_leds);
	}
	if (s->queue.frame != UNIFYING_QUEUED_NONE && taken_count != s->queue.taken_count) {
		if (s->queue.frame == UNIFYING_QUEUED_HIDPP) {
			hidpp_taken(&s->hidpp, now_ms);
		}
		s->queue.frame = UNIFYING_QUEUED_NONE;
	}
	if (s->queue.frame == UNIFYING_QUEUED_NONE) {
		out->send_led = unifying_led_due(&s->led, unifying_has_device(&s->pairing),
						 &out->leds);
	}
	/* The ACK payloads of a pairing attempt share the queue. */
	if (s->queue.frame == UNIFYING_QUEUED_NONE && !out->send_led &&
	    unifying_has_device(&s->pairing) && !attempting(&s->pairing)) {
		out->hidpp_len = hidpp_request(&s->hidpp, s->receiver.device_octet,
							 now_ms, 2U * s->link.keepalive_ms,
							 out->hidpp);
	}
	take_divert_end(s, out);
	out->attempt_expired = unifying_pairing_expired(&s->pairing, now_ms);
	if (out->attempt_expired) {
		_Static_assert(UNIFYING_ANSWERED_1 == 1 && UNIFYING_ANSWERED_2 == 2,
			       "expired_step is the state");
		out->expired_step = step;
	}
	pending = step != UNIFYING_IDLE && !out->attempt_expired;
	out->window_closed = !pending && pairwin_ran_out(&s->pairing.window, (uint32_t)now_ms);
	if (open) {
		uint8_t next = unifying_sweep_channel(&s->sweep, &s->pairing, now_ms);

		if (next != s->channel) {
			out->set_channel = true;
			out->channel = next;
			s->channel = next;
		}
	}
}

void unifying_session_led_queued(struct unifying_session *s, uint8_t leds, uint32_t taken_count)
{
	unifying_led_sent(&s->led, leds);
	s->queue = (struct unifying_queue){.frame = UNIFYING_QUEUED_LED, .taken_count = taken_count};
}

void unifying_session_hidpp_queued(struct unifying_session *s, uint32_t taken_count)
{
	s->queue = (struct unifying_queue){.frame = UNIFYING_QUEUED_HIDPP, .taken_count = taken_count};
}

void unifying_session_replies_dropped(struct unifying_session *s)
{
	if (s->queue.frame == UNIFYING_QUEUED_LED) {
		unifying_led_resend(&s->led);
	}
	s->queue.frame = UNIFYING_QUEUED_NONE;
}

void unifying_session_open_pairing(struct unifying_session *s, enum pairwin_cause cause,
				    uint64_t now_ms)
{
	unifying_pairing_open(&s->pairing, cause, now_ms);
}

enum unifying_status unifying_session_status(const struct unifying_session *s, uint64_t now_ms)
{
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

int unifying_session_home(const struct unifying_session *s, uint64_t now_ms)
{
	bool home = unifying_has_device(&s->pairing) &&
		    !unifying_pairing_window_open(&s->pairing, now_ms);

	return home ? s->channel : CHANMOVE_NO_HOME;
}

int unifying_session_movable(const struct unifying_session *s, uint64_t now_ms)
{
	return attempting(&s->pairing) ? CHANMOVE_NO_HOME : unifying_session_home(s, now_ms);
}

size_t unifying_session_channels(const uint8_t **out)
{
	*out = unifying_pairing_channels;
	return UNIFYING_PAIRING_CHANNELS;
}

enum chanmove_result unifying_session_move(struct unifying_session *s, uint8_t channel,
					   uint64_t now_ms)
{
	const uint8_t *channels;
	size_t n = unifying_session_channels(&channels);

	if (memchr(channels, channel, n) == NULL) {
		return CHANMOVE_NOT_A_CHANNEL;
	}
	if (unifying_session_movable(s, now_ms) == CHANMOVE_NO_HOME) {
		return CHANMOVE_NOT_MOVABLE;
	}
	if (channel != s->channel) {
		s->channel = channel;
		s->search_until_ms = now_ms + UNIFYING_MOVE_SEARCH_MS;
	}
	return CHANMOVE_DONE;
}
