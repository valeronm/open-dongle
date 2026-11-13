#ifndef CHANWATCH_H
#define CHANWATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "noise.h"

#define CHANWATCH_MAX 25
#define CHANWATCH_DECAY_MS 300000
#define CHANWATCH_HOLD_MS 60000
#define CHANWATCH_MIN_SAMPLES 50
/* Assumed: the nearest datasheet figure is 7.1 dB co-channel rejection for Bluetooth LE at
 * 2 Mbps. */
#define CHANWATCH_MARGIN_DB 10

struct chanwatch {
	uint8_t channels[CHANWATCH_MAX];
	float weight[CHANWATCH_MAX][NOISE_BUCKETS];
	size_t count;
	uint64_t decayed_ms;
	uint64_t hold_until_ms;
};

/* Shares are in hundredths of a percent of the readings at or above the threshold. */
struct chanwatch_move {
	uint8_t to;
	uint16_t home_share;
	uint16_t to_share;
};

/* Takes at most CHANWATCH_MAX channels. */
void chanwatch_init(struct chanwatch *w, const uint8_t *channels, size_t n, uint64_t now_ms);

/* dbm holds one reading per channel in the order given to chanwatch_init; home's is skipped, since
 * a scan of the home channel reads the keyboard's own packets as noise. */
void chanwatch_round(struct chanwatch *w, const int8_t *dbm, uint8_t home, uint64_t now_ms);

/* Adds readings taken between keyboard packets on home. */
void chanwatch_home(struct chanwatch *w, uint8_t home, const struct noise_tally *t);

/*
 * Returns true with the channel to move to when one is clearly quieter than home at the keyboard's
 * level, weakest_dbm being its weakest recent packet; a true return starts the hold before the next
 * move.
 */
bool chanwatch_decide(struct chanwatch *w, uint8_t home, int8_t weakest_dbm, uint64_t now_ms,
		      struct chanwatch_move *out);

#endif
