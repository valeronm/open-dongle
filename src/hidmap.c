#include "hidmap.h"

#define STACK_DEPTH 4
#define REPORT_IDS_MAX 16

#define TYPE_MAIN 0
#define TYPE_GLOBAL 1
#define TYPE_LOCAL 2

#define MAIN_INPUT 0x8
#define MAIN_OUTPUT 0x9
#define MAIN_COLLECTION 0xa
#define MAIN_END_COLLECTION 0xc

#define GLOBAL_USAGE_PAGE 0x0
#define GLOBAL_LOGICAL_MIN 0x1
#define GLOBAL_LOGICAL_MAX 0x2
#define GLOBAL_REPORT_SIZE 0x7
#define GLOBAL_REPORT_ID 0x8
#define GLOBAL_REPORT_COUNT 0x9
#define GLOBAL_PUSH 0xa
#define GLOBAL_POP 0xb

#define LOCAL_USAGE 0x0
#define LOCAL_USAGE_MIN 0x1
#define LOCAL_USAGE_MAX 0x2

#define LONG_ITEM 0xfe
#define COLLECTION_APPLICATION 0x01
#define FLAG_CONSTANT 0x01
#define FLAG_VARIABLE 0x02

#define PAGE_GENERIC_DESKTOP 0x01
#define PAGE_KEYBOARD 0x07
#define PAGE_LED 0x08
#define PAGE_CONSUMER 0x0c
#define USAGE_KEYBOARD 0x06
#define USAGE_SYSTEM_CONTROL 0x80
#define USAGE_CONSUMER_CONTROL 0x01

struct globals {
	uint16_t page;
	int32_t logical_min;
	int32_t logical_max;
	uint32_t size;
	uint32_t count;
	uint8_t report_id;
};

struct locals {
	uint32_t usage;
	uint32_t usage_min;
	bool has_usage;
	bool has_min;
	bool has_max;
};

struct parser {
	struct globals g;
	struct globals stack[STACK_DEPTH];
	uint8_t stacked;
	struct locals l;
	uint8_t depth;
	bool in_target;
	enum hidmap_target target;
	struct {
		uint8_t id;
		uint32_t bits;
	} inputs[REPORT_IDS_MAX];
	uint8_t input_ids;
};

static uint32_t unsigned_value(const uint8_t *data, uint8_t size)
{
	uint32_t v = 0;

	for (uint8_t i = 0; i < size; i++) {
		v |= (uint32_t)data[i] << (8 * i);
	}
	return v;
}

static int32_t signed_value(const uint8_t *data, uint8_t size)
{
	uint32_t v = unsigned_value(data, size);

	if (size == 1) {
		return (int8_t)v;
	}
	if (size == 2) {
		return (int16_t)v;
	}
	return (int32_t)v;
}

/* The application collection that holds each target's fields, and the usage page of those
 * fields. */
static const struct {
	uint16_t page;
	uint16_t usage;
	uint16_t field_page;
} targets[] = {
	[HIDMAP_KEYBOARD] = {PAGE_GENERIC_DESKTOP, USAGE_KEYBOARD, PAGE_KEYBOARD},
	[HIDMAP_CONSUMER] = {PAGE_CONSUMER, USAGE_CONSUMER_CONTROL, PAGE_CONSUMER},
	[HIDMAP_SYSTEM] = {PAGE_GENERIC_DESKTOP, USAGE_SYSTEM_CONTROL, PAGE_GENERIC_DESKTOP},
};

static void enter_application(struct parser *p)
{
	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
		if (targets[i].page == p->g.page && targets[i].usage == p->l.usage) {
			p->in_target = true;
			p->target = (enum hidmap_target)i;
		}
	}
}

static uint32_t *input_bits(struct parser *p)
{
	for (uint8_t i = 0; i < p->input_ids; i++) {
		if (p->inputs[i].id == p->g.report_id) {
			return &p->inputs[i].bits;
		}
	}
	if (p->input_ids == REPORT_IDS_MAX) {
		return NULL;
	}
	p->inputs[p->input_ids].id = p->g.report_id;
	p->inputs[p->input_ids].bits = 0;
	return &p->inputs[p->input_ids++].bits;
}

static bool is_field(const struct parser *p, uint32_t flags)
{
	const struct globals *g = &p->g;
	const struct locals *l = &p->l;

	if (!p->in_target || (flags & FLAG_CONSTANT) || g->count == 0 ||
	    g->count > UINT8_MAX) {
		return false;
	}
	if (!l->has_min || !l->has_max || l->has_usage) {
		return false;
	}
	if (g->page != targets[p->target].field_page) {
		return false;
	}
	if (g->logical_min < 0 || g->logical_max > UINT16_MAX || g->logical_max < g->logical_min) {
		return false;
	}
	if (flags & FLAG_VARIABLE) {
		return g->size == 1;
	}
	return g->size == 8 || g->size == 16;
}

static enum hidmap_result on_input(struct parser *p, uint32_t flags, struct hidmap *out)
{
	uint32_t *bits = input_bits(p);
	uint64_t width = (uint64_t)p->g.size * p->g.count;

