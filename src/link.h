#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chanmove.h"
#include "diagwire.h"
#include "nuphy.h"
#include "pairwin.h"

#define LINK_OWN_CHANNELS 3
#define LINK_CHANNELS_MAX (LINK_FIXED_CHANNELS + LINK_OWN_CHANNELS)

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
	bool save_pairing;
	/* The paired keyboard sent a name other than the one in the record. */
	bool save_name;
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

/* The flash record is these bytes in this order. */
struct link_record {
	uint8_t own[LINK_OWN_CHANNELS];
	uint8_t keyboard_id[NUPHY_ID_LEN];
	/* Padded with zeros. */
	uint8_t name[NUPHY_NAME_MAX];
};

struct link {
	enum link_state state;
	uint8_t dongle_id[NUPHY_ID_LEN];
	/* The keyboard the last pairing confirmed; zero where it is unknown. */
	uint8_t keyboard_id[NUPHY_ID_LEN];
	/* The keyboard whose pair request the pairing in progress answers. */
	uint8_t requester_id[NUPHY_ID_LEN];
	/* Empty until the paired keyboard sends its name. */
	struct nuphy_name keyboard_name;
	uint8_t own[LINK_OWN_CHANNELS];
	bool own_known;
	/* The fixed channels, then each of own that is not among them. */
	uint8_t channels[LINK_CHANNELS_MAX];
	uint8_t channel_count;
	/* The channel a paired dongle waits on: own[0] until a requested move. */
	uint8_t home;
	enum offer_kind next_offer_kind;
	uint8_t next_offer[LINK_OWN_CHANNELS];
	uint32_t surveyed_ms;
	/* The forced channels can change between the pair reply and the keyboard's confirm. */
	uint8_t offered[LINK_OWN_CHANNELS];
	uint8_t channel;
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

/*
 * record is what link_record_of gave, or NULL for a dongle that never paired; channels
 * link_channels_allowed refuses count as never paired.
 */
void link_init(struct link *l, const uint8_t dongle_id[NUPHY_ID_LEN],
	       const struct link_record *record, uint32_t now_ms, struct link_out *out);
void link_record_of(const struct link *l, struct link_record *out);
/* False for a packet the dongle drops: no report in it reaches the host. */
bool link_on_message(struct link *l, const struct nuphy_msg *m, uint8_t pipe, uint32_t now_ms,
		     struct link_out *out);
/* After a call sets start_survey, the caller must call link_on_survey or link_survey_failed. */
void link_on_survey(struct link *l, const uint8_t channels[LINK_OWN_CHANNELS], uint32_t now_ms,
		    struct link_out *out);
void link_survey_failed(struct link *l, uint32_t now_ms, struct link_out *out);
/* Refuses a repeated channel, one outside CHAN_FIRST..CHAN_LAST or a keyboard pairing channel. */
bool link_channels_allowed(const uint8_t channels[LINK_OWN_CHANNELS]);
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
/* The replies queued on the radio were dropped, so the next packet takes a status. */
void link_replies_dropped(struct link *l, uint32_t now_ms);
void link_set_host_leds(struct link *l, uint8_t leds);
void link_set_host_suspended(struct link *l, bool suspended);
void link_open_pairing(struct link *l, enum pairwin_cause cause, uint32_t now_ms);
/* A pair request starts a pairing only while this is true. */
bool link_pairing_open(const struct link *l, uint32_t now_ms);
enum link_led link_led(const struct link *l, uint32_t now_ms);
bool link_connected(const struct link *l);

/* Makes channel, one of the keyboard's non-pairing channels, the home of a paired link with
 * pairing closed and the host awake, connected or idle; the caller retunes the radio. */
enum chanmove_result link_move(struct link *l, uint8_t channel, uint32_t now_ms);

/* The channel a paired dongle waits on, or CHANMOVE_NO_HOME for one that never paired. */
int link_home(const struct link *l);

/* The home channel while link_move would move it, or CHANMOVE_NO_HOME. */
int link_movable(const struct link *l, uint32_t now_ms);

/* Points *out at the keyboard's non-pairing channels, each once, and returns how many. A pairing
 * changes them. */
size_t link_channels(const struct link *l, const uint8_t **out);

#endif
