#ifndef CORE_H
#define CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include "chanmove.h"
#include "chanpick.h"
#include "chanwatch.h"
#include "diagwire.h"
#include "history.h"
#include "indicator.h"
#include "pairwin.h"
#include "protocol.h"
#include "radio.h"
#include "reports.h"

enum core_counter {
	CORE_INVALID,
	CORE_SURVEYS,
	CORE_COUNTERS,
};

/* What the core asks of the running protocol loop. */
struct core_loop {
	/* The channel the channel watch follows, or CHANMOVE_NO_HOME. */
	int (*home)(void);
	/* Takes the noise read since the last call and returns whether it counts toward the
	 * minute; *home is the home channel it was read on, or CHANMOVE_NO_HOME. */
	bool (*take_noise)(struct noise_tally *t, int *home);
	/* Runs after each survey, which keeps the receiver off and releases the host's keys. */
	void (*radio_resumed)(void);
	/* At most DEVICE_SLOTS. */
	uint8_t slot_count;
	/* Fills the kind, stored and ready of each of the slot_count slots. */
	void (*slots)(struct diag_slot *out);
	/* Fills the protocol's link-wide fields and the slot's signal, last packet and name. */
	void (*fill_status)(struct diag_status *st, uint8_t slot);
	/* The home channel while a noise move is allowed, or CHANMOVE_NO_HOME. */
	int (*movable)(void);
	/* Points *channels at the channels a noise move may go to and returns how many; a change
	 * in them drops the noise heard so far. */
	size_t (*candidates)(const uint8_t **channels);
	/* Runs after each channel change, scan round or survey, which drops the replies queued on
	 * the radio. */
	void (*replies_dropped)(void);
	/* Moves the protocol state for a requested or noise move; *from is the channel before it. */
	enum chanmove_result (*move)(uint8_t channel, uint8_t *from);
};

struct rx_item {
	uint32_t received_us;
	uint8_t len;
	uint8_t pipe;
	int8_t rssi;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

extern const uint8_t core_firmware_version[3];

/* Starts USB, the report senders and the watchdog for the protocol this boot runs. */
void core_start(enum protocol active, const struct bootinfo *boot);
uint32_t core_reset_cause(void);
void core_on_radio_rx(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi);
/* Blocks until the radio starts, retrying each second. */
void core_start_radio(const struct radio_config *rc);
bool core_take_rx(struct rx_item *out, k_timeout_t wait);
uint32_t core_rx_dropped(void);
/* Counts a received packet or event lost to a full queue. */
void core_count_dropped(void);
/* Returns true when the report changed what the host sees; a change wakes a suspended host.
 * received_us is when the packet carrying the report arrived, from clock_now_us, and NULL for a
 * report the dongle makes up. */
bool core_submit(uint8_t slot, const struct report *r, const uint32_t *received_us);
void core_release_keys(void);
uint32_t core_usb_retries(void);
/* Returns true once after each host LED change; *leds always receives the current LEDs. */
bool core_take_leds(uint8_t *leds);
void core_watchdog_feed(void);
/* A suspended host shows IND_SLEEP whatever the mode. */
void core_show_state(enum indicator_mode mode);
/* Starts the history with a boot event; loop must stay valid for the rest of the program. */
void core_start_history(const struct core_loop *loop);
void core_count(enum core_counter c);
void core_packet(uint8_t slot);
void core_gap(uint8_t slot);
void core_link_lost(uint8_t slot);
/* Takes the radio's noise readings; they count toward the minute when read on the home channel,
 * which *home then holds, else CHANMOVE_NO_HOME. */
bool core_take_radio_noise(struct noise_tally *t, int *home);
/* Takes the pending noise readings first, which a gap or loss event records. */
void core_log(uint8_t slot, struct diag_detail d);
void core_log_pairing_opened(const struct pairwin *w);
void core_log_pairing_closed(void);
/* Takes the pending noise readings first. */
void core_rssi(uint8_t slot, int8_t dbm);
/* The time since the slot's last input, a report that changed what the host sees, as an event
 * records it. */
uint16_t core_since_input_s(uint8_t slot);
/* Logs USB suspend changes and closes the minutes that ended. */
void core_poll(void);
/* Takes the pending noise readings, which a channel change drops, and counts a change that
 * succeeds. */
int core_set_channel(uint8_t channel);
/* Takes the pending noise readings, which a survey drops, and releases the host's keys. Blocks for
 * about 1.5 s with the receiver off; returns NULL when the radio failed or there is no ESB radio. */
const struct chan_survey *core_survey(void);
/* Sets the fields every protocol reports and the slot table, marks the slot's signal and last
 * packet as absent and zeroes the rest; loop.fill_status adds the protocol's own. slot must be
 * below loop.slot_count. */
void core_fill_status(struct diag_status *st, uint8_t slot);
/* Answers the requests every loop shares and refuses those the running protocol lacks; returns
 * true with the next request the loop must answer. */
bool core_take_request(struct diag_request *out);
void core_reply_devices(const struct diag_request *req, const struct diag_device *d, size_t n);

FUNC_NORETURN void nuphy_run(int store_err);
FUNC_NORETURN void unifying_run(int store_err);
FUNC_NORETURN void bluetooth_run(int store_err);

#endif
