#include <string.h>
#include "chanpick.h"
#include "link.h"

#define IDLE_AFTER_MS 1000
#define SLOT_MS 400
#define STATUS_EVERY_MS 100
/* A connected keyboard sends about every 40 ms, even with no key change. */
#define GAP_MS 250
/* The keyboard repeats a pair request that goes unanswered while a survey has the receiver off. */
#define OFFER_REUSE_MS 10000

/* The keyboard pairs on channel 2 and only rarely visits its other pairing channels, 50 and 80. */
#define PAIRING_CHANNEL 2
static const uint8_t pairing_channels[] = {PAIRING_CHANNEL, 50, 80};
/* The keyboard firmware fixes these channels besides the three it gets at pairing. */
static const uint8_t fixed_channels[] = {16, 22, 44, 48, 58, 72, 78};

static bool connected(const struct link *l)
{
	return l->state == LINK_CONNECTED || l->state == LINK_MOVING_HOME;
}

static void set_channel(struct link *l, uint8_t channel, struct link_out *out)
{
	l->channel = channel;
	out->set_channel = true;
	out->channel = channel;
}

static uint8_t slot_channel(const struct link *l)
{
	if (l->on_pairing_channel) {
		return PAIRING_CHANNEL;
	}
	return l->own_known ? l->own[0] : fixed_channels[l->fixed_next];
}

static void go_idle(struct link *l, uint32_t now_ms, struct link_out *out)
{
	l->state = LINK_IDLE;
	l->suspend_sent = false;
	l->on_pairing_channel = false;
	l->slot_start_ms = now_ms;
	set_channel(l, slot_channel(l), out);
}

static void reply(struct link *l, uint8_t pipe, struct link_out *out)
{
	l->seq = nuphy_next_seq(l->seq);
	out->reply = true;
	out->reply_pipe = pipe;
}

static void reply_status(struct link *l, uint32_t now_ms, struct link_out *out)
{
	reply(l, 1, out);
	out->reply_len = nuphy_status(out->reply_buf, l->seq, l->dongle_id, l->host_leds);
	l->last_status_ms = now_ms;
}

static void reply_pairing(struct link *l, struct link_out *out)
{
	reply(l, 0, out);
	out->reply_len = nuphy_pair_reply(out->reply_buf, l->seq, l->dongle_id, l->keyboard_id,
					  l->offered);
}

void link_init(struct link *l, const uint8_t dongle_id[NUPHY_ID_LEN], const uint8_t *own,
	       uint32_t now_ms, struct link_out *out)
{
	memset(l, 0, sizeof(*l));
	memcpy(l->dongle_id, dongle_id, NUPHY_ID_LEN);
	if (own != NULL) {
		memcpy(l->own, own, sizeof(l->own));
		l->own_known = true;
	}
	go_idle(l, now_ms, out);
}

static bool next_offer_ready(const struct link *l, uint32_t now_ms)
{
	return l->next_offer_kind == OFFER_FORCED ||
	       (l->next_offer_kind == OFFER_SURVEYED && now_ms - l->surveyed_ms < OFFER_REUSE_MS);
}

static void offer(struct link *l, struct link_out *out)
{
	memcpy(l->offered, l->next_offer, sizeof(l->offered));
	l->state = LINK_PAIRING;
	reply_pairing(l, out);
}

static void on_pairing_message(struct link *l, const struct nuphy_msg *m, uint32_t now_ms,
			       struct link_out *out)
{
	bool same_keyboard = memcmp(m->ids.keyboard, l->keyboard_id, NUPHY_ID_LEN) == 0;
	bool pairing = l->state == LINK_SURVEY || l->state == LINK_PAIRING ||
		       l->state == LINK_CONFIRMED;

