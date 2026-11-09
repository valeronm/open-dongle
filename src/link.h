#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

enum link_state {
	LINK_IDLE,
	LINK_SURVEY,
	LINK_PAIRING,
	LINK_CONFIRMED,
	LINK_CONNECTED,
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
	uint8_t reply_buf[PROTO_FRAME_MAX];
	bool save_channels;
	bool lost;
};

struct link {
	enum link_state state;
	uint8_t dongle_id[PROTO_ID_LEN];
	uint8_t keyboard_id[PROTO_ID_LEN];
	uint8_t own[3];
	bool own_known;
	uint8_t offered[3];
	uint8_t channel;
	uint8_t slot;
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
void link_init(struct link *l, const uint8_t dongle_id[PROTO_ID_LEN], const uint8_t *own,
	       uint32_t now_ms, struct link_out *out);
void link_on_message(struct link *l, const struct proto_msg *m, uint8_t pipe, uint32_t now_ms,
		     struct link_out *out);
/* After a call sets start_survey, the caller must call link_on_survey or link_survey_failed. */
void link_on_survey(struct link *l, const uint8_t channels[3], uint32_t now_ms,
		    struct link_out *out);
void link_survey_failed(struct link *l, uint32_t now_ms, struct link_out *out);
void link_tick(struct link *l, uint32_t now_ms, struct link_out *out);
void link_set_host_leds(struct link *l, uint8_t leds);
void link_set_host_suspended(struct link *l, bool suspended);
enum link_led link_led(const struct link *l);

#endif
