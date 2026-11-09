#include "test.h"
#include "reports.h"

static struct reports r;

static bool send_key(uint8_t code)
{
	struct proto_msg m = {.kind = PROTO_KEYBOARD};

	m.keyboard[2] = code;
	return reports_on_message(&r, &m);
}

static void drain(void)
{
	struct report out;

	while (reports_peek(&r, &out)) {
		reports_pop(&r);
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
	CHECK(reports_peek(&r, &out));
	reports_pop(&r);
	CHECK(!reports_peek(&r, &out));
}

static void test_quick_tap_keeps_press_and_release_in_order(void)
{
	struct report out;

	before();
	send_key(0x09);
	send_key(0x00);
	CHECK(reports_peek(&r, &out));
	CHECK_EQ(out.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.keyboard[2], 0x09);
	reports_pop(&r);
	CHECK(reports_peek(&r, &out));
	CHECK_EQ(out.keyboard[2], 0x00);
}

static void test_media_report_carries_its_usage(void)
{
	struct proto_msg m = {.kind = PROTO_CONSUMER, .usage = 0x00e9};
	struct report out;

	before();
	reports_on_message(&r, &m);
	CHECK(reports_peek(&r, &out));
	CHECK_EQ(out.kind, REPORT_CONSUMER);
	CHECK_EQ(out.usage, 0x00e9);
}

static void test_other_messages_change_nothing(void)
{
	struct proto_msg m = {.kind = PROTO_KEEPALIVE};
	struct report out;

	before();
	CHECK(!reports_on_message(&r, &m));
	CHECK(!reports_peek(&r, &out));
	CHECK(send_key(0x09));
	CHECK(reports_peek(&r, &out));
}

static void test_while_not_live_only_the_current_state_survives(void)
{
	struct report out;
	int keyboards = 0;

	before();
	reports_set_live(&r, false);
	send_key(0x09);
	send_key(0x0a);
	CHECK(!reports_peek(&r, &out));
	reports_set_live(&r, true);
	while (reports_peek(&r, &out)) {
		if (out.kind == REPORT_KEYBOARD) {
			keyboards++;
			CHECK_EQ(out.keyboard[2], 0x0a);
		}
		reports_pop(&r);
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
	CHECK(reports_peek(&r, &out));
	CHECK_EQ(out.kind, REPORT_KEYBOARD);
	CHECK_EQ(out.keyboard[2], 0x00);
}

static void test_overflow_merges_the_oldest_change(void)
{
	struct report out;

	before();
	for (int i = 1; i <= REPORTS_QUEUE_LEN + 1; i++) {
		send_key((uint8_t)i);
	}
	CHECK_EQ(r.merges, 1);
	CHECK(reports_peek(&r, &out));
	CHECK_EQ(out.keyboard[2], 2);
}

int main(void)
{
	RUN(test_repeated_report_is_queued_once);
	RUN(test_quick_tap_keeps_press_and_release_in_order);
	RUN(test_media_report_carries_its_usage);
	RUN(test_other_messages_change_nothing);
	RUN(test_while_not_live_only_the_current_state_survives);
	RUN(test_release_all_lets_go_of_a_held_key);
	RUN(test_overflow_merges_the_oldest_change);
	return TEST_EXIT();
}
