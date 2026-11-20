#include <string.h>
#include "fixture.h"
#include "hidkeys.h"
#include "test.h"

static struct hidmap air75, mx, mouse;
static struct hidkeys keys;

static void load(const char *name, enum device_kind kind, struct hidmap *m)
{
	uint8_t map[512];
	size_t len = load_fixture(name, map, sizeof(map));

	CHECK_EQ(hidmap_parse(map, len, kind, m), HIDMAP_OK);
}

static size_t hex(const char *text, uint8_t *out)
{
	size_t n = 0;
	unsigned int byte;

	while (sscanf(text + 2 * n, "%2x", &byte) == 1) {
		out[n++] = (uint8_t)byte;
	}
	return n;
}

static size_t send(const struct hidmap *m, uint8_t id, const char *text, struct report *out)
{
	uint8_t data[64];
	size_t len = hex(text, data);

	return hidkeys_translate(&keys, m, id, data, len, out);
}

static void check_keyboard(const struct report *r, uint8_t modifiers, const char *slots)
{
	uint8_t keys[REPORT_KEY_SLOTS];
	struct report want;

	CHECK_EQ(hex(slots, keys), REPORT_KEY_SLOTS);
	want = reports_held_keys(modifiers, keys);
	CHECK_EQ(r->kind, REPORT_KEYBOARD);
	CHECK_MEM(r->keys, want.keys, REPORT_KEYS_LEN);
}


static void air75_letter_and_release(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	CHECK_EQ(send(&air75, 1, "0000040000000000", out), 1);
	check_keyboard(&out[0], 0, "040000000000");
	CHECK_EQ(send(&air75, 1, "0000000000000000", out), 1);
	check_keyboard(&out[0], 0, "000000000000");
}

static void air75_shift_letter(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	CHECK_EQ(send(&air75, 1, "0200040000000000", out), 1);
	check_keyboard(&out[0], 0x02, "040000000000");
}

static void mx_keys_seven_byte_report(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	CHECK_EQ(send(&mx, 1, "02040000000000", out), 1);
	check_keyboard(&out[0], 0x02, "040000000000");
}

static void consumer_usage(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	CHECK_EQ(send(&air75, 3, "e900", out), 1);
	CHECK_EQ(out[0].kind, REPORT_CONSUMER);
	CHECK_EQ(out[0].usage, 0xe9);
	CHECK_EQ(send(&air75, 3, "0000", out), 1);
	CHECK_EQ(out[0].usage, 0);
	CHECK_EQ(send(&mx, 3, "e9000000", out), 1);
	CHECK_EQ(out[0].kind, REPORT_CONSUMER);
	CHECK_EQ(out[0].usage, 0xe9);
	CHECK_EQ(send(&mx, 3, "00000000", out), 1);
	CHECK_EQ(out[0].usage, 0);
}

static void mx_second_consumer_slot(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	CHECK_EQ(send(&mx, 3, "0000cd00", out), 1);
	CHECK_EQ(out[0].usage, 0xcd);
}

static void replay(const char *name, const struct hidmap *m)
{
	char text[160];
	struct report out[REPORT_KINDS];
	struct report last = {.kind = REPORT_KEYBOARD};
	unsigned int id;
	size_t lines = 0, pressed = 0;
	FILE *f = fixture_open(name);

	if (!f) {
		return;
	}
	hidkeys_reset(&keys);
	while (fscanf(f, "%u %159s", &id, text) == 2) {
		size_t n = send(m, (uint8_t)id, text, out);

		lines++;
		for (size_t i = 0; i < n; i++) {
			if (out[i].kind == REPORT_KEYBOARD) {
				last = out[i];
				pressed += !reports_keys_empty(&last);
			}
		}
	}
	fclose(f);
	CHECK(lines > 10);
	CHECK(pressed > 3);
	check_keyboard(&last, 0, "000000000000");
}

static void replay_air75_fixture(void)
{
	replay("air75-v2-reports.txt", &air75);
}

static void replay_mx_keys_fixture(void)
{
	replay("mx-keys-reports.txt", &mx);
}

static void bitmap_report_merges_with_the_array_report(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	send(&air75, 1, "0000040000000000", out);
	CHECK_EQ(send(&air75, 2, "00200000000000000000000000000000", out), 1);
	check_keyboard(&out[0], 0, "040500000000");
	CHECK_EQ(send(&air75, 2, "00000000000000000000000000000000", out), 1);
	check_keyboard(&out[0], 0, "040000000000");
}

static void seven_keys_from_two_reports_are_all_held(void)
{
	static const uint8_t held[] = {0x04, 0x16, 0x09, 0x0d, 0x0e, 0x07, 0x0f};
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	send(&air75, 1, "00000416090d0e07", out);
	CHECK_EQ(send(&air75, 2, "00008000000000000000000000000000", out), 1);
	for (size_t i = 0; i < sizeof(held); i++) {
		CHECK(reports_key_held(&out[0], held[i]));
	}
	CHECK(!reports_key_held(&out[0], 0x01));
}

