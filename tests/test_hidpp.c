#include "test.h"
#include "hidpp.h"

#define DEVICE 0xff
#define UNIFIED_AT 0x06

static struct hidpp_msg request(struct hidpp *x, uint64_t now_ms)
{
	struct hidpp_msg m = {0};

	CHECK(hidpp_request(x, DEVICE, now_ms, 1000, &m));
	hidpp_taken(x, now_ms);
	return m;
}

static bool reply(struct hidpp *x, struct device_battery *b, uint8_t feature, uint8_t function,
		  uint8_t p0, uint8_t p1, uint8_t p2)
{
	struct hidpp_msg m = {.device = DEVICE, .feature = feature, .function = function,
			      .params = {p0, p1, p2}, .is_long = true};
	uint16_t usage;

	return hidpp_rx(x, b, &m, &usage);
}

static void check_find(const struct hidpp_msg *m, uint16_t feature)
{
	CHECK_EQ(m->device, DEVICE);
	CHECK_EQ(m->feature, 0x00);
	CHECK_EQ(m->function, 0x01);
	CHECK_EQ(m->params[0], feature >> 8);
	CHECK_EQ(m->params[1], feature & 0xff);
}

/* The device has no reprogrammable controls and no feature 0x1000. */
static void past_battery_status(struct hidpp *x, struct device_battery *b)
{
	hidpp_init(x, DEVICE_KEYBOARD);
	*b = DEVICE_BATTERY_UNKNOWN;
	hidpp_start(x);
	request(x, 0);
	reply(x, b, 0x00, 0x01, 0x00, 0x00, 0x00);
	request(x, 10);
	reply(x, b, 0x00, 0x01, 0x00, 0x00, 0x00);
}

static void with_unified_battery(struct hidpp *x, struct device_battery *b)
{
	past_battery_status(x, b);
	request(x, 20);
	reply(x, b, 0x00, 0x01, UNIFIED_AT, 0x00, 0x00);
}

static void test_a_device_without_battery_status_is_asked_for_unified_battery(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	past_battery_status(&x, &b);
	m = request(&x, 20);
	check_find(&m, 0x1004);
}

static void test_unified_battery_is_read_with_its_get_status_function(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	with_unified_battery(&x, &b);
	m = request(&x, 30);
	CHECK_EQ(m.feature, UNIFIED_AT);
	CHECK_EQ(m.function, 0x11);
}

static void test_a_unified_battery_reply_gives_the_level_and_charging(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	with_unified_battery(&x, &b);
	request(&x, 30);
	reply(&x, &b, UNIFIED_AT, 0x11, 55, 0x04, 0x01);
	CHECK_EQ(b.level, 55);
	CHECK_EQ(b.charging, DEVICE_CHARGING);
	CHECK(!hidpp_request(&x, DEVICE, 40, 1000, &m));
}

static void test_a_unified_battery_event_updates_the_battery(void)
{
	struct hidpp x;
	struct device_battery b;

	with_unified_battery(&x, &b);
	request(&x, 30);
	reply(&x, &b, UNIFIED_AT, 0x11, 55, 0x04, 0x00);
	reply(&x, &b, UNIFIED_AT, 0x00, 100, 0x08, 0x03);
	CHECK_EQ(b.level, 100);
	CHECK_EQ(b.charging, DEVICE_CHARGED);
}

static void test_a_device_with_neither_battery_feature_asks_nothing_more(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	past_battery_status(&x, &b);
	request(&x, 20);
	reply(&x, &b, 0x00, 0x01, 0x00, 0x00, 0x00);
	CHECK(!hidpp_request(&x, DEVICE, 30, 1000, &m));
	CHECK_EQ(b.level, DEVICE_NO_BATTERY);
}

#define WHEEL_AT 0x0d

/* The mouse has feature 0x1000 at index 4 and has answered its battery read. */
static void mouse_past_battery(struct hidpp *x, struct device_battery *b)
{
	struct hidpp_msg m;

	hidpp_init(x, DEVICE_MOUSE);
	*b = DEVICE_BATTERY_UNKNOWN;
	hidpp_start(x);
	m = request(x, 0);
	check_find(&m, 0x1000);
	reply(x, b, 0x00, 0x01, 0x04, 0x00, 0x00);
	request(x, 10);
	reply(x, b, 0x04, 0x01, 80, 0x00, 0x00);
}

