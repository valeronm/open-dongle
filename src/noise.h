#ifndef NOISE_H
#define NOISE_H

#include <stdint.h>

#define NOISE_BUSY_DBM -85
#define NOISE_STRONG_DBM -60
#define NOISE_SHARE_FULL 10000

/* peak is meaningful only once readings > 0. */
struct noise_tally {
	uint32_t readings;
	uint32_t busy;
	uint32_t strong;
	int8_t peak;
};

/* busy and strong are shares of the readings in hundredths of a percent; all 0 without readings. */
struct noise_record {
	int8_t peak;
	uint16_t busy;
	uint16_t strong;
};

void noise_add(struct noise_tally *t, int8_t dbm);
void noise_merge(struct noise_tally *into, const struct noise_tally *from);
struct noise_record noise_record(const struct noise_tally *t);

#endif
