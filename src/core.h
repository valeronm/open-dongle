#ifndef CORE_H
#define CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include "chanmove.h"
#include "chanpick.h"
#include "chanwatch.h"
#include "diagproto.h"
#include "history.h"
#include "indicator.h"
#include "pairwin.h"
#include "protocol.h"
#include "radio.h"
#include "reports.h"

#define CORE_NO_HOME (-1)

enum core_counter {
	CORE_PACKETS,
	CORE_INVALID,
	CORE_CHANNEL_CHANGES,
	CORE_LINK_LOSSES,
	CORE_GAPS,
	CORE_SURVEYS,
	CORE_COUNTERS,
};

/* What the core asks of the running protocol loop. */
struct core_loop {
	/* The channel whose noise counts toward the minute, or CORE_NO_HOME. */
	int (*home)(void);
	/* Runs after each survey that answers a noise request, which keeps the receiver off. */
	void (*radio_resumed)(void);
	void (*fill_status)(struct diag_status *st);
	/* Channels the dongle may move to for less noise; empty for a protocol that does not
	 * move. */
	const uint8_t *candidates;
	size_t candidates_len;
	/* The home channel while a noise move is allowed, or CORE_NO_HOME; required with
	 * candidates. */
	int (*movable)(void);
	/* Runs after each scan round or move, which drops the replies queued on the
	 * radio; required with candidates. */
	void (*replies_dropped)(void);
	/* Moves the protocol state for a requested or noise move; *from is the channel before it. */
	enum chanmove_result (*move)(uint8_t channel, uint8_t *from);
};

struct rx_item {
	uint8_t len;
	uint8_t pipe;
	int8_t rssi;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

extern const uint8_t core_firmware_version[3];

/* Starts USB, the report senders and the watchdog for the protocol this boot runs. */
void core_start(enum protocol active, uint32_t reset_cause);
uint32_t core_reset_cause(void);
void core_on_radio_rx(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi);
/* Blocks until the radio starts, retrying each second. */
void core_start_radio(const struct radio_config *rc);
bool core_take_rx(struct rx_item *out, k_timeout_t wait);
uint32_t core_rx_dropped(void);
/* Returns true when the report changed what the host sees; a change wakes a suspended host. */
bool core_submit(const struct report *r);
void core_release_keys(void);
uint32_t core_usb_retries(void);
uint32_t core_merges(void);
/* Returns true once after each host LED change; *leds always receives the current LEDs. */
bool core_take_leds(uint8_t *leds);
void core_watchdog_feed(void);
/* A suspended host shows IND_SLEEP whatever the mode. */
void core_show_state(enum indicator_mode mode);
/* Starts the history with a boot event; loop must stay valid for the rest of the program. */
void core_start_history(const struct core_loop *loop);
void core_count(enum core_counter c);
/* Takes the pending noise readings first, which a gap or loss event records. */
void core_log(uint8_t type, uint8_t a, uint8_t b, uint8_t c, uint32_t value);
void core_log_pairing_opened(const struct pairwin *w);
/* Takes the pending noise readings first. */
void core_rssi(int8_t dbm);
/* UINT8_MAX when no report has changed what the host sees since boot, or 255 minutes or more ago. */
uint8_t core_minutes_since_key(void);
/* Logs USB suspend changes and closes the minutes that ended. */
void core_poll(void);
/* Takes the pending noise readings, which a channel change drops, and counts a change that
 * succeeds. */
int core_set_channel(uint8_t channel);
/* Takes the pending noise readings, which a survey drops. Blocks for about 1.5 s with the receiver
 * off; returns NULL when the radio failed. */
const struct chan_survey *core_survey(void);
/* Sets the fields every protocol reports and zeroes the rest; loop.fill_status adds the
 * protocol's own. */
void core_fill_status(struct diag_status *st);
/* Answers status, protocol, reboot, bootloader, history, events, clear and noise requests, and
 * requests for the other protocol's commands, itself; returns true with the next request it does
 * not answer. */
bool core_take_request(struct diag_request *out);

FUNC_NORETURN void nuphy_run(int store_err);
FUNC_NORETURN void unifying_run(int store_err);

#endif
