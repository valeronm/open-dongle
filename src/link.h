#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "chanmove.h"
#include "nuphy.h"
#include "pairwin.h"

#define LINK_FIXED_CHANNELS 7
/* The keyboard firmware fixes these channels besides the three it gets at pairing. */
extern const uint8_t link_fixed_channels[LINK_FIXED_CHANNELS];

enum link_state {
	LINK_IDLE,
	LINK_SURVEY,
	LINK_PAIRING,
	LINK_CONFIRMED,
	LINK_CONNECTED,
	LINK_MOVING_HOME,
};

enum link_led {
	LINK_LED_IDLE,
	LINK_LED_PAIRING,
	LINK_LED_CONNECTED,
};

/* What the caller must do after a link call; zero it before each call. */
struct link_out {
	bool set_channel;
	uint8_t channel;
	bool start_survey;
	bool reply;
	uint8_t reply_pipe;
	uint8_t reply_len;
	uint8_t reply_buf[NUPHY_FRAME_MAX];
	bool save_channels;
	bool lost;
	bool up;
	bool moved;
	uint8_t moved_from;
	/* Nonzero when this packet ended a gap. */
	uint32_t gap_ms;
	uint8_t gap_channel;
	/* Pairing ran out with nothing paired, once any attempt in progress had ended. */
	bool window_closed;
	/* A pair reply went unconfirmed. */
	bool pairing_expired;
};

enum offer_kind {
	OFFER_NONE,
	OFFER_SURVEYED,
	OFFER_FORCED,
};

struct link {
	enum link_state state;
	uint8_t dongle_id[NUPHY_ID_LEN];
	uint8_t keyboard_id[NUPHY_ID_LEN];
	uint8_t own[3];
	bool own_known;
	/* The channel a paired dongle waits on: own[0] until a requested move. */
	uint8_t home;
	enum offer_kind next_offer_kind;
	uint8_t next_offer[3];
	uint32_t surveyed_ms;
	/* The forced channels can change between the pair reply and the keyboard's confirm. */
	uint8_t offered[3];
	uint8_t channel;
	bool on_pairing_channel;
	uint8_t fixed_next;
	uint32_t slot_start_ms;
	uint32_t last_rx_ms;
	uint32_t last_status_ms;
	uint8_t seq;
	uint8_t host_leds;
	bool host_suspended;
	bool suspend_sent;
	struct pairwin window;
};

/* own is the three channels from the last pairing, or NULL for a dongle that never paired. */
void link_init(struct link *l, const uint8_t dongle_id[NUPHY_ID_LEN], const uint8_t *own,
	       uint32_t now_ms, struct link_out *out);
void link_on_message(struct link *l, const struct nuphy_msg *m, uint8_t pipe, uint32_t now_ms,
		     struct link_out *out);
/* After a call sets start_survey, the caller must call link_on_survey or link_survey_failed. */
void link_on_survey(struct link *l, const uint8_t channels[3], uint32_t now_ms,
		    struct link_out *out);
void link_survey_failed(struct link *l, uint32_t now_ms, struct link_out *out);
/* Refuses a repeated channel, one outside CHAN_FIRST..CHAN_LAST or a keyboard pairing channel. */
bool link_channels_allowed(const uint8_t channels[3]);
/*
 * Every later pairing offers channels instead of surveying, until called with NULL. Returns false,
 * keeping what was forced before, for channels link_channels_allowed refuses.
 */
bool link_force_channels(struct link *l, const uint8_t *channels);
/* NULL while pairing surveys. */
const uint8_t *link_forced_channels(const struct link *l);
void link_tick(struct link *l, uint32_t now_ms, struct link_out *out);
/* The receiver was off until now_ms, so silence before it is not a sign of a lost link. */
void link_radio_resumed(struct link *l, uint32_t now_ms);
void link_set_host_leds(struct link *l, uint8_t leds);
void link_set_host_suspended(struct link *l, bool suspended);
void link_open_pairing(struct link *l, enum pairwin_cause cause, uint32_t now_ms);
/* A pair request starts a pairing only while this is true. */
bool link_pairing_open(const struct link *l, uint32_t now_ms);
enum link_led link_led(const struct link *l, uint32_t now_ms);

/* Makes channel, one of the keyboard's non-pairing channels, the home of a paired link with
 * pairing closed, connected or idle; the caller retunes the radio. */
enum chanmove_result link_move(struct link *l, uint8_t channel, uint32_t now_ms);

#endif
