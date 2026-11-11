#include "noise.h"

void noise_add(struct noise_tally *t, int8_t dbm)
{
	if (t->readings == 0 || dbm > t->peak) {
		t->peak = dbm;
	}
	t->readings++;
	t->busy += dbm >= NOISE_BUSY_DBM;
	t->strong += dbm >= NOISE_STRONG_DBM;
}

void noise_merge(struct noise_tally *into, const struct noise_tally *from)
{
	if (from->readings == 0) {
		return;
	}
	if (into->readings == 0 || from->peak > into->peak) {
		into->peak = from->peak;
	}
	into->readings += from->readings;
	into->busy += from->busy;
	into->strong += from->strong;
}

static uint16_t share(uint32_t count, uint32_t readings)
{
	return (uint16_t)((uint64_t)count * NOISE_SHARE_FULL / readings);
}

struct noise_record noise_record(const struct noise_tally *t)
{
	if (t->readings == 0) {
		return (struct noise_record){0};
	}
	return (struct noise_record){.peak = t->peak,
				     .busy = share(t->busy, t->readings),
				     .strong = share(t->strong, t->readings)};
}
