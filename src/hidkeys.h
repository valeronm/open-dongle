#ifndef HIDKEYS_H
#define HIDKEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hidmap.h"
#include "reports.h"

#define HIDKEYS_SOURCES 4

struct hidkeys {
	/* The keys each keyboard report ID holds down, one bit per usage. */
	struct {
		uint8_t report_id;
		uint8_t down[REPORT_KEYS_LEN];
	} sources[HIDKEYS_SOURCES];
	uint8_t count;
};

void hidkeys_reset(struct hidkeys *k);
/* Writes one report for each report kind this input report carries and returns how many; data
 * excludes the report ID. Keyboard reports past HIDKEYS_SOURCES report IDs are ignored. */
size_t hidkeys_translate(struct hidkeys *k, const struct hidmap *m, uint8_t report_id,
			 const uint8_t *data, size_t len, struct report out[REPORT_KINDS]);
/* Writes the mouse report a pointer input report carries; false for another report ID or a
 * report too short for the pointer's values. data excludes the report ID. */
bool hidkeys_pointer(const struct hidmap_pointer *p, uint8_t report_id, const uint8_t *data,
		     size_t len, struct report *out);

#endif
