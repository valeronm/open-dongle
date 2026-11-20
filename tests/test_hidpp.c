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
	*x = (struct hidpp){0};
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

int main(void)
{
	RUN(test_a_device_without_battery_status_is_asked_for_unified_battery);
	RUN(test_unified_battery_is_read_with_its_get_status_function);
	RUN(test_a_unified_battery_reply_gives_the_level_and_charging);
	RUN(test_a_unified_battery_event_updates_the_battery);
	RUN(test_a_device_with_neither_battery_feature_asks_nothing_more);
	return TEST_EXIT();
}
