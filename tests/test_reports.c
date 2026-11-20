#include "test.h"
#include "reports.h"

static struct reports r;

static bool send_key(uint8_t code)
{
	struct report next = {.kind = REPORT_KEYBOARD};

	next.keyboard[2] = code;
	return reports_update(&r, &next, REPORT_UNSTAMPED);
}

static void drain(void)
{
	struct report_entry out;

	for (int iface = 0; iface < REPORT_IFACES; iface++) {
		while (reports_peek(&r, iface, &out)) {
			reports_pop(&r, iface);
		}
	}
}

static void before(void)
{
	reports_init(&r);
	reports_set_live(&r, true);
	drain();
}

static void test_repeated_report_is_queued_once(void)
{
	struct report_entry out;

	before();
	CHECK(send_key(0x09));
	CHECK(!send_key(0x09));
	CHECK(!send_key(0x09));
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	reports_pop(&r, REPORT_IFACE_KEYBOARD);
	CHECK(!reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
}

static void test_quick_tap_keeps_press_and_release_in_order(void)
{
	struct report_entry out;

	before();
	send_key(0x09);
	send_key(0x00);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.report.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.report.keyboard[2], 0x09);
	reports_pop(&r, REPORT_IFACE_KEYBOARD);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.report.keyboard[2], 0x00);
}

static void test_media_report_carries_its_usage(void)
{
	struct report consumer = {.kind = REPORT_CONSUMER, .usage = 0x00e9};
	struct report_entry out;

	before();
	reports_update(&r, &consumer, REPORT_UNSTAMPED);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.report.kind, REPORT_CONSUMER);
	CHECK_EQ(out.report.usage, 0x00e9);
}

static void test_while_not_live_only_the_current_state_survives(void)
{
	struct report_entry out;
	int keyboards = 0;

	before();
	reports_set_live(&r, false);
	send_key(0x09);
	send_key(0x0a);
	CHECK(!reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK(!reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	reports_set_live(&r, true);
	while (reports_peek(&r, REPORT_IFACE_KEYBOARD, &out)) {
		keyboards++;
		CHECK_EQ(out.report.kind, REPORT_KEYBOARD);
		CHECK_EQ(out.report.keyboard[2], 0x0a);
		reports_pop(&r, REPORT_IFACE_KEYBOARD);
	}
	CHECK_EQ(keyboards, 1);
}

static void test_release_all_lets_go_of_a_held_key(void)
{
	struct report_entry out;

	before();
	send_key(0x09);
	drain();
	reports_release_all(&r);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.report.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.report.keyboard[2], 0x00);
}

static void test_release_all_lets_go_of_held_media_and_system_keys(void)
{
	struct report consumer = {.kind = REPORT_CONSUMER, .usage = 0x00e9};
	struct report system = {.kind = REPORT_SYSTEM, .usage = 0x0081};
	struct report_entry out;

	before();
	reports_update(&r, &consumer, REPORT_UNSTAMPED);
	reports_update(&r, &system, REPORT_UNSTAMPED);
	drain();
	reports_release_all(&r);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.report.kind, REPORT_CONSUMER);
	CHECK_EQ(out.report.usage, 0);
	reports_pop(&r, REPORT_IFACE_EXTRA);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.report.kind, REPORT_SYSTEM);
	CHECK_EQ(out.report.usage, 0);
	reports_pop(&r, REPORT_IFACE_EXTRA);
	CHECK(!reports_peek(&r, REPORT_IFACE_EXTRA, &out));
}

static void test_overflow_merges_the_oldest_change(void)
{
	struct report_entry out;

	before();
	for (int i = 1; i <= REPORTS_QUEUE_LEN + 1; i++) {
		send_key((uint8_t)i);
	}
	CHECK_EQ(r.merges, 1);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.report.keyboard[2], 2);
}

static void test_consumer_report_does_not_hold_up_the_keyboard(void)
{
	struct report consumer = {.kind = REPORT_CONSUMER, .usage = 0x00e9};
	struct report_entry out;

	before();
	reports_update(&r, &consumer, REPORT_UNSTAMPED);
	send_key(0x09);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.report.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.report.keyboard[2], 0x09);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.report.kind, REPORT_CONSUMER);
	CHECK_EQ(out.report.usage, 0x00e9);
}

