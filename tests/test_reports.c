#include "test.h"
#include "reports.h"

static struct reports r;

static bool send_key(uint8_t code)
{
	struct report next = {.kind = REPORT_KEYBOARD};

	next.keyboard[2] = code;
	return reports_update(&r, &next);
}

static void drain(void)
{
	struct report out;

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
	struct report out;

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
	struct report out;

	before();
	send_key(0x09);
	send_key(0x00);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.keyboard[2], 0x09);
	reports_pop(&r, REPORT_IFACE_KEYBOARD);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.keyboard[2], 0x00);
}

static void test_media_report_carries_its_usage(void)
{
	struct report consumer = {.kind = REPORT_CONSUMER, .usage = 0x00e9};
	struct report out;

	before();
	reports_update(&r, &consumer);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.kind, REPORT_CONSUMER);
	CHECK_EQ(out.usage, 0x00e9);
}

static void test_while_not_live_only_the_current_state_survives(void)
{
	struct report out;
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
		CHECK_EQ(out.kind, REPORT_KEYBOARD);
		CHECK_EQ(out.keyboard[2], 0x0a);
		reports_pop(&r, REPORT_IFACE_KEYBOARD);
	}
	CHECK_EQ(keyboards, 1);
}

static void test_release_all_lets_go_of_a_held_key(void)
{
	struct report out;

	before();
	send_key(0x09);
	drain();
	reports_release_all(&r);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.keyboard[2], 0x00);
}

static void test_release_all_lets_go_of_held_media_and_system_keys(void)
{
	struct report consumer = {.kind = REPORT_CONSUMER, .usage = 0x00e9};
	struct report system = {.kind = REPORT_SYSTEM, .usage = 0x0081};
	struct report out;

	before();
	reports_update(&r, &consumer);
	reports_update(&r, &system);
	drain();
	reports_release_all(&r);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.kind, REPORT_CONSUMER);
	CHECK_EQ(out.usage, 0);
	reports_pop(&r, REPORT_IFACE_EXTRA);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.kind, REPORT_SYSTEM);
	CHECK_EQ(out.usage, 0);
	reports_pop(&r, REPORT_IFACE_EXTRA);
	CHECK(!reports_peek(&r, REPORT_IFACE_EXTRA, &out));
}

static void test_overflow_merges_the_oldest_change(void)
{
	struct report out;

	before();
	for (int i = 1; i <= REPORTS_QUEUE_LEN + 1; i++) {
		send_key((uint8_t)i);
	}
	CHECK_EQ(r.merges, 1);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.keyboard[2], 2);
}

static void test_consumer_report_does_not_hold_up_the_keyboard(void)
{
	struct report consumer = {.kind = REPORT_CONSUMER, .usage = 0x00e9};
	struct report out;

	before();
	reports_update(&r, &consumer);
	send_key(0x09);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.keyboard[2], 0x09);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.kind, REPORT_CONSUMER);
	CHECK_EQ(out.usage, 0x00e9);
}

static void test_going_live_queues_each_kind_on_its_interface(void)
{
	struct report consumer = {.kind = REPORT_CONSUMER, .usage = 0x00e9};
	struct report system = {.kind = REPORT_SYSTEM, .usage = 0x0081};
	struct report out;

	before();
	reports_set_live(&r, false);
	send_key(0x09);
	reports_update(&r, &consumer);
	reports_update(&r, &system);
	reports_set_live(&r, true);
	CHECK(reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK_EQ(out.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.keyboard[2], 0x09);
	reports_pop(&r, REPORT_IFACE_KEYBOARD);
	CHECK(!reports_peek(&r, REPORT_IFACE_KEYBOARD, &out));
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.kind, REPORT_CONSUMER);
	CHECK_EQ(out.usage, 0x00e9);
	reports_pop(&r, REPORT_IFACE_EXTRA);
	CHECK(reports_peek(&r, REPORT_IFACE_EXTRA, &out));
	CHECK_EQ(out.kind, REPORT_SYSTEM);
	CHECK_EQ(out.usage, 0x0081);
	reports_pop(&r, REPORT_IFACE_EXTRA);
	CHECK(!reports_peek(&r, REPORT_IFACE_EXTRA, &out));
}

int main(void)
{
	RUN(test_repeated_report_is_queued_once);
	RUN(test_quick_tap_keeps_press_and_release_in_order);
	RUN(test_media_report_carries_its_usage);
	RUN(test_while_not_live_only_the_current_state_survives);
	RUN(test_release_all_lets_go_of_a_held_key);
	RUN(test_release_all_lets_go_of_held_media_and_system_keys);
	RUN(test_overflow_merges_the_oldest_change);
	RUN(test_consumer_report_does_not_hold_up_the_keyboard);
	RUN(test_going_live_queues_each_kind_on_its_interface);
	return TEST_EXIT();
}
