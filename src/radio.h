#ifndef RADIO_H
#define RADIO_H

#include <stddef.h>
#include <stdint.h>
#include "chanpick.h"

#define RADIO_MAX_PAYLOAD 32

typedef void (*radio_rx_cb)(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi_dbm);

/* cb runs in the ESB interrupt, once for every payload received. */
int radio_start(const uint8_t dongle_id[4], uint8_t channel, radio_rx_cb cb);
int radio_set_channel(uint8_t channel);

/* Replies on a pipe go out in order, one per ACK; changing channel drops any still queued. */
int radio_reply(uint8_t pipe, const uint8_t *frame, size_t len);

/* Blocks for about 1.5 s with the receiver off. */
int radio_survey(struct chan_survey *s);

#endif