static void mouse_with_wheel(struct hidpp *x, struct device_battery *b)
{
	mouse_past_battery(x, b);
	request(x, 20);
	reply(x, b, 0x00, 0x01, WHEEL_AT, 0x00, 0x00);
	request(x, 30);
	reply(x, b, WHEEL_AT, 0x01, 8, 0x00, 0x00);
}

static void test_a_mouse_is_asked_for_its_wheel_instead_of_a_divert(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	mouse_past_battery(&x, &b);
	m = request(&x, 20);
	check_find(&m, 0x2121);
}

static void test_the_wheel_capability_is_read_after_the_lookup(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	mouse_past_battery(&x, &b);
	request(&x, 20);
	reply(&x, &b, 0x00, 0x01, WHEEL_AT, 0x00, 0x00);
	m = request(&x, 30);
	CHECK_EQ(m.feature, WHEEL_AT);
	CHECK_EQ(m.function, 0x01);
}

static void test_a_wheel_the_host_wants_in_notches_is_set_to_notches(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	mouse_with_wheel(&x, &b);
	m = request(&x, 40);
	CHECK_EQ(m.feature, WHEEL_AT);
	CHECK_EQ(m.function, 0x21);
	CHECK_EQ(m.params[0], 0x00);
	reply(&x, &b, WHEEL_AT, 0x21, 0x00, 0x00, 0x00);
	CHECK(!hidpp_request(&x, DEVICE, 50, 1000, &m));
	CHECK_EQ(hidpp_wheel_steps(&x), 1);
}

static void test_a_host_asking_for_high_resolution_sets_the_wheel_to_it(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	mouse_with_wheel(&x, &b);
	request(&x, 40);
	reply(&x, &b, WHEEL_AT, 0x21, 0x00, 0x00, 0x00);
	hidpp_want_hires(&x, true);
	CHECK_EQ(hidpp_wheel_steps(&x), 1);
	m = request(&x, 50);
	CHECK_EQ(m.function, 0x21);
	CHECK_EQ(m.params[0], 0x02);
	reply(&x, &b, WHEEL_AT, 0x21, 0x02, 0x00, 0x00);
	CHECK_EQ(hidpp_wheel_steps(&x), 8);
}

static void test_a_wanted_high_resolution_is_set_at_the_first_chance(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	mouse_past_battery(&x, &b);
	hidpp_want_hires(&x, true);
	request(&x, 20);
	reply(&x, &b, 0x00, 0x01, WHEEL_AT, 0x00, 0x00);
	request(&x, 30);
	reply(&x, &b, WHEEL_AT, 0x01, 8, 0x00, 0x00);
	m = request(&x, 40);
	CHECK_EQ(m.function, 0x21);
	CHECK_EQ(m.params[0], 0x02);
}

static void test_a_reconnect_reads_the_battery_and_sets_the_wheel_mode_again(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	mouse_with_wheel(&x, &b);
	hidpp_want_hires(&x, true);
	request(&x, 40);
	reply(&x, &b, WHEEL_AT, 0x21, 0x02, 0x00, 0x00);
	hidpp_start(&x);
	CHECK_EQ(hidpp_wheel_steps(&x), 1);
	m = request(&x, 50);
	CHECK_EQ(m.feature, 0x04);
	CHECK_EQ(m.function, 0x01);
	reply(&x, &b, 0x04, 0x01, 80, 0x00, 0x00);
	m = request(&x, 60);
	CHECK_EQ(m.function, 0x21);
	CHECK_EQ(m.params[0], 0x02);
}

static void test_a_mouse_without_the_wheel_feature_stays_in_notches(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	mouse_past_battery(&x, &b);
	hidpp_want_hires(&x, true);
	request(&x, 20);
	reply(&x, &b, 0x00, 0x01, 0x00, 0x00, 0x00);
	CHECK(!hidpp_request(&x, DEVICE, 30, 1000, &m));
	CHECK_EQ(hidpp_wheel_steps(&x), 1);
}

