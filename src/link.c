#include <string.h>
#include "link.h"

#define IDLE_AFTER_MS 1000
#define HOME_SLOT_MS 400
#define PAIRING_SLOT_MS 100
#define STATUS_EVERY_MS 100
/* The keyboard repeats a pair request that goes unanswered while a survey has the receiver off. */
#define OFFER_REUSE_MS 10000

static const uint8_t pairing_channels[] = {2, 50, 80};
/* The keyboard firmware fixes these channels besides the three it gets at pairing. */
static const uint8_t fixed_channels[] = {16, 22, 44, 48, 58, 72, 78};

#define IDLE_SLOTS (1 + sizeof(pairing_channels))

static void set_channel(struct link *l, uint8_t channel, struct link_out *out)
{
	l->channel = channel;
	out->set_channel = true;
	out->channel = channel;
}

static uint8_t slot_channel(const struct link *l)
{
	if (l->slot > 0) {
		return pairing_channels[l->slot - 1];
	}
	return l->own_known ? l->own[0] : fixed_channels[l->fixed_next];
}

static void go_idle(struct link *l, uint32_t now_ms, struct link_out *out)
{
	l->state = LINK_IDLE;
	l->suspend_sent = false;
	l->slot = 0;
	l->slot_start_ms = now_ms;
	set_channel(l, slot_channel(l), out);
}

static void reply(struct link *l, uint8_t pipe, struct link_out *out)
{
	l->seq = proto_next_seq(l->seq);
	out->reply = true;
	out->reply_pipe = pipe;
}

static void reply_status(struct link *l, uint32_t now_ms, struct link_out *out)
{
	reply(l, 1, out);
	out->reply_len = proto_status(out->reply_buf, l->seq, l->dongle_id, l->host_leds);
	l->last_status_ms = now_ms;
}

static void reply_pairing(struct link *l, struct link_out *out)
{
	reply(l, 0, out);
	out->reply_len = proto_pair_reply(out->reply_buf, l->seq, l->dongle_id, l->keyboard_id,
					  l->offered);
}

void link_init(struct link *l, const uint8_t dongle_id[PROTO_ID_LEN], const uint8_t *own,
	       uint32_t now_ms, struct link_out *out)
{
	memset(l, 0, sizeof(*l));
	memcpy(l->dongle_id, dongle_id, PROTO_ID_LEN);
	if (own != NULL) {
		memcpy(l->own, own, sizeof(l->own));
		l->own_known = true;
	}
	l->pairing_allowed = true;
	go_idle(l, now_ms, out);
}

static void on_pairing_message(struct link *l, const struct proto_msg *m, uint32_t now_ms,
			       struct link_out *out)
{
	bool same_keyboard = memcmp(m->ids.keyboard, l->keyboard_id, PROTO_ID_LEN) == 0;
	bool pairing = l->state == LINK_SURVEY || l->state == LINK_PAIRING ||
		       l->state == LINK_CONFIRMED;

	if (pairing && same_keyboard) {
		l->last_rx_ms = now_ms;
	}
	if (m->kind == PROTO_PAIR_REQUEST) {
		if (l->state == LINK_IDLE && l->pairing_allowed) {
			memcpy(l->keyboard_id, m->ids.keyboard, PROTO_ID_LEN);
			l->last_rx_ms = now_ms;
			if (l->offer_valid && now_ms - l->offered_ms < OFFER_REUSE_MS) {
				l->state = LINK_PAIRING;
				reply_pairing(l, out);
			} else {
				l->state = LINK_SURVEY;
				out->start_survey = true;
			}
		} else if (l->state == LINK_PAIRING && same_keyboard) {
			reply_pairing(l, out);
		}
		return;
	}
	if (m->kind == PROTO_PAIR_CONFIRM && l->state == LINK_PAIRING && same_keyboard &&
	    memcmp(m->ids.dongle, l->dongle_id, PROTO_ID_LEN) == 0) {
		memcpy(l->own, l->offered, sizeof(l->own));
		l->own_known = true;
		out->save_channels = true;
		l->state = LINK_CONFIRMED;
		/* The keyboard addresses pipe 1 once this confirm is acknowledged. */
		reply_status(l, now_ms, out);
	}
}

