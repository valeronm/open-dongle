#include <string.h>
#include "test.h"
#include "nuphy.h"

static const uint8_t pair_request[] = {
	0x82, 0x80, 0x70, 0x15, 0xa3, 0x29, 0x96, 0xbe, 0x5c, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xe6};
static const uint8_t pair_confirm[] = {
	0xc2, 0x81, 0x70, 0x15, 0xa3, 0x29, 0x26, 0xc1, 0x21, 0x59, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x4f};
static const uint8_t hello[] = {
	0x18, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xa7};
static const uint8_t name[] = {
	0x24, 0x56, 0x2c, 0x03, 0x4e, 0x00, 0x75, 0x00, 0x50, 0x00, 0x68, 0x00, 0x79, 0x00, 0x20,
	0x00, 0x41, 0x00, 0x69, 0x00, 0x72, 0x00, 0x5a, 0x33};
static const uint8_t keyboard_f[] = {
	0x7e, 0x61, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x42};
static const uint8_t consumer_volume_up[] = {
	0x30, 0x63, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xd6};
static const uint8_t system_sleep[] = {
	0x31, 0x64, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x71};
static const uint8_t keepalive[] = {0x10, 0x57, 0x5a, 0xc1, 0x00, 0x00, 0x00, 0x00};

static const uint8_t dongle_id[4] = {0x26, 0xc1, 0x21, 0x59};
static const uint8_t keyboard_id[4] = {0x70, 0x15, 0xa3, 0x29};

static void test_pair_request_carries_keyboard_and_previous_dongle_ids(void)
{
	static const uint8_t previous[4] = {0x96, 0xbe, 0x5c, 0x89};
	struct nuphy_msg m = nuphy_parse(pair_request, sizeof(pair_request));

	CHECK_EQ(m.kind, NUPHY_PAIR_REQUEST);
	CHECK_EQ(m.seq, 0x82);
	CHECK_MEM(m.ids.keyboard, keyboard_id, 4);
	CHECK_MEM(m.ids.dongle, previous, 4);
}

static void test_pair_confirm_carries_both_ids(void)
{
	struct nuphy_msg m = nuphy_parse(pair_confirm, sizeof(pair_confirm));

	CHECK_EQ(m.kind, NUPHY_PAIR_CONFIRM);
	CHECK_MEM(m.ids.keyboard, keyboard_id, 4);
	CHECK_MEM(m.ids.dongle, dongle_id, 4);
}

static void test_keyboard_report_is_the_8_byte_boot_report(void)
{
	static const uint8_t expected[8] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00};
	struct nuphy_msg m = nuphy_parse(keyboard_f, sizeof(keyboard_f));

	CHECK_EQ(m.kind, NUPHY_KEYBOARD);
	CHECK_MEM(m.keyboard, expected, 8);
}

static void test_media_and_system_reports_carry_a_usage(void)
{
	struct nuphy_msg consumer = nuphy_parse(consumer_volume_up, sizeof(consumer_volume_up));
	struct nuphy_msg sys = nuphy_parse(system_sleep, sizeof(system_sleep));

	CHECK_EQ(consumer.kind, NUPHY_CONSUMER);
	CHECK_EQ(consumer.usage, 0x00e9);
	CHECK_EQ(sys.kind, NUPHY_SYSTEM);
	CHECK_EQ(sys.usage, 0x0082);
}

static void test_short_frame_is_a_keepalive(void)
{
	CHECK_EQ(nuphy_parse(keepalive, sizeof(keepalive)).kind, NUPHY_KEEPALIVE);
}

static void test_hello_and_other_valid_types(void)
{
	CHECK_EQ(nuphy_parse(hello, sizeof(hello)).kind, NUPHY_HELLO);
	CHECK_EQ(nuphy_parse(name, sizeof(name)).kind, NUPHY_IGNORED);
}

static void test_bad_checksum_or_length_is_invalid(void)
{
	uint8_t corrupted[sizeof(pair_confirm)];

	CHECK_EQ(nuphy_parse(pair_confirm, sizeof(pair_confirm)).kind, NUPHY_PAIR_CONFIRM);
	CHECK_EQ(nuphy_parse(keepalive, sizeof(keepalive)).kind, NUPHY_KEEPALIVE);
	memcpy(corrupted, pair_confirm, sizeof(corrupted));
	corrupted[5] ^= 0x01;
	CHECK_EQ(nuphy_parse(corrupted, sizeof(corrupted)).kind, NUPHY_INVALID);
	CHECK_EQ(nuphy_parse(pair_confirm, 23).kind, NUPHY_INVALID);
	CHECK_EQ(nuphy_parse(keepalive, 7).kind, NUPHY_INVALID);
}

