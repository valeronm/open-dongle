#include <string.h>
#include "fixture.h"
#include "hidmap.h"
#include "test.h"

static const uint8_t consumer_only[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x19, 0x01, 0x2a, 0xa0, 0x02,
	0x15, 0x01, 0x26, 0xa0, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xc0,
};

static const uint8_t boot_keyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25,
	0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

static void check_field(const struct hidmap_field *f, uint8_t id, enum hidmap_target target,
			enum hidmap_shape shape, uint16_t offset, uint8_t size, uint8_t count,
			uint16_t usage_min)
{
	CHECK_EQ(f->report_id, id);
	CHECK_EQ(f->target, target);
	CHECK_EQ(f->shape, shape);
	CHECK_EQ(f->bit_offset, offset);
	CHECK_EQ(f->size, size);
	CHECK_EQ(f->count, count);
	CHECK_EQ(f->usage_min, usage_min);
}

static void air75_map_parses(void)
{
	uint8_t map[512];
	size_t len = load_fixture("air75-v2-report-map.hex", map, sizeof(map));
	struct hidmap m;

	CHECK_EQ(len, 262);
	CHECK_EQ(hidmap_parse(map, len, &m), HIDMAP_OK);
	CHECK_EQ(m.count, 6);
	check_field(&m.fields[0], 1, HIDMAP_KEYBOARD, HIDMAP_BITS, 0, 1, 8, 0xe0);
	check_field(&m.fields[1], 1, HIDMAP_KEYBOARD, HIDMAP_ARRAY, 16, 8, 6, 0);
	check_field(&m.fields[2], 2, HIDMAP_KEYBOARD, HIDMAP_BITS, 0, 1, 8, 0xe0);
	check_field(&m.fields[3], 2, HIDMAP_KEYBOARD, HIDMAP_BITS, 8, 1, 120, 0);
	check_field(&m.fields[4], 3, HIDMAP_CONSUMER, HIDMAP_ARRAY, 0, 16, 1, 1);
	CHECK_EQ(m.fields[4].logical_min, 1);
	CHECK_EQ(m.fields[4].logical_max, 0x2a0);
	check_field(&m.fields[5], 5, HIDMAP_SYSTEM, HIDMAP_ARRAY, 0, 16, 1, 1);
	CHECK_EQ(m.fields[5].logical_max, 0xb7);
	CHECK(m.has_leds);
	CHECK_EQ(m.led_report_id, 1);
}

static void mx_keys_map_parses(void)
{
	uint8_t map[512];
	size_t len = load_fixture("mx-keys-report-map.hex", map, sizeof(map));
	struct hidmap m;

	CHECK_EQ(len, 181);
	CHECK_EQ(hidmap_parse(map, len, &m), HIDMAP_OK);
	CHECK_EQ(m.count, 3);
	check_field(&m.fields[0], 1, HIDMAP_KEYBOARD, HIDMAP_BITS, 0, 1, 8, 0xe0);
	check_field(&m.fields[1], 1, HIDMAP_KEYBOARD, HIDMAP_ARRAY, 8, 8, 6, 0);
	CHECK_EQ(m.fields[1].logical_max, 0xa4);
	check_field(&m.fields[2], 3, HIDMAP_CONSUMER, HIDMAP_ARRAY, 0, 16, 2, 1);
	CHECK(m.has_leds);
	CHECK_EQ(m.led_report_id, 1);
}

static void a_map_without_a_keyboard_is_refused(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(consumer_only, sizeof(consumer_only), &m), HIDMAP_NO_KEYBOARD);
}

static void a_truncated_item_is_malformed(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(boot_keyboard, 3, &m), HIDMAP_MALFORMED);
}

static void an_unclosed_collection_is_malformed(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(boot_keyboard, sizeof(boot_keyboard) - 1, &m), HIDMAP_MALFORMED);
}

static void a_stray_end_collection_is_malformed(void)
{
	static const uint8_t stray[] = {0xc0};
	struct hidmap m;

	CHECK_EQ(hidmap_parse(stray, sizeof(stray), &m), HIDMAP_MALFORMED);
}

static void a_long_item_is_malformed(void)
{
	static const uint8_t long_item[] = {0xfe, 0x00, 0x00};
	struct hidmap m;

	CHECK_EQ(hidmap_parse(long_item, sizeof(long_item), &m), HIDMAP_MALFORMED);
}

static void too_many_fields_is_too_large(void)
{
	static const uint8_t open[] = {0x05, 0x01, 0x09, 0x06, 0xa1, 0x01};
	static const uint8_t field[] = {0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00,
					0x25, 0x65, 0x75, 0x08, 0x95, 0x01, 0x81, 0x00};
	uint8_t map[sizeof(open) + (HIDMAP_FIELDS_MAX + 1) * sizeof(field) + 1];
	size_t len = 0;
	struct hidmap m;

	memcpy(map, open, sizeof(open));
	len += sizeof(open);
	for (int i = 0; i < HIDMAP_FIELDS_MAX + 1; i++) {
		memcpy(&map[len], field, sizeof(field));
		len += sizeof(field);
	}
	map[len++] = 0xc0;
	CHECK_EQ(hidmap_parse(map, len, &m), HIDMAP_TOO_LARGE);
	map[len - 1 - sizeof(field)] = 0xc0;
	CHECK_EQ(hidmap_parse(map, len - sizeof(field), &m), HIDMAP_OK);
	CHECK_EQ(m.count, HIDMAP_FIELDS_MAX);
}

static void a_map_with_no_report_ids_parses(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(boot_keyboard, sizeof(boot_keyboard), &m), HIDMAP_OK);
	CHECK_EQ(m.count, 2);
	check_field(&m.fields[0], 0, HIDMAP_KEYBOARD, HIDMAP_BITS, 0, 1, 8, 0xe0);
	check_field(&m.fields[1], 0, HIDMAP_KEYBOARD, HIDMAP_ARRAY, 16, 8, 6, 0);
	CHECK(!m.has_leds);
}

int main(void)
{
	RUN(air75_map_parses);
	RUN(mx_keys_map_parses);
	RUN(a_map_without_a_keyboard_is_refused);
	RUN(a_truncated_item_is_malformed);
	RUN(an_unclosed_collection_is_malformed);
	RUN(a_stray_end_collection_is_malformed);
	RUN(a_long_item_is_malformed);
	RUN(too_many_fields_is_too_large);
	RUN(a_map_with_no_report_ids_parses);
	return TEST_EXIT();
}