	if (pairing && same_keyboard) {
		l->last_rx_ms = now_ms;
	}
	if (m->kind == NUPHY_PAIR_REQUEST) {
		if (l->state == LINK_IDLE) {
			memcpy(l->keyboard_id, m->ids.keyboard, NUPHY_ID_LEN);
			l->last_rx_ms = now_ms;
			if (next_offer_ready(l, now_ms)) {
				offer(l, out);
			} else {
				l->state = LINK_SURVEY;
				out->start_survey = true;
			}
		} else if (l->state == LINK_PAIRING && same_keyboard) {
			reply_pairing(l, out);
		}
		return;
	}
	if (m->kind == NUPHY_PAIR_CONFIRM && l->state == LINK_PAIRING && same_keyboard &&
	    memcmp(m->ids.dongle, l->dongle_id, NUPHY_ID_LEN) == 0) {
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
		out->reply_len = nuphy_suspend(out->reply_buf, l->seq);
		l->suspend_sent = true;
		l->last_status_ms = now_ms;
	} else if (now_ms - l->last_status_ms >= STATUS_EVERY_MS) {
		reply_status(l, now_ms, out);
	}
}

static void move_home(struct link *l, uint32_t now_ms, struct link_out *out)
{
	l->state = LINK_CONNECTED;
	set_channel(l, l->own[0], out);
	reply_status(l, now_ms, out);
}

void link_on_message(struct link *l, const struct nuphy_msg *m, uint8_t pipe, uint32_t now_ms,
		     struct link_out *out)
{
	if (m->kind == NUPHY_INVALID) {
		return;
	}
	if (pipe == 0) {
		on_pairing_message(l, m, now_ms, out);
		return;
	}
	if (pipe != 1) {
		return;
	}
	if (connected(l) && now_ms - l->last_rx_ms >= GAP_MS) {
		out->gap_ms = now_ms - l->last_rx_ms;
		out->gap_channel = l->channel;
	}
	l->last_rx_ms = now_ms;
	switch (l->state) {
	case LINK_CONFIRMED:
		/* This packet's ACK carried the status queued on entering this state. */
		move_home(l, now_ms, out);
		break;
	case LINK_MOVING_HOME:
		/* A keyboard follows a move only after taking a status, which its hello shows. */
		if (m->kind != NUPHY_HELLO) {
			on_connected_message(l, now_ms, out);
			break;
		}
		out->moved = true;
		out->moved_from = l->channel;
		move_home(l, now_ms, out);
		break;
	case LINK_IDLE:
		/* A keyboard heard on the pairing channel stays there until a send fails. */
		l->state = (l->on_pairing_channel && l->own_known) ? LINK_MOVING_HOME
								   : LINK_CONNECTED;
		out->up = true;
		out->woke = m->kind == NUPHY_HELLO;
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
	memcpy(l->next_offer, channels, sizeof(l->next_offer));
	l->next_offer_kind = OFFER_SURVEYED;
	l->surveyed_ms = now_ms;
	/* The survey sweeps the radio across channels with the receiver off. */
	l->last_rx_ms = now_ms;
	set_channel(l, l->channel, out);
	offer(l, out);
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
			out->lost = connected(l);
			go_idle(l, now_ms, out);
		}
		break;
	case LINK_IDLE:
		if (now_ms - l->slot_start_ms >= SLOT_MS) {
			l->on_pairing_channel = !l->on_pairing_channel;
			if (!l->on_pairing_channel && !l->own_known) {
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
	if (l->state == LINK_IDLE) {
		return LINK_LED_IDLE;
	}
	return connected(l) ? LINK_LED_CONNECTED : LINK_LED_PAIRING;
}

static bool is_pairing_channel(uint8_t ch)
{
	for (size_t i = 0; i < sizeof(pairing_channels); i++) {
		if (ch == pairing_channels[i]) {
			return true;
		}
	}
	return false;
}

bool link_channels_allowed(const uint8_t channels[3])
{
	for (int i = 0; i < 3; i++) {
		if (!chan_in_range(channels[i]) || is_pairing_channel(channels[i])) {
			return false;
		}
		for (int j = 0; j < i; j++) {
			if (channels[i] == channels[j]) {
				return false;
			}
		}
	}
	return true;
}

bool link_force_channels(struct link *l, const uint8_t *channels)
{
	if (channels == NULL) {
		if (l->next_offer_kind == OFFER_FORCED) {
			l->next_offer_kind = OFFER_NONE;
		}
		return true;
	}
	if (!link_channels_allowed(channels)) {
		return false;
	}
	memcpy(l->next_offer, channels, sizeof(l->next_offer));
	l->next_offer_kind = OFFER_FORCED;
	return true;
}

const uint8_t *link_forced_channels(const struct link *l)
{
	return l->next_offer_kind == OFFER_FORCED ? l->next_offer : NULL;
}
