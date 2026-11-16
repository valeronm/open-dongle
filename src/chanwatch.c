#include <string.h>
#include "chanwatch.h"

static void init(struct chanwatch *w, const uint8_t *channels, size_t n, uint64_t now_ms)
{
	memset(w, 0, sizeof(*w));
	w->count = n;
	if (w->count > 0) {
		memcpy(w->channels, channels, w->count);
	}
	w->decayed_ms = now_ms;
}

bool chanwatch_set_channels(struct chanwatch *w, const uint8_t *channels, size_t n,
			    uint64_t now_ms)
{
	uint64_t hold_until_ms;

	if (n > CHANWATCH_MAX) {
		n = 0;
	}
	if (n == w->count && (n == 0 || memcmp(w->channels, channels, n) == 0)) {
		return false;
	}
	hold_until_ms = w->hold_until_ms;
	init(w, channels, n, now_ms);
	w->hold_until_ms = hold_until_ms;
	return true;
}

static int index_of(const struct chanwatch *w, uint8_t channel)
{
	for (size_t i = 0; i < w->count; i++) {
		if (w->channels[i] == channel) {
			return (int)i;
		}
	}
	return -1;
}

/* A linear step stands in for the exponential, since rounds come far more often than the decay
 * time. */
static void decay(struct chanwatch *w, uint64_t now_ms)
{
	uint64_t dt;
	float keep;

	if (now_ms <= w->decayed_ms) {
		return;
	}
	dt = now_ms - w->decayed_ms;
	keep = dt >= CHANWATCH_DECAY_MS ? 0.0f : 1.0f - (float)dt / CHANWATCH_DECAY_MS;
	for (size_t i = 0; i < w->count; i++) {
		for (int b = 0; b < NOISE_BUCKETS; b++) {
			w->weight[i][b] *= keep;
		}
	}
	w->decayed_ms = now_ms;
}

void chanwatch_round(struct chanwatch *w, const int8_t *dbm, uint8_t home, uint64_t now_ms)
{
	decay(w, now_ms);
	for (size_t i = 0; i < w->count; i++) {
		if (w->channels[i] != home) {
			w->weight[i][noise_bucket(dbm[i])] += 1.0f;
		}
	}
}

void chanwatch_home(struct chanwatch *w, uint8_t home, const struct noise_tally *t)
{
	int i = index_of(w, home);

	if (i < 0) {
		return;
	}
	for (int b = 0; b < NOISE_BUCKETS; b++) {
		w->weight[i][b] += (float)t->buckets[b];
	}
}

static float sum_from(const float *weight, uint8_t from)
{
	float sum = 0.0f;

	for (int b = from; b < NOISE_BUCKETS; b++) {
		sum += weight[b];
	}
	return sum;
}

/* A threshold counts the whole bucket it falls in. */
uint8_t chanwatch_threshold_bucket(int8_t weakest_dbm)
{
	int t = weakest_dbm - CHANWATCH_MARGIN_DB;

	return noise_bucket((int8_t)(t < NOISE_BUSY_DBM ? NOISE_BUSY_DBM : t));
}

float chanwatch_share(const struct chanwatch *w, size_t i, uint8_t from)
{
	float n = sum_from(w->weight[i], 0);

	return n > 0.0f ? sum_from(w->weight[i], from) / n : 0.0f;
}

uint16_t chanwatch_share_hundredths(float share)
{
	return (uint16_t)(share * NOISE_SHARE_FULL + 0.5f);
}

bool chanwatch_enough(const struct chanwatch *w, size_t i)
{
	return sum_from(w->weight[i], 0) >= CHANWATCH_MIN_SAMPLES;
}

void chanwatch_hold(struct chanwatch *w, uint64_t now_ms)
{
	w->hold_until_ms = now_ms + CHANWATCH_HOLD_MS;
	w->move_waiting = false;
}

bool chanwatch_take_move(struct chanwatch *w, struct chanwatch_move *out)
{
	if (!w->move_waiting) {
		return false;
	}
	w->move_waiting = false;
	*out = w->move;
	return true;
}

void chanwatch_decide(struct chanwatch *w, uint8_t home, uint8_t threshold_bucket,
		      uint64_t now_ms)
{
	int h = index_of(w, home);
	float home_share;
	float best_n = 0.0f;
	float best_share = 0.0f;
	float limit;
	float gap;
	int best = -1;

	if (h < 0 || now_ms < w->hold_until_ms) {
		return;
	}
	home_share = chanwatch_share(w, (size_t)h, threshold_bucket);
	for (size_t i = 0; i < w->count; i++) {
		float share;

		if ((int)i == h || !chanwatch_enough(w, i)) {
			continue;
		}
		share = chanwatch_share(w, i, threshold_bucket);
		if (best < 0 || share < best_share) {
			best = (int)i;
			best_n = sum_from(w->weight[i], 0);
			best_share = share;
		}
	}
	if (best < 0) {
		return;
	}
	limit = home_share / 2.0f < home_share - 0.02f ? home_share / 2.0f : home_share - 0.02f;
	gap = limit - best_share;
	/*
	 * The candidate's Wilson upper bound at 3 standard deviations must lie below limit: the
	 * best of many candidates reads low by luck. Home's readings far outnumber a candidate's,
	 * so home's share is taken as exact.
	 */
	if (gap <= 0.0f || best_n * gap * gap <= 9.0f * limit * (1.0f - limit)) {
		return;
	}
	chanwatch_hold(w, now_ms);
	w->move = (struct chanwatch_move){
		.to = w->channels[best],
		.home_share = chanwatch_share_hundredths(home_share),
		.to_share = chanwatch_share_hundredths(best_share),
	};
	w->move_waiting = true;
}