static void on_connected_message(struct link *l, uint32_t now_ms, struct link_out *out)
{
	if (l->host_suspended && !l->suspend_sent) {
		reply(l, 1, out);
		out->reply_len = proto_suspend(out->reply_buf, l->seq);
		l->suspend_sent = true;
		l->last_status_ms = now_ms;
	} else if (now_ms - l->last_status_ms >= STATUS_EVERY_MS) {
		reply_status(l, now_ms, out);
	}
}

void link_on_message(struct link *l, const struct proto_msg *m, uint8_t pipe, uint32_t now_ms,
		     struct link_out *out)
{
	if (m->kind == PROTO_INVALID) {
		return;
	}
	if (pipe == 0) {
		on_pairing_message(l, m, now_ms, out);
		return;
	}
	if (pipe != 1) {
		return;
	}
	l->last_rx_ms = now_ms;
	switch (l->state) {
	case LINK_CONFIRMED:
	case LINK_MOVING_HOME:
		/* This packet's ACK carried the status queued on entering this state. */
		if (l->state == LINK_MOVING_HOME) {
			out->moved = true;
			out->moved_from = l->channel;
		}
		l->state = LINK_CONNECTED;
		set_channel(l, l->own[0], out);
		reply_status(l, now_ms, out);
		break;
	case LINK_IDLE:
		/* A keyboard heard on a pairing channel stays there until a send fails. */
		l->state = (l->slot > 0 && l->own_known) ? LINK_MOVING_HOME : LINK_CONNECTED;
		out->up = true;
		reply_status(l, now_ms, out);
		break;
	case LINK_CONNECTED:
		on_connected_message(l, now_ms, out);
		break;
	default:
		break;
	}
}

void link_on_survey(struct link *l, const uint8_t channels[3], uint32_t now_ms,
		    struct link_out *out)
{
	if (l->state != LINK_SURVEY) {
		return;
	}
	memcpy(l->offered, channels, sizeof(l->offered));
	l->offer_valid = true;
	l->offered_ms = now_ms;
	l->state = LINK_PAIRING;
	/* The survey sweeps the radio across channels with the receiver off. */
	l->last_rx_ms = now_ms;
	set_channel(l, l->channel, out);
	reply_pairing(l, out);
}

void link_survey_failed(struct link *l, uint32_t now_ms, struct link_out *out)
{
	if (l->state == LINK_SURVEY) {
		go_idle(l, now_ms, out);
	}
}

void link_tick(struct link *l, uint32_t now_ms, struct link_out *out)
{
	switch (l->state) {
	case LINK_CONNECTED:
	case LINK_CONFIRMED:
	case LINK_MOVING_HOME:
	case LINK_PAIRING:
		if (now_ms - l->last_rx_ms >= IDLE_AFTER_MS) {
			out->lost = l->state == LINK_CONNECTED || l->state == LINK_MOVING_HOME;
			go_idle(l, now_ms, out);
		}
		break;
	case LINK_IDLE:
		if (now_ms - l->slot_start_ms >= (l->slot == 0 ? HOME_SLOT_MS : PAIRING_SLOT_MS)) {
			l->slot = (l->slot + 1) % IDLE_SLOTS;
			if (l->slot == 0 && !l->own_known) {
				l->fixed_next = (l->fixed_next + 1) % sizeof(fixed_channels);
			}
			l->slot_start_ms = now_ms;
			set_channel(l, slot_channel(l), out);
		}
		break;
	case LINK_SURVEY:
		break;
	}
}

void link_radio_resumed(struct link *l, uint32_t now_ms)
{
	l->last_rx_ms = now_ms;
}

void link_set_host_leds(struct link *l, uint8_t leds)
{
	l->host_leds = leds;
}

void link_set_host_suspended(struct link *l, bool suspended)
{
	l->host_suspended = suspended;
	if (!suspended) {
		l->suspend_sent = false;
	}
}

enum link_led link_led(const struct link *l)
{
	switch (l->state) {
	case LINK_IDLE:
		return LINK_LED_IDLE;
	case LINK_CONNECTED:
	case LINK_MOVING_HOME:
		return LINK_LED_CONNECTED;
	default:
		return LINK_LED_PAIRING;
	}
}