static void out_of_range_usages_are_dropped(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	CHECK_EQ(send(&air75, 3, "a102", out), 1);
	CHECK_EQ(out[0].usage, 0);
	CHECK_EQ(send(&air75, 3, "a002", out), 1);
	CHECK_EQ(out[0].usage, 0x2a0);
	CHECK_EQ(send(&air75, 5, "8400", out), 1);
	CHECK_EQ(out[0].kind, REPORT_SYSTEM);
	CHECK_EQ(out[0].usage, 0);
	CHECK_EQ(send(&air75, 5, "8200", out), 1);
	CHECK_EQ(out[0].usage, 0x82);
}

static void array_values_below_four_are_not_keys(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	CHECK_EQ(send(&air75, 1, "0000010101010101", out), 1);
	check_keyboard(&out[0], 0, "000000000000");
}

static void a_short_report_is_ignored(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	send(&air75, 1, "0000040000000000", out);
	CHECK_EQ(send(&air75, 1, "02", out), 0);
	CHECK_EQ(send(&air75, 2, "00000000000000000000000000000000", out), 1);
	check_keyboard(&out[0], 0, "040000000000");
}

static void an_unknown_report_id_writes_nothing(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	CHECK_EQ(send(&air75, 4, "0000000000", out), 0);
	CHECK_EQ(send(&mx, 17, "ff040001010100000000000000000000000000", out), 0);
}

static void reset_forgets_held_keys(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	send(&air75, 1, "0000040000000000", out);
	hidkeys_reset(&keys);
	CHECK_EQ(send(&air75, 2, "00000000000000000000000000000000", out), 1);
	check_keyboard(&out[0], 0, "000000000000");
}

static void check_pointer(const char *text, uint16_t buttons, int16_t x, int16_t y, int8_t wheel,
			  int8_t pan)
{
	uint8_t data[64];
	size_t len = hex(text, data);
	struct report out;

	CHECK(hidkeys_pointer(&mouse.pointer, 2, data, len, &out));
	CHECK_EQ(out.kind, REPORT_MOUSE);
	CHECK_EQ(out.mouse.buttons, buttons);
	CHECK_EQ(out.mouse.x, x);
	CHECK_EQ(out.mouse.y, y);
	CHECK_EQ(out.mouse.wheel, wheel);
	CHECK_EQ(out.mouse.pan, pan);
}

static void mx_master_reports_translate(void)
{
	check_pointer("00000200000000", 0, 2, 0, 0, 0);
	check_pointer("000002e0ff0000", 0, 2, -2, 0, 0);
	check_pointer("0000ffffff0000", 0, -1, -1, 0, 0);
	check_pointer("20000000000000", 0x20, 0, 0, 0, 0);
	check_pointer("00000110000001", 0, 1, 1, 0, 1);
	check_pointer("000001f0ff00ff", 0, 1, -1, 0, -1);
	check_pointer("00000118800000", 0, -2047, -2047, 0, 0);
	check_pointer("0000000000ff00", 0, 0, 0, -1, 0);
}

static void another_report_or_a_short_one_is_not_a_pointer(void)
{
	uint8_t data[64];
	struct report out;

	CHECK(!hidkeys_pointer(&mouse.pointer, 17, data,
			       hex("ff040001010100000000000000000000000000", data), &out));
	CHECK(!hidkeys_pointer(&mouse.pointer, 2, data, hex("000002000000", data), &out));
}

static void a_wide_wheel_is_clamped(void)
{
	const struct hidmap_pointer wide = {
		.x = {.bit_offset = 0, .size = 8, .is_signed = true},
		.y = {.bit_offset = 8, .size = 8, .is_signed = true},
		.wheel = {.bit_offset = 16, .size = 16, .is_signed = true},
	};
	uint8_t data[64];
	struct report out;

	CHECK(hidkeys_pointer(&wide, 0, data, hex("0000d4fe", data), &out));
	CHECK_EQ(out.mouse.wheel, -127);
	CHECK(hidkeys_pointer(&wide, 0, data, hex("00002c01", data), &out));
	CHECK_EQ(out.mouse.wheel, 127);
}

int main(void)
{
	load("air75-v2-report-map.hex", DEVICE_KIND_KEYBOARD, &air75);
	load("mx-keys-report-map.hex", DEVICE_KIND_KEYBOARD, &mx);
	load("mx-master-3s-report-map.hex", DEVICE_KIND_MOUSE, &mouse);
	RUN(air75_letter_and_release);
	RUN(air75_shift_letter);
	RUN(mx_keys_seven_byte_report);
	RUN(consumer_usage);
	RUN(mx_second_consumer_slot);
	RUN(replay_air75_fixture);
	RUN(replay_mx_keys_fixture);
	RUN(bitmap_report_merges_with_the_array_report);
	RUN(seven_keys_from_two_reports_are_all_held);
	RUN(out_of_range_usages_are_dropped);
	RUN(array_values_below_four_are_not_keys);
	RUN(a_short_report_is_ignored);
	RUN(an_unknown_report_id_writes_nothing);
	RUN(reset_forgets_held_keys);
	RUN(mx_master_reports_translate);
	RUN(another_report_or_a_short_one_is_not_a_pointer);
	RUN(a_wide_wheel_is_clamped);
	return TEST_EXIT();
}
