#include <string.h>
#include "fixture.h"
#include "hidkeys.h"
#include "test.h"

static struct hidmap air75, mx;
static struct hidkeys keys;

static void load(const char *name, struct hidmap *m)
{
	uint8_t map[512];
	size_t len = load_fixture(name, map, sizeof(map));

	CHECK_EQ(hidmap_parse(map, len, m), HIDMAP_OK);
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
	uint8_t want[REPORT_KEYBOARD_LEN] = {modifiers};

	CHECK_EQ(hex(slots, &want[2]), 6);
	CHECK_EQ(r->kind, REPORT_KEYBOARD);
	CHECK_MEM(r->keyboard, want, sizeof(want));
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
				pressed += last.keyboard[2] != 0;
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

static void seven_keys_report_rollover(void)
{
	struct report out[REPORT_KINDS];

	hidkeys_reset(&keys);
	send(&air75, 1, "02000405060708090a", out);
	check_keyboard(&out[0], 0x02, "040506070809");
	CHECK_EQ(send(&air75, 2, "00000400000000000000000000000000", out), 1);
	check_keyboard(&out[0], 0x02, "010101010101");
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

int main(void)
{
	load("air75-v2-report-map.hex", &air75);
	load("mx-keys-report-map.hex", &mx);
	RUN(air75_letter_and_release);
	RUN(air75_shift_letter);
	RUN(mx_keys_seven_byte_report);
	RUN(consumer_usage);
	RUN(mx_second_consumer_slot);
	RUN(replay_air75_fixture);
	RUN(replay_mx_keys_fixture);
	RUN(bitmap_report_merges_with_the_array_report);
	RUN(seven_keys_report_rollover);
	RUN(out_of_range_usages_are_dropped);
	RUN(array_values_below_four_are_not_keys);
	RUN(a_short_report_is_ignored);
	RUN(an_unknown_report_id_writes_nothing);
	RUN(reset_forgets_held_keys);
	return TEST_EXIT();
}