static void test_going_live_queues_each_kind_on_its_interface(void)
{
	struct report consumer = {.kind = REPORT_CONSUMER, .usage = 0x00e9};
	struct report system = {.kind = REPORT_SYSTEM, .usage = 0x0081};
	struct report_entry out;

	before();
	reports_set_live(&r, false);
	send_key(0x09);
	reports_update(&r, &consumer, REPORT_UNSTAMPED);
	reports_update(&r, &system, REPORT_UNSTAMPED);
	reports_set_live(&r, true);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.report.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.report.keyboard[2], 0x09);
	reports_pop(&r, REPORT_IFACE_KEYBOARD);
	CHECK(!reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.report.kind, REPORT_CONSUMER);
	CHECK_EQ(out.report.usage, 0x00e9);
	reports_pop(&r, REPORT_IFACE_EXTRA);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.report.kind, REPORT_SYSTEM);
	CHECK_EQ(out.report.usage, 0x0081);
	reports_pop(&r, REPORT_IFACE_EXTRA);
	CHECK(!reports_peek(&r, REPORT_IFACE_EXTRA, &out));
}

static void test_a_key_modifier_or_media_usage_counts_as_held(void)
{
	struct report shift = {.kind = REPORT_KEYBOARD, .keyboard = {0x02}};
	struct report volume = {.kind = REPORT_CONSUMER, .usage = 0xe9};

	before();
	CHECK(!reports_any_held(&r));
	send_key(0x04);
	CHECK(reports_any_held(&r));
	send_key(0);
	CHECK(!reports_any_held(&r));
	reports_update(&r, &shift, REPORT_UNSTAMPED);
	CHECK(reports_any_held(&r));
	reports_release_all(&r);
	reports_update(&r, &volume, REPORT_UNSTAMPED);
	CHECK(reports_any_held(&r));
	reports_release_all(&r);
	CHECK(!reports_any_held(&r));
}

static struct report mouse_report(uint16_t buttons, int16_t x, int16_t y)
{
	return (struct report){.kind = REPORT_MOUSE, .mouse = {.buttons = buttons, .x = x, .y = y}};
}

static bool move(uint16_t buttons, int16_t x, int16_t y)
{
	struct report next = mouse_report(buttons, x, y);

	return reports_update(&r, &next, REPORT_UNSTAMPED);
}

static int mouse_queued(void)
{
	return r.queues[REPORT_IFACE_MOUSE].count;
}

