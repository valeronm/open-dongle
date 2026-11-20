#ifndef NOISE_H
#define NOISE_H

#include <stdint.h>

#include "diagwire.h"

#define NOISE_SHARE_FULL 10000

/* peak is meaningful only once readings > 0. */
struct noise_tally {
	uint32_t readings;
	uint32_t busy;
	uint32_t strong;
	int8_t peak;
	uint32_t buckets[NOISE_BUCKETS];
};

void noise_add(struct noise_tally *t, int8_t dbm);
void noise_merge(struct noise_tally *into, const struct noise_tally *from);
struct noise_record noise_record(const struct noise_tally *t);


#endif
