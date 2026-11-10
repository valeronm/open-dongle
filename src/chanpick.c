#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "chanpick.h"

#define NEIGHBORS 5

/* The RF channels of BLE advertising channels 37, 38 and 39; 2 and 80 are also the keyboard's
 * pairing channels. */
static const uint8_t excluded_channels[] = {2, 26, 80};

static bool excluded(int ch)
{
	for (size_t i = 0; i < sizeof(excluded_channels); i++) {
		if (ch == excluded_channels[i]) {
			return true;
		}
	}
	return false;
}

/* Wi-Fi transmits in bursts, which a mean would hide. */
static int8_t busy_level(const int8_t samples[CHAN_SAMPLES])
{
	int8_t sorted[CHAN_SAMPLES];

	memcpy(sorted, samples, sizeof(sorted));
	for (int i = 1; i < CHAN_SAMPLES; i++) {
		int8_t v = sorted[i];
		int j = i - 1;

		while (j >= 0 && sorted[j] > v) {
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = v;
	}
	return sorted[CHAN_SAMPLES * 9 / 10];
}

/* A lull inside a Wi-Fi band is not a quiet channel. */
static int8_t band_level(const int8_t busy[CHAN_COUNT], int i)
{
	int8_t level = INT8_MIN;

	for (int j = i - NEIGHBORS; j <= i + NEIGHBORS; j++) {
		if (j >= 0 && j < CHAN_COUNT && busy[j] > level) {
			level = busy[j];
		}
	}
	return level;
}

void chan_pick(const struct chan_survey *s, uint8_t out[3])
{
	int8_t busy[CHAN_COUNT];
	int8_t level[CHAN_COUNT];

	for (int i = 0; i < CHAN_COUNT; i++) {
		busy[i] = busy_level(s->dbm[i]);
	}
	for (int i = 0; i < CHAN_COUNT; i++) {
		level[i] = band_level(busy, i);
	}
	for (int pick = 0; pick < 3; pick++) {
		int best = -1;

		for (int i = 0; i < CHAN_COUNT; i++) {
			int ch = CHAN_FIRST + i;
			bool near = false;

			for (int p = 0; p < pick; p++) {
				near |= abs(ch - out[p]) < CHAN_MIN_SPACING;
			}
			if (!near && !excluded(ch) && (best < 0 || level[i] < level[best])) {
				best = i;
			}
		}
		out[pick] = (uint8_t)(CHAN_FIRST + best);
	}
}
