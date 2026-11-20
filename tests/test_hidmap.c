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
	CHECK_EQ(hidmap_parse(map, len, DEVICE_KEYBOARD, &m), HIDMAP_OK);
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
	CHECK_EQ(hidmap_parse(map, len, DEVICE_KEYBOARD, &m), HIDMAP_OK);
	CHECK_EQ(m.count, 3);
	check_field(&m.fields[0], 1, HIDMAP_KEYBOARD, HIDMAP_BITS, 0, 1, 8, 0xe0);
	check_field(&m.fields[1], 1, HIDMAP_KEYBOARD, HIDMAP_ARRAY, 8, 8, 6, 0);
	CHECK_EQ(m.fields[1].logical_max, 0xa4);
	check_field(&m.fields[2], 3, HIDMAP_CONSUMER, HIDMAP_ARRAY, 0, 16, 2, 1);
	CHECK(m.has_leds);
	CHECK_EQ(m.led_report_id, 1);
}

static size_t load_mx_master(uint8_t *map, size_t cap)
{
	size_t len = load_fixture("mx-master-3s-report-map.hex", map, cap);

	CHECK_EQ(len, 97);
	return len;
}

static void check_value(const struct hidmap_value *v, uint16_t offset, uint8_t size)
{
	CHECK_EQ(v->bit_offset, offset);
	CHECK_EQ(v->size, size);
	CHECK(v->is_signed);
}

static void mx_master_map_parses_as_a_mouse(void)
{
	uint8_t map[512];
	size_t len = load_mx_master(map, sizeof(map));
	struct hidmap m;

	CHECK_EQ(hidmap_parse(map, len, DEVICE_MOUSE, &m), HIDMAP_OK);
	CHECK_EQ(m.pointer.report_id, 2);
	CHECK_EQ(m.pointer.buttons.bit_offset, 0);
	CHECK_EQ(m.pointer.buttons.size, 16);
	CHECK(!m.pointer.buttons.is_signed);
	check_value(&m.pointer.x, 16, 12);
	check_value(&m.pointer.y, 28, 12);
	check_value(&m.pointer.wheel, 40, 8);
	check_value(&m.pointer.pan, 48, 8);
}

static void logitech_maps_declare_hidpp(void)
{
	uint8_t map[512];
	size_t len = load_fixture("mx-keys-report-map.hex", map, sizeof(map));
	struct hidmap m;

	CHECK_EQ(hidmap_parse(map, len, DEVICE_KEYBOARD, &m), HIDMAP_OK);
	CHECK(m.has_hidpp);
	len = load_mx_master(map, sizeof(map));
	CHECK_EQ(hidmap_parse(map, len, DEVICE_MOUSE, &m), HIDMAP_OK);
	CHECK(m.has_hidpp);
}

static void air75_map_has_no_hidpp(void)
{
	uint8_t map[512];
	size_t len = load_fixture("air75-v2-report-map.hex", map, sizeof(map));
	struct hidmap m;

	CHECK_EQ(hidmap_parse(map, len, DEVICE_KEYBOARD, &m), HIDMAP_OK);
	CHECK(!m.has_hidpp);
}

static void mx_master_map_is_not_a_keyboard(void)
{
	uint8_t map[512];
	size_t len = load_mx_master(map, sizeof(map));
	struct hidmap m;

	CHECK_EQ(hidmap_parse(map, len, DEVICE_KEYBOARD, &m), HIDMAP_NO_KEYBOARD);
}

static void a_map_without_a_pointer_is_not_a_mouse(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(boot_keyboard, sizeof(boot_keyboard), DEVICE_MOUSE, &m),
		 HIDMAP_NO_POINTER);
}

static void mx_keys_map_holds_its_own_pointer(void)
{
	uint8_t map[512];
	size_t len = load_fixture("mx-keys-report-map.hex", map, sizeof(map));
	struct hidmap m;

	CHECK_EQ(hidmap_parse(map, len, DEVICE_KEYBOARD, &m), HIDMAP_OK);
	CHECK_EQ(m.pointer.report_id, 2);
	check_value(&m.pointer.x, 16, 12);
	check_value(&m.pointer.y, 28, 12);
}

