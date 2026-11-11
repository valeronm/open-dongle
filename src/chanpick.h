#ifndef CHANPICK_H
#define CHANPICK_H

#include <stdbool.h>
#include <stdint.h>

#define CHAN_FIRST 2
#define CHAN_LAST 80
#define CHAN_COUNT (CHAN_LAST - CHAN_FIRST + 1)
#define CHAN_SAMPLES 32
#define CHAN_MIN_SPACING 10

static inline bool chan_in_range(int ch)
{
	return ch >= CHAN_FIRST && ch <= CHAN_LAST;
}

struct chan_survey {
	int8_t dbm[CHAN_COUNT][CHAN_SAMPLES];
};

/*
 * Writes the three quietest channels at least CHAN_MIN_SPACING apart, never a BLE advertising
 * channel, quietest first.
 */
void chan_pick(const struct chan_survey *s, uint8_t out[3]);

#endif
