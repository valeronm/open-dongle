#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chanpick.h"
#include "noise.h"

#define RADIO_MAX_PAYLOAD 32

typedef void (*radio_rx_cb)(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi_dbm);

enum radio_rate { RADIO_1MBPS, RADIO_2MBPS };
enum radio_crc { RADIO_CRC_8, RADIO_CRC_16 };

struct radio_config {
	enum radio_rate rate;
	enum radio_crc crc;
	/* Bytes in ESB's order, the reverse of how the Unifying sources print an address. */
	uint8_t base0[4];
	uint8_t base1[4];
	/* Pipe n listens on prefixes[n] with base0 for pipe 0 and base1 for the rest. */
	uint8_t prefixes[8];
	uint8_t pipes;
	uint8_t channel;
	/* Measures noise between packets for radio_noise_take. */
	bool noise;
};

/* cb runs in the ESB interrupt, once for every payload received. */
int radio_start(const struct radio_config *c, radio_rx_cb cb);
int radio_set_channel(uint8_t channel);

/* Moves the kept readings into t and returns their channel. Readings start once radio_start
 * succeeds with noise set and are kept only on the channel being listened to; a channel change
 * drops those not yet taken. */
uint8_t radio_noise_take(struct noise_tally *t);

/* Replies on a pipe go out in order, one per ACK; changing channel drops any still queued. */
int radio_reply(uint8_t pipe, const uint8_t *frame, size_t len);

/*
 * Changes each time the transmitter takes queued replies, which is when it sends a new packet on
 * the pipe after the ACK that carried one; replies taken together may change it once. A reply whose
 * ACK the transmitter never received counts as taken too.
 */
uint32_t radio_replies_taken(void);

/* Blocks for about 1.5 s with the receiver off, and returns with it listening on the current
 * channel again. */
int radio_survey(struct chan_survey *s);

#endif