static void test_mouse_reports_go_to_their_own_interface(void)
{
	struct report_entry out;

	before();
	CHECK(move(0, 3, -2));
	CHECK(!reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK(!reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK(reports_peek(&r, REPORT_IFACE_MOUSE, &out));
	CHECK_EQ(out.report.kind, REPORT_MOUSE);
	CHECK_EQ(out.report.mouse.x, 3);
	CHECK_EQ(out.report.mouse.y, -2);
}

static void test_a_mouse_report_without_motion_or_button_change_is_dropped(void)
{
	before();
	CHECK(!move(0, 0, 0));
	CHECK_EQ(mouse_queued(), 0);
	CHECK(move(1, 0, 0));
	CHECK(!move(1, 0, 0));
	CHECK_EQ(mouse_queued(), 1);
}

static void test_queued_motion_with_the_same_buttons_adds_up(void)
{
	struct report_entry out;

	before();
	move(0, 3, 1);
	move(0, 4, -5);
	CHECK_EQ(mouse_queued(), 1);
	CHECK(reports_peek(&r, REPORT_IFACE_MOUSE, &out));
	CHECK_EQ(out.report.mouse.x, 7);
	CHECK_EQ(out.report.mouse.y, -4);
}

static void test_a_click_is_never_merged_away(void)
{
	struct report_entry out;

	before();
	move(0, 1, 0);
	move(1, 0, 0);
	move(0, 0, 0);
	move(0, 2, 0);
	CHECK_EQ(mouse_queued(), 3);
	reports_peek(&r, REPORT_IFACE_MOUSE, &out);
	CHECK_EQ(out.report.mouse.buttons, 0);
	reports_pop(&r, REPORT_IFACE_MOUSE);
	reports_peek(&r, REPORT_IFACE_MOUSE, &out);
	CHECK_EQ(out.report.mouse.buttons, 1);
	reports_pop(&r, REPORT_IFACE_MOUSE);
	reports_peek(&r, REPORT_IFACE_MOUSE, &out);
	CHECK_EQ(out.report.mouse.buttons, 0);
	CHECK_EQ(out.report.mouse.x, 2);
}

static void test_merged_motion_saturates(void)
{
	struct report next = {.kind = REPORT_MOUSE, .mouse = {.x = 30000, .wheel = 100}};
	struct report_entry out;

	before();
	reports_update(&r, &next, REPORT_UNSTAMPED);
	reports_update(&r, &next, REPORT_UNSTAMPED);
	reports_peek(&r, REPORT_IFACE_MOUSE, &out);
	CHECK_EQ(out.report.mouse.x, 32767);
	CHECK_EQ(out.report.mouse.wheel, 127);
}

static void test_going_live_sends_the_buttons_without_motion(void)
{
	struct report_entry out;

	before();
	reports_set_live(&r, false);
	move(2, 50, 50);
	reports_set_live(&r, true);
	CHECK(reports_peek(&r, REPORT_IFACE_MOUSE, &out));
	CHECK_EQ(out.report.mouse.buttons, 2);
	CHECK_EQ(out.report.mouse.x, 0);
	CHECK_EQ(out.report.mouse.y, 0);
}

static void test_release_all_lets_go_of_a_held_mouse_button(void)
{
	struct report_entry out;

	before();
	move(1, 0, 0);
	CHECK(reports_any_held(&r));
	drain();
	reports_release_all(&r);
	CHECK(!reports_any_held(&r));
	CHECK(reports_peek(&r, REPORT_IFACE_MOUSE, &out));
	CHECK_EQ(out.report.mouse.buttons, 0);
}

static void test_a_mouse_report_encodes_in_report_protocol(void)
{
	struct report_mouse m = {.buttons = 0x0102, .x = -300, .y = 2, .wheel = -1, .pan = 1};
	uint8_t out[REPORT_MOUSE_LEN];
	const uint8_t want[REPORT_MOUSE_LEN] = {0x02, 0x01, 0xd4, 0xfe, 0x02, 0x00, 0xff, 0x01};

	CHECK_EQ(reports_encode_mouse(&m, false, out), REPORT_MOUSE_LEN);
	CHECK_MEM(out, want, REPORT_MOUSE_LEN);
}

static void test_a_mouse_report_encodes_in_boot_protocol(void)
{
	struct report_mouse m = {.buttons = 0x0102, .x = -300, .y = 200, .wheel = -1};
	uint8_t out[REPORT_MOUSE_LEN];
	const uint8_t want[REPORT_MOUSE_BOOT_LEN] = {0x02, 0x81, 0x7f};

	CHECK_EQ(reports_encode_mouse(&m, true, out), REPORT_MOUSE_BOOT_LEN);
	CHECK_MEM(out, want, REPORT_MOUSE_BOOT_LEN);
}

static void move_mouse(uint16_t buttons, uint8_t slot, uint32_t received_us)
{
	struct report next = mouse_report(buttons, 1, 0);
	struct report_stamp stamp = {.slot = slot, .received_us = received_us};

	reports_update(&r, &next, stamp);
}

static void deliver(enum report_iface iface, uint32_t now_us)
{
	struct report_entry out;

	while (reports_peek(&r, iface, &out)) {
		reports_pop(&r, iface);
		reports_delivered(&r, &out, now_us);
	}
}

static void test_a_delivered_report_counts_its_delay_toward_its_slot(void)
{
	before();
	move_mouse(0, 1, 1000);
	deliver(REPORT_IFACE_MOUSE, 2000);
	move_mouse(1, 1, 5000);
	deliver(REPORT_IFACE_MOUSE, 8000);
	CHECK_EQ(r.latency[1].count, 2);
	CHECK_EQ(reports_latency_average_us(&r.latency[1]), 2000);
	CHECK_EQ(r.latency[1].max_us, 3000);
	CHECK_EQ(r.latency[0].count, 0);
}

static void test_no_latency_reads_as_unknown(void)
{
	before();
	CHECK_EQ(reports_latency_average_us(&r.latency[0]), UINT32_MAX);
}

static void test_merged_motion_keeps_the_oldest_receipt(void)
{
	before();
	move_mouse(0, 1, 1000);
	move_mouse(0, 1, 2500);
	deliver(REPORT_IFACE_MOUSE, 4000);
	CHECK_EQ(r.latency[1].count, 1);
	CHECK_EQ(r.latency[1].max_us, 3000);
}

static void test_a_release_the_dongle_makes_up_is_not_timed(void)
{
	before();
	move_mouse(1, 1, 1000);
	deliver(REPORT_IFACE_MOUSE, 2000);
	reports_release_all(&r);
	deliver(REPORT_IFACE_MOUSE, 9000);
	CHECK_EQ(r.latency[1].count, 1);
	CHECK_EQ(r.latency[1].max_us, 1000);
}

static void test_the_state_resent_on_going_live_is_not_timed(void)
{
	before();
	move_mouse(1, 1, 1000);
	reports_set_live(&r, false);
	reports_set_live(&r, true);
	deliver(REPORT_IFACE_MOUSE, 9000);
	CHECK_EQ(r.latency[1].count, 0);
}

static void test_a_delay_across_the_clock_wrapping_counts_its_length(void)
{
	before();
	move_mouse(0, 0, UINT32_MAX - 99);
	deliver(REPORT_IFACE_MOUSE, 400);
	CHECK_EQ(r.latency[0].max_us, 500);
}

static void test_a_wheel_in_notches_is_multiplied_for_a_high_resolution_host(void)
{
	int carry = 0;

	CHECK_EQ(reports_scale_wheel(&carry, 2, 1, 8), 16);
	CHECK_EQ(reports_scale_wheel(&carry, -1, 1, 8), -8);
	CHECK_EQ(carry, 0);
}

static void test_a_finer_wheel_carries_what_does_not_make_a_whole_step(void)
{
	int carry = 0;

	CHECK_EQ(reports_scale_wheel(&carry, 1, 15, 8), 0);
	CHECK_EQ(reports_scale_wheel(&carry, 1, 15, 8), 1);
	CHECK_EQ(reports_scale_wheel(&carry, 13, 15, 8), 7);
	CHECK_EQ(carry, 0);
	CHECK_EQ(reports_scale_wheel(&carry, -15, 15, 8), -8);
}

static void test_equal_resolutions_pass_the_wheel_through(void)
{
	int carry = 0;

	CHECK_EQ(reports_scale_wheel(&carry, 5, 8, 8), 5);
	CHECK_EQ(reports_scale_wheel(&carry, -3, 1, 1), -3);
}

int main(void)
{
	RUN(test_a_wheel_in_notches_is_multiplied_for_a_high_resolution_host);
	RUN(test_a_finer_wheel_carries_what_does_not_make_a_whole_step);
	RUN(test_equal_resolutions_pass_the_wheel_through);
	RUN(test_a_key_modifier_or_media_usage_counts_as_held);
	RUN(test_repeated_report_is_queued_once);
	RUN(test_quick_tap_keeps_press_and_release_in_order);
	RUN(test_media_report_carries_its_usage);
	RUN(test_while_not_live_only_the_current_state_survives);
	RUN(test_release_all_lets_go_of_a_held_key);
	RUN(test_release_all_lets_go_of_held_media_and_system_keys);
	RUN(test_overflow_merges_the_oldest_change);
	RUN(test_consumer_report_does_not_hold_up_the_keyboard);
	RUN(test_going_live_queues_each_kind_on_its_interface);
	RUN(test_mouse_reports_go_to_their_own_interface);
	RUN(test_a_mouse_report_without_motion_or_button_change_is_dropped);
	RUN(test_queued_motion_with_the_same_buttons_adds_up);
	RUN(test_a_click_is_never_merged_away);
	RUN(test_merged_motion_saturates);
	RUN(test_going_live_sends_the_buttons_without_motion);
	RUN(test_release_all_lets_go_of_a_held_mouse_button);
	RUN(test_a_mouse_report_encodes_in_report_protocol);
	RUN(test_a_mouse_report_encodes_in_boot_protocol);
	RUN(test_a_delivered_report_counts_its_delay_toward_its_slot);
	RUN(test_no_latency_reads_as_unknown);
	RUN(test_merged_motion_keeps_the_oldest_receipt);
	RUN(test_a_release_the_dongle_makes_up_is_not_timed);
	RUN(test_the_state_resent_on_going_live_is_not_timed);
	RUN(test_a_delay_across_the_clock_wrapping_counts_its_length);
	return TEST_EXIT();
}
