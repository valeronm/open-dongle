#include <string.h>
#include "chanwatch.h"

void chanwatch_init(struct chanwatch *w, const uint8_t *channels, size_t n, uint64_t now_ms)
{
	memset(w, 0, sizeof(*w));
	w->count = n < CHANWATCH_MAX ? n : CHANWATCH_MAX;
	if (w->count > 0) {
		memcpy(w->channels, channels, w->count);
	}
	w->decayed_ms = now_ms;
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
static uint8_t threshold_bucket(int8_t weakest_dbm)
{
	int t = weakest_dbm - CHANWATCH_MARGIN_DB;

	return noise_bucket((int8_t)(t < NOISE_BUSY_DBM ? NOISE_BUSY_DBM : t));
}

void chanwatch_hold(struct chanwatch *w, uint64_t now_ms)
{
	w->hold_until_ms = now_ms + CHANWATCH_HOLD_MS;
}

bool chanwatch_decide(struct chanwatch *w, uint8_t home, int8_t weakest_dbm, uint64_t now_ms,
		      struct chanwatch_move *out)
{
	int h = index_of(w, home);
	uint8_t from = threshold_bucket(weakest_dbm);
	float home_n;
	float home_share;
	float best_n = 0.0f;
	float best_harmful = 0.0f;
	float best_share;
	float limit;
	float gap;
	int best = -1;

	if (h < 0 || now_ms < w->hold_until_ms) {
		return false;
	}
	home_n = sum_from(w->weight[h], 0);
	if (home_n <= 0.0f) {
		return false;
	}
	home_share = sum_from(w->weight[h], from) / home_n;
	for (size_t i = 0; i < w->count; i++) {
		float n = sum_from(w->weight[i], 0);
		float k;

		if ((int)i == h || n < CHANWATCH_MIN_SAMPLES) {
			continue;
		}
		k = sum_from(w->weight[i], from);
		if (best < 0 || k * best_n < best_harmful * n) {
			best = (int)i;
			best_n = n;
			best_harmful = k;
		}
	}
	if (best < 0) {
		return false;
	}
	best_share = best_harmful / best_n;
	limit = home_share / 2.0f < home_share - 0.02f ? home_share / 2.0f : home_share - 0.02f;
	gap = limit - best_share;
	/*
	 * The candidate's Wilson upper bound at 3 standard deviations must lie below limit: the
	 * best of many candidates reads low by luck. Home's readings far outnumber a candidate's,
	 * so home's share is taken as exact.
	 */
	if (gap <= 0.0f || best_n * gap * gap <= 9.0f * limit * (1.0f - limit)) {
		return false;
	}
	out->to = w->channels[best];
	out->home_share = (uint16_t)(home_share * NOISE_SHARE_FULL + 0.5f);
	out->to_share = (uint16_t)(best_share * NOISE_SHARE_FULL + 0.5f);
	chanwatch_hold(w, now_ms);
	return true;
}
