#ifndef HIDMAP_H
#define HIDMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIDMAP_FIELDS_MAX 16

enum hidmap_target {
	HIDMAP_KEYBOARD,
	HIDMAP_CONSUMER,
	HIDMAP_SYSTEM,
};

enum hidmap_shape {
	/* count one-bit flags; flag i is usage usage_min + i. */
	HIDMAP_BITS,
	/* count values of size bits; a value v is usage usage_min + v - logical_min. */
	HIDMAP_ARRAY,
};

struct hidmap_field {
	uint8_t report_id;
	enum hidmap_target target;
	enum hidmap_shape shape;
	/* Counted from the first byte after the report ID. */
	uint16_t bit_offset;
	uint8_t size;
	uint8_t count;
	uint16_t usage_min;
	uint16_t logical_min;
	uint16_t logical_max;
};

struct hidmap {
	struct hidmap_field fields[HIDMAP_FIELDS_MAX];
	uint8_t count;
	bool has_leds;
	uint8_t led_report_id;
};

enum hidmap_result {
	HIDMAP_OK,
	HIDMAP_MALFORMED,
	HIDMAP_TOO_LARGE,
	HIDMAP_NO_KEYBOARD,
};

/* Keeps the keyboard, consumer and system input fields that give their usages as a range; a
 * field that lists its usages one by one is skipped. */
enum hidmap_result hidmap_parse(const uint8_t *map, size_t len, struct hidmap *out);

#endif
