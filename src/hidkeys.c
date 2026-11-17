#include <string.h>

#include "hidkeys.h"

#define MODIFIER_FIRST 0xe0
#define MODIFIER_LAST 0xe7
#define KEY_FIRST 0x04
#define KEY_ROLLOVER 0x01
#define KEY_SLOTS 6
#define NO_USAGE (-1)

void hidkeys_reset(struct hidkeys *k)
{
	*k = (struct hidkeys){0};
}

static uint32_t bits_at(const uint8_t *data, uint32_t offset, uint8_t size)
{
	uint32_t v = 0;

	for (uint8_t i = 0; i < size; i++) {
		uint32_t bit = offset + i;

		if (data[bit / 8] & (1u << (bit % 8))) {
			v |= 1u << i;
		}
	}
	return v;
}

static bool fits(const struct hidmap_field *f, size_t len)
{
	return (size_t)f->bit_offset + (size_t)f->size * f->count <= len * 8;
}

static uint8_t *source_for(struct hidkeys *k, uint8_t report_id)
{
	for (uint8_t i = 0; i < k->count; i++) {
		if (k->sources[i].report_id == report_id) {
			return k->sources[i].down;
		}
	}
	if (k->count == HIDKEYS_SOURCES) {
		return NULL;
	}
	k->sources[k->count].report_id = report_id;
	return k->sources[k->count++].down;
}

/* The usage the field's value i reports as down, or NO_USAGE. */
static int32_t usage_at(const struct hidmap_field *f, const uint8_t *data, uint8_t i)
{
	uint32_t v = bits_at(data, f->bit_offset + (uint32_t)i * f->size, f->size);

	if (f->shape == HIDMAP_BITS) {
		return v ? (int32_t)f->usage_min + i : NO_USAGE;
	}
	if (v < f->logical_min || v > f->logical_max) {
		return NO_USAGE;
	}
	return (int32_t)(f->usage_min + v - f->logical_min);
}

static void keys_down(const struct hidmap_field *f, const uint8_t *data, uint8_t *down)
{
	for (uint8_t i = 0; i < f->count; i++) {
		int32_t usage = usage_at(f, data, i);

		if (usage >= KEY_FIRST && usage <= UINT8_MAX) {
			down[usage / 8] |= 1u << (usage % 8);
		}
	}
}

static uint16_t first_usage(const struct hidmap_field *f, const uint8_t *data)
{
	for (uint8_t i = 0; i < f->count; i++) {
		int32_t usage = usage_at(f, data, i);

		if (usage > 0 && usage <= UINT16_MAX) {
			return (uint16_t)usage;
		}
	}
	return 0;
}

static void keyboard_report(const struct hidkeys *k, struct report *out)
{
	uint8_t slots = 0;

	*out = (struct report){.kind = REPORT_KEYBOARD};
	for (uint32_t byte = 0; byte < sizeof(k->sources[0].down); byte++) {
		uint8_t down = 0;

		for (uint8_t i = 0; i < k->count; i++) {
			down |= k->sources[i].down[byte];
		}
		for (uint32_t usage = byte * 8; down; usage++, down >>= 1) {
			if (!(down & 1)) {
				continue;
			}
			if (usage >= MODIFIER_FIRST && usage <= MODIFIER_LAST) {
				out->keyboard[0] |= 1u << (usage - MODIFIER_FIRST);
			} else if (slots < KEY_SLOTS) {
				out->keyboard[2 + slots++] = (uint8_t)usage;
			} else {
				memset(&out->keyboard[2], KEY_ROLLOVER, KEY_SLOTS);
			}
		}
	}
}

static uint16_t within(uint16_t usage, uint16_t min, uint16_t max)
{
	return usage < min || usage > max ? 0 : usage;
}

size_t hidkeys_translate(struct hidkeys *k, const struct hidmap *m, uint8_t report_id,
			 const uint8_t *data, size_t len, struct report out[REPORT_KINDS])
{
	bool carries[REPORT_KINDS] = {false};
	uint16_t usage[REPORT_KINDS] = {0};
	uint8_t *down = NULL;
	size_t n = 0;

	for (uint8_t i = 0; i < m->count; i++) {
		if (m->fields[i].report_id == report_id && !fits(&m->fields[i], len)) {
			return 0;
		}
	}
	for (uint8_t i = 0; i < m->count; i++) {
		const struct hidmap_field *f = &m->fields[i];
		enum report_kind kind = f->target == HIDMAP_KEYBOARD   ? REPORT_KEYBOARD
					: f->target == HIDMAP_CONSUMER ? REPORT_CONSUMER
								       : REPORT_SYSTEM;

		if (f->report_id != report_id) {
			continue;
		}
		if (kind != REPORT_KEYBOARD) {
			if (usage[kind] == 0) {
				usage[kind] = first_usage(f, data);
			}
		} else {
			if (!down) {
				down = source_for(k, report_id);
				if (!down) {
					return 0;
				}
				memset(down, 0, sizeof(k->sources[0].down));
			}
			keys_down(f, data, down);
		}
		carries[kind] = true;
	}
	if (carries[REPORT_KEYBOARD]) {
		keyboard_report(k, &out[n++]);
	}
	if (carries[REPORT_CONSUMER]) {
		out[n++] = (struct report){
			.kind = REPORT_CONSUMER,
			.usage = within(usage[REPORT_CONSUMER], 1, REPORT_CONSUMER_MAX),
		};
	}
	if (carries[REPORT_SYSTEM]) {
		out[n++] = (struct report){
			.kind = REPORT_SYSTEM,
			.usage = within(usage[REPORT_SYSTEM], REPORT_SYSTEM_MIN, REPORT_SYSTEM_MAX),
		};
	}
	return n;
}
