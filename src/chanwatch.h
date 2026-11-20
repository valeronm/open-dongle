#ifndef CHANWATCH_H
#define CHANWATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "noise.h"

#define CHANWATCH_MAX 11
#define CHANWATCH_DECAY_MS 300000
#define CHANWATCH_HOLD_MS 60000
#define CHANWATCH_MIN_SAMPLES 50
/* Assumed: the nearest datasheet figure is 7.1 dB co-channel rejection for Bluetooth LE at
 * 2 Mbps. */
#define CHANWATCH_MARGIN_DB 10
#define CHANWATCH_NO_THRESHOLD 0

/* Shares are in hundredths of a percent of the readings at or above the threshold. */
struct chanwatch_move {
	uint8_t to;
	uint16_t home_share;
	uint16_t to_share;
};

/* Zeroed, it watches no channels. */
struct chanwatch {
	uint8_t channels[CHANWATCH_MAX];
	float weight[CHANWATCH_MAX][NOISE_BUCKETS];
	size_t count;
	uint64_t decayed_ms;
	uint64_t hold_until_ms;
	bool move_waiting;
	struct chanwatch_move move;
};

/* Returns true, after dropping the noise heard and any move waiting, when the channels differ from
 * the ones w holds; the hold before the next move stays. Given more than CHANWATCH_MAX channels, w
 * watches none. */
bool chanwatch_set_channels(struct chanwatch *w, const uint8_t *channels, size_t n,
			    uint64_t now_ms);

/* dbm holds one reading per channel in the order given to chanwatch_set_channels; home's is
 * skipped, since a scan of the home channel reads the keyboard's own packets as noise. */
void chanwatch_round(struct chanwatch *w, const int8_t *dbm, uint8_t home, uint64_t now_ms);

/* Adds readings taken between keyboard packets on home. */
void chanwatch_home(struct chanwatch *w, uint8_t home, const struct noise_tally *t);

/* When a channel is clearly quieter than home counting readings in threshold_bucket and louder,
 * a move to it waits in w and the hold before the next move starts. CHANWATCH_NO_THRESHOLD counts
 * every reading, so no channel is quieter. */
void chanwatch_decide(struct chanwatch *w, uint8_t home, uint8_t threshold_bucket,
		      uint64_t now_ms);

/* The first bucket counted as noise that harms a keyboard whose weakest recent packet is
 * weakest_dbm. */
uint8_t chanwatch_threshold_bucket(int8_t weakest_dbm);

/* The share of the readings at index i of w in bucket from and louder; 0 without readings. */
float chanwatch_share(const struct chanwatch *w, size_t i, uint8_t from);

/* Converts share, a fraction of the readings, to hundredths of a percent. */
uint16_t chanwatch_share_hundredths(float share);

/* Returns true when the channel at index i of w has heard enough to be moved to. */
bool chanwatch_enough(const struct chanwatch *w, size_t i);

/* Returns true once with the move that waits. */
bool chanwatch_take_move(struct chanwatch *w, struct chanwatch_move *out);

/* Starts the hold before the next move and drops a move that waits, which would undo the one just
 * made. */
void chanwatch_hold(struct chanwatch *w, uint64_t now_ms);

uint32_t chanwatch_hold_left(const struct chanwatch *w, uint64_t now_ms);

/* CHANWATCH_NO_THRESHOLD counts nothing. Returns the number of channels written. */
size_t chanwatch_report(const struct chanwatch *w, uint8_t threshold_bucket,
			struct diag_watch_channel out[CHANWATCH_MAX]);

#endif