static void an_absolute_pointer_is_not_a_mouse(void)
{
	static const uint8_t xy_relative[] = {0x09, 0x31, 0x81, 0x06};
	uint8_t map[512];
	size_t len = load_mx_master(map, sizeof(map));
	struct hidmap m;
	size_t at = 0;

	while (memcmp(&map[at], xy_relative, sizeof(xy_relative)) != 0) {
		at++;
	}
	map[at + 3] = 0x02;
	CHECK_EQ(hidmap_parse(map, len, DEVICE_MOUSE, &m), HIDMAP_NO_POINTER);
}

static void a_four_byte_usage_carries_its_page(void)
{
	static const uint8_t map[] = {
		0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
		0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0x0b, 0x38, 0x02, 0x0c,
		0x00, 0x95, 0x01, 0x81, 0x06, 0xc0,
	};
	struct hidmap m;

	CHECK_EQ(hidmap_parse(map, sizeof(map), DEVICE_MOUSE, &m), HIDMAP_OK);
	check_value(&m.pointer.x, 0, 8);
	check_value(&m.pointer.y, 8, 8);
	check_value(&m.pointer.pan, 16, 8);
	CHECK_EQ(m.pointer.wheel.size, 0);
	CHECK_EQ(m.pointer.buttons.size, 0);
}

static void a_map_without_a_keyboard_is_refused(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(consumer_only, sizeof(consumer_only), DEVICE_KEYBOARD, &m),
		 HIDMAP_NO_KEYBOARD);
}

static void a_truncated_item_is_malformed(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(boot_keyboard, 3, DEVICE_KEYBOARD, &m), HIDMAP_MALFORMED);
}

static void an_unclosed_collection_is_malformed(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(boot_keyboard, sizeof(boot_keyboard) - 1, DEVICE_KEYBOARD, &m),
		 HIDMAP_MALFORMED);
}

static void a_stray_end_collection_is_malformed(void)
{
	static const uint8_t stray[] = {0xc0};
	struct hidmap m;

	CHECK_EQ(hidmap_parse(stray, sizeof(stray), DEVICE_KEYBOARD, &m), HIDMAP_MALFORMED);
}

static void a_long_item_is_malformed(void)
{
	static const uint8_t long_item[] = {0xfe, 0x00, 0x00};
	struct hidmap m;

	CHECK_EQ(hidmap_parse(long_item, sizeof(long_item), DEVICE_KEYBOARD, &m), HIDMAP_MALFORMED);
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
	CHECK_EQ(hidmap_parse(map, len, DEVICE_KEYBOARD, &m), HIDMAP_TOO_LARGE);
	map[len - 1 - sizeof(field)] = 0xc0;
	CHECK_EQ(hidmap_parse(map, len - sizeof(field), DEVICE_KEYBOARD, &m), HIDMAP_OK);
	CHECK_EQ(m.count, HIDMAP_FIELDS_MAX);
}

static void a_map_with_no_report_ids_parses(void)
{
	struct hidmap m;

	CHECK_EQ(hidmap_parse(boot_keyboard, sizeof(boot_keyboard), DEVICE_KEYBOARD, &m),
		 HIDMAP_OK);
	CHECK_EQ(m.count, 2);
	check_field(&m.fields[0], 0, HIDMAP_KEYBOARD, HIDMAP_BITS, 0, 1, 8, 0xe0);
	check_field(&m.fields[1], 0, HIDMAP_KEYBOARD, HIDMAP_ARRAY, 16, 8, 6, 0);
	CHECK(!m.has_leds);
}

int main(void)
{
	RUN(air75_map_parses);
	RUN(mx_keys_map_parses);
	RUN(mx_master_map_parses_as_a_mouse);
	RUN(logitech_maps_declare_hidpp);
	RUN(air75_map_has_no_hidpp);
	RUN(mx_master_map_is_not_a_keyboard);
	RUN(a_map_without_a_pointer_is_not_a_mouse);
	RUN(mx_keys_map_holds_its_own_pointer);
	RUN(an_absolute_pointer_is_not_a_mouse);
	RUN(a_four_byte_usage_carries_its_page);
	RUN(a_map_without_a_keyboard_is_refused);
	RUN(a_truncated_item_is_malformed);
	RUN(an_unclosed_collection_is_malformed);
	RUN(a_stray_end_collection_is_malformed);
	RUN(a_long_item_is_malformed);
	RUN(too_many_fields_is_too_large);
	RUN(a_map_with_no_report_ids_parses);
	return TEST_EXIT();
}
