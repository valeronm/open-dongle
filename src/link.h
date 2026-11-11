#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "nuphy.h"

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
};

struct link {
	enum link_state state;
	uint8_t dongle_id[NUPHY_ID_LEN];
	uint8_t keyboard_id[NUPHY_ID_LEN];
	uint8_t own[3];
	bool own_known;
	uint8_t offered[3];
	bool offer_valid;
	uint32_t offered_ms;
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
	bool pairing_allowed;
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
void link_tick(struct link *l, uint32_t now_ms, struct link_out *out);
/* The receiver was off until now_ms, so silence before it is not a sign of a lost link. */
void link_radio_resumed(struct link *l, uint32_t now_ms);
void link_set_host_leds(struct link *l, uint8_t leds);
void link_set_host_suspended(struct link *l, bool suspended);
enum link_led link_led(const struct link *l);

#endif