	if (!bits) {
		return HIDMAP_TOO_LARGE;
	}
	if (*bits + width > UINT16_MAX) {
		return HIDMAP_MALFORMED;
	}
	if (is_field(p, flags)) {
		struct hidmap_field *f;

		if (out->count == HIDMAP_FIELDS_MAX) {
			return HIDMAP_TOO_LARGE;
		}
		f = &out->fields[out->count++];
		*f = (struct hidmap_field){
			.report_id = p->g.report_id,
			.target = p->target,
			.shape = (flags & FLAG_VARIABLE) ? HIDMAP_BITS : HIDMAP_ARRAY,
			.bit_offset = (uint16_t)*bits,
			.size = (uint8_t)p->g.size,
			.count = (uint8_t)p->g.count,
			.usage_min = (uint16_t)p->l.usage_min,
			.logical_min = (uint16_t)p->g.logical_min,
			.logical_max = (uint16_t)p->g.logical_max,
		};
	}
	*bits += (uint32_t)width;
	return HIDMAP_OK;
}

static enum hidmap_result on_main(struct parser *p, uint8_t tag, uint32_t value,
				  struct hidmap *out)
{
	enum hidmap_result result = HIDMAP_OK;

	switch (tag) {
	case MAIN_INPUT:
		result = on_input(p, value, out);
		break;
	case MAIN_OUTPUT:
		if (p->in_target && p->target == HIDMAP_KEYBOARD && p->g.page == PAGE_LED &&
		    !out->has_leds) {
			out->has_leds = true;
			out->led_report_id = p->g.report_id;
		}
		break;
	case MAIN_COLLECTION:
		if (p->depth == UINT8_MAX) {
			return HIDMAP_MALFORMED;
		}
		if (p->depth == 0 && value == COLLECTION_APPLICATION) {
			enter_application(p);
		}
		p->depth++;
		break;
	case MAIN_END_COLLECTION:
		if (p->depth == 0) {
			return HIDMAP_MALFORMED;
		}
		if (--p->depth == 0) {
			p->in_target = false;
		}
		break;
	default:
		break;
	}
	p->l = (struct locals){0};
	return result;
}

static enum hidmap_result on_global(struct parser *p, uint8_t tag, const uint8_t *data,
				    uint8_t size)
{
	switch (tag) {
	case GLOBAL_USAGE_PAGE:
		p->g.page = (uint16_t)unsigned_value(data, size);
		break;
	case GLOBAL_LOGICAL_MIN:
		p->g.logical_min = signed_value(data, size);
		break;
	case GLOBAL_LOGICAL_MAX:
		p->g.logical_max = signed_value(data, size);
		break;
	case GLOBAL_REPORT_SIZE:
		p->g.size = unsigned_value(data, size);
		break;
	case GLOBAL_REPORT_ID:
		p->g.report_id = (uint8_t)unsigned_value(data, size);
		break;
	case GLOBAL_REPORT_COUNT:
		p->g.count = unsigned_value(data, size);
		break;
	case GLOBAL_PUSH:
		if (p->stacked == STACK_DEPTH) {
			return HIDMAP_MALFORMED;
		}
		p->stack[p->stacked++] = p->g;
		break;
	case GLOBAL_POP:
		if (p->stacked == 0) {
			return HIDMAP_MALFORMED;
		}
		p->g = p->stack[--p->stacked];
		break;
	default:
		break;
	}
	return HIDMAP_OK;
}

static void on_local(struct parser *p, uint8_t tag, uint32_t value)
{
	switch (tag) {
	case LOCAL_USAGE:
		p->l.usage = value & UINT16_MAX;
		p->l.has_usage = true;
		break;
	case LOCAL_USAGE_MIN:
		p->l.usage_min = value & UINT16_MAX;
		p->l.has_min = true;
		break;
	case LOCAL_USAGE_MAX:
		p->l.has_max = true;
		break;
	default:
		break;
	}
}

enum hidmap_result hidmap_parse(const uint8_t *map, size_t len, struct hidmap *out)
{
	struct parser p = {0};
	size_t at = 0;

	*out = (struct hidmap){0};
	while (at < len) {
		uint8_t prefix = map[at++];
		uint8_t size = prefix & 3;
		uint8_t type = (prefix >> 2) & 3;
		uint8_t tag = prefix >> 4;
		enum hidmap_result result = HIDMAP_OK;
		const uint8_t *data = &map[at];

		if (prefix == LONG_ITEM) {
			return HIDMAP_MALFORMED;
		}
		if (size == 3) {
			size = 4;
		}
		if (size > len - at) {
			return HIDMAP_MALFORMED;
		}
		at += size;
		if (type == TYPE_MAIN) {
			result = on_main(&p, tag, unsigned_value(data, size), out);
		} else if (type == TYPE_GLOBAL) {
			result = on_global(&p, tag, data, size);
		} else if (type == TYPE_LOCAL) {
			on_local(&p, tag, unsigned_value(data, size));
		}
		if (result != HIDMAP_OK) {
			return result;
		}
	}
	if (p.depth != 0) {
		return HIDMAP_MALFORMED;
	}
	for (uint8_t i = 0; i < out->count; i++) {
		if (out->fields[i].target == HIDMAP_KEYBOARD) {
			return HIDMAP_OK;
		}
	}
	return HIDMAP_NO_KEYBOARD;
}