/* Filled with 0xff before each build, so a builder that writes nothing fails the comparison. */
static uint8_t out[NUPHY_FRAME_MAX];

static uint8_t *blank_out(void)
{
	memset(out, 0xff, sizeof(out));
	return out;
}

static void check_frame(size_t len, const uint8_t *expected, size_t expected_len)
{
	CHECK_EQ(len, expected_len);
	CHECK_MEM(out, expected, expected_len);
}

static void test_status_frame(void)
{
	static const uint8_t expected[] = {0x01, 0x30, 0x26, 0xc1, 0x21, 0x59, 0x02, 0x00,
					   0x00, 0x00, 0x5a, 0xee};

	check_frame(nuphy_status(blank_out(), 0x01, dongle_id, 0x02), expected, sizeof(expected));
}

static void test_status_keeps_only_the_three_lock_bits(void)
{
	nuphy_status(blank_out(), 0x01, dongle_id, 0xfa);
	CHECK_EQ(out[6], 0x02);
}

static void test_pair_reply_frame(void)
{
	static const uint8_t channels[3] = {26, 30, 34};
	static const uint8_t expected[] = {0x02, 0x32, 0x26, 0xc1, 0x21, 0x59, 0x70, 0x15,
					   0xa3, 0x29, 0x1a, 0x1e, 0x22, 0x5a, 0x9a};

	check_frame(nuphy_pair_reply(blank_out(), 0x02, dongle_id, keyboard_id, channels), expected,
		    sizeof(expected));
}

static void test_suspend_frame(void)
{
	static const uint8_t expected[] = {0x03, 0x34, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x91};

	check_frame(nuphy_suspend(blank_out(), 0x03), expected, sizeof(expected));
}

static void test_sequence_skips_zero(void)
{
	CHECK_EQ(nuphy_next_seq(0x01), 0x02);
	CHECK_EQ(nuphy_next_seq(0xff), 0x01);
}

static void test_report_messages_convert_to_reports(void)
{
	struct nuphy_msg key = nuphy_parse(keyboard_f, sizeof(keyboard_f));
	struct nuphy_msg consumer = nuphy_parse(consumer_volume_up, sizeof(consumer_volume_up));
	struct nuphy_msg sys = nuphy_parse(system_sleep, sizeof(system_sleep));
	struct report out;

	CHECK(nuphy_report(&key, &out));
	CHECK_EQ(out.kind, REPORT_KEYBOARD);
	CHECK_MEM(out.keyboard, key.keyboard, REPORT_KEYBOARD_LEN);
	CHECK(nuphy_report(&consumer, &out));
	CHECK_EQ(out.kind, REPORT_CONSUMER);
	CHECK_EQ(out.usage, 0x00e9);
	CHECK(nuphy_report(&sys, &out));
	CHECK_EQ(out.kind, REPORT_SYSTEM);
	CHECK_EQ(out.usage, 0x0082);
}

static void test_other_messages_carry_no_report(void)
{
	struct nuphy_msg m = nuphy_parse(keepalive, sizeof(keepalive));
	struct report out;

	CHECK(!nuphy_report(&m, &out));
}

int main(void)
{
	RUN(test_pair_request_carries_keyboard_and_previous_dongle_ids);
	RUN(test_pair_confirm_carries_both_ids);
	RUN(test_keyboard_report_is_the_8_byte_boot_report);
	RUN(test_media_and_system_reports_carry_a_usage);
	RUN(test_report_messages_convert_to_reports);
	RUN(test_other_messages_carry_no_report);
	RUN(test_short_frame_is_a_keepalive);
	RUN(test_hello_and_other_valid_types);
	RUN(test_bad_checksum_or_length_is_invalid);
	RUN(test_status_frame);
	RUN(test_status_keeps_only_the_three_lock_bits);
	RUN(test_pair_reply_frame);
	RUN(test_suspend_frame);
	RUN(test_sequence_skips_zero);
	return TEST_EXIT();
}