static void check_wheel_event(struct hidpp *x, enum history_wheel outcome, uint8_t steps)
{
	struct hidpp_event event;

	CHECK(hidpp_take_event(x, &event));
	CHECK_EQ(event.type, HISTORY_WHEEL_SET);
	CHECK_EQ(event.a, outcome);
	CHECK_EQ(event.b, steps);
	CHECK(!hidpp_take_event(x, &event));
}

static void test_a_wheel_set_to_high_resolution_is_reported_with_its_steps(void)
{
	struct hidpp x;
	struct device_battery b;

	mouse_with_wheel(&x, &b);
	hidpp_want_hires(&x, true);
	request(&x, 40);
	reply(&x, &b, WHEEL_AT, 0x21, 0x02, 0x00, 0x00);
	check_wheel_event(&x, HISTORY_WHEEL_HIRES, 8);
}

static void test_a_wheel_set_to_notches_is_reported(void)
{
	struct hidpp x;
	struct device_battery b;

	mouse_with_wheel(&x, &b);
	request(&x, 40);
	reply(&x, &b, WHEEL_AT, 0x21, 0x00, 0x00, 0x00);
	check_wheel_event(&x, HISTORY_WHEEL_NOTCHES, 0);
}

static void test_a_mouse_without_the_wheel_feature_reports_it_at_each_connect(void)
{
	struct hidpp x;
	struct device_battery b;

	mouse_past_battery(&x, &b);
	request(&x, 20);
	reply(&x, &b, 0x00, 0x01, 0x00, 0x00, 0x00);
	check_wheel_event(&x, HISTORY_WHEEL_NO_FEATURE, 0);
	hidpp_start(&x);
	check_wheel_event(&x, HISTORY_WHEEL_NO_FEATURE, 0);
}

static void test_an_unanswered_wheel_setting_is_reported(void)
{
	struct hidpp x;
	struct device_battery b;
	struct hidpp_msg m;

	mouse_with_wheel(&x, &b);
	for (int i = 0; i < HIDPP_TRIES; i++) {
		request(&x, 40 + 2000 * (uint64_t)i);
	}
	CHECK(!hidpp_request(&x, DEVICE, 40 + 2000 * HIDPP_TRIES, 1000, &m));
	check_wheel_event(&x, HISTORY_WHEEL_UNANSWERED, 0);
}

static void test_a_refused_wheel_setting_is_reported(void)
{
	struct hidpp x;
	struct device_battery b;

	mouse_with_wheel(&x, &b);
	request(&x, 40);
	reply(&x, &b, 0xff, WHEEL_AT, 0x21, 0x02, 0x00);
	check_wheel_event(&x, HISTORY_WHEEL_REFUSED, 0);
}

int main(void)
{
	RUN(test_a_wheel_set_to_high_resolution_is_reported_with_its_steps);
	RUN(test_a_wheel_set_to_notches_is_reported);
	RUN(test_a_mouse_without_the_wheel_feature_reports_it_at_each_connect);
	RUN(test_an_unanswered_wheel_setting_is_reported);
	RUN(test_a_refused_wheel_setting_is_reported);
	RUN(test_a_mouse_is_asked_for_its_wheel_instead_of_a_divert);
	RUN(test_the_wheel_capability_is_read_after_the_lookup);
	RUN(test_a_wheel_the_host_wants_in_notches_is_set_to_notches);
	RUN(test_a_host_asking_for_high_resolution_sets_the_wheel_to_it);
	RUN(test_a_wanted_high_resolution_is_set_at_the_first_chance);
	RUN(test_a_reconnect_reads_the_battery_and_sets_the_wheel_mode_again);
	RUN(test_a_mouse_without_the_wheel_feature_stays_in_notches);
	RUN(test_a_device_without_battery_status_is_asked_for_unified_battery);
	RUN(test_unified_battery_is_read_with_its_get_status_function);
	RUN(test_a_unified_battery_reply_gives_the_level_and_charging);
	RUN(test_a_unified_battery_event_updates_the_battery);
	RUN(test_a_device_with_neither_battery_feature_asks_nothing_more);
	return TEST_EXIT();
}
