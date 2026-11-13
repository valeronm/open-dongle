#ifndef RADIO_H
#define RADIO_H

#include <stddef.h>
#include <stdint.h>
#include "chanpick.h"
#include "noise.h"

#define RADIO_MAX_PAYLOAD 32

typedef void (*radio_rx_cb)(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi_dbm);

/* cb runs in the ESB interrupt, once for every payload received. */
int radio_start(const uint8_t dongle_id[4], uint8_t channel, radio_rx_cb cb);

/* Listens for Logitech Unifying on pipe 0 at pairing, pipe 1 at device, and pipe 2 at the
 * receiver's own address, which is device's base with octet 0x00. Addresses are written in the
 * order the Unifying sources print them. */
int radio_start_unifying(const uint8_t pairing[5], const uint8_t device[5], uint8_t channel,
			 radio_rx_cb cb);
int radio_set_channel(uint8_t channel);

/* Moves the kept readings into t and returns their channel. Readings start once radio_start
 * succeeds and are kept only on the channel being listened to; a channel change drops those not
 * yet taken. */
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
