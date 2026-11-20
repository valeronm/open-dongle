#include "test.h"
#include "pairwin.h"

static void test_a_window_is_closed_until_opened(void)
{
	struct pairwin w = {0};

	CHECK(!pairwin_is_open(&w, 0));
	CHECK(!pairwin_is_open(&w, 100000));
}

static void test_a_window_runs_for_30_s(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 5000);
	CHECK(pairwin_is_open(&w, 5000));
	CHECK(pairwin_is_open(&w, 5000 + PAIRWIN_MS - 1));
	CHECK(!pairwin_is_open(&w, 5000 + PAIRWIN_MS));
}

static void test_a_window_runs_across_a_clock_wrap(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, UINT32_MAX - 1000);
	CHECK(pairwin_is_open(&w, 1000));
	CHECK(!pairwin_is_open(&w, PAIRWIN_MS));
}

static void test_opening_again_restarts_the_30_s(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	pairwin_open(&w, PAIRWIN_REQUEST, true, 20000);
	CHECK(pairwin_is_open(&w, 20000 + PAIRWIN_MS - 1));
}

static void test_with_nothing_paired_it_stays_open_until_a_pairing(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_BOOT, false, 0);
	CHECK(pairwin_is_open(&w, 10 * PAIRWIN_MS));
	pairwin_close(&w);
	CHECK(!pairwin_is_open(&w, 10 * PAIRWIN_MS));
}

static void test_a_pairing_closes_a_timed_window(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	pairwin_close(&w);
	CHECK(!pairwin_is_open(&w, 1));
}

static void test_the_keyboard_connecting_closes_a_boot_window(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_BOOT, true, 0);
	pairwin_link_up(&w);
	CHECK(!pairwin_is_open(&w, 1));
}

static void test_the_keyboard_connecting_leaves_a_requested_window_open(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	pairwin_link_up(&w);
	CHECK(pairwin_is_open(&w, 1));
}

static void test_a_timed_window_running_out_is_reported_once(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	CHECK(!pairwin_ran_out(&w, PAIRWIN_MS - 1));
	CHECK(pairwin_ran_out(&w, PAIRWIN_MS));
	CHECK(!pairwin_ran_out(&w, PAIRWIN_MS + 1));
}

static void test_a_window_closed_early_does_not_run_out(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_BOOT, true, 0);
	pairwin_link_up(&w);
	CHECK(!pairwin_ran_out(&w, PAIRWIN_MS));
	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	pairwin_close(&w);
	CHECK(!pairwin_ran_out(&w, PAIRWIN_MS));
}

static void test_a_window_until_paired_does_not_run_out(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_BOOT, false, 0);
	CHECK(!pairwin_ran_out(&w, 10 * PAIRWIN_MS));
}

static void test_the_keyboard_connecting_leaves_a_window_open_until_paired(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_BOOT, false, 0);
	pairwin_link_up(&w);
	CHECK(pairwin_is_open(&w, 1));
}

static void test_a_timed_window_counts_down_its_time_left(void)
{
	struct pairwin w = {0};

	CHECK_EQ(pairwin_left_ms(&w, 0), 0);
	pairwin_open(&w, PAIRWIN_REQUEST, true, 1000);
	CHECK_EQ(pairwin_left_ms(&w, 1000), PAIRWIN_MS);
	CHECK_EQ(pairwin_left_ms(&w, 1000 + PAIRWIN_MS - 1), 1);
	CHECK_EQ(pairwin_left_ms(&w, 1000 + PAIRWIN_MS), 0);
	pairwin_open(&w, PAIRWIN_REQUEST, false, 0);
	CHECK_EQ(pairwin_left_ms(&w, 0), 0);
}

static void test_a_held_window_stays_open_past_its_30_s(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	CHECK(pairwin_hold(&w, DEVICE_KIND_KEYBOARD, 0));
	CHECK(pairwin_is_open(&w, 10 * PAIRWIN_MS));
	CHECK_EQ(pairwin_holder(&w), DEVICE_KIND_KEYBOARD);
	CHECK(!pairwin_ran_out(&w, 10 * PAIRWIN_MS));
	CHECK_EQ(pairwin_left_ms(&w, 0), 0);
}

static void test_the_holders_attempt_ending_closes_the_window_without_running_out(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	pairwin_hold(&w, DEVICE_KIND_KEYBOARD, 0);
	CHECK(pairwin_end_attempt(&w, DEVICE_KIND_KEYBOARD));
	CHECK(!pairwin_is_open(&w, 1));
	CHECK_EQ(pairwin_holder(&w), DEVICE_NO_KIND);
	CHECK(!pairwin_ran_out(&w, PAIRWIN_MS));
	CHECK(!pairwin_end_attempt(&w, DEVICE_KIND_KEYBOARD));
}

static void test_another_devices_attempt_ending_leaves_the_hold(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	CHECK(!pairwin_end_attempt(&w, DEVICE_KIND_KEYBOARD));
	CHECK(pairwin_is_open(&w, 1));
	pairwin_hold(&w, DEVICE_KIND_KEYBOARD, 0);
	CHECK(!pairwin_end_attempt(&w, DEVICE_KIND_MOUSE));
	CHECK_EQ(pairwin_holder(&w), DEVICE_KIND_KEYBOARD);
}

static void test_closing_ends_a_held_window(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	pairwin_hold(&w, DEVICE_KIND_KEYBOARD, 0);
	pairwin_close(&w);
	CHECK(!pairwin_is_open(&w, 1));
	CHECK_EQ(pairwin_holder(&w), DEVICE_NO_KIND);
}

static void test_opening_again_drops_the_hold(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	pairwin_hold(&w, DEVICE_KIND_KEYBOARD, 0);
	pairwin_open(&w, PAIRWIN_REQUEST, true, 1000);
	CHECK_EQ(pairwin_holder(&w), DEVICE_NO_KIND);
	CHECK(!pairwin_is_open(&w, 1000 + PAIRWIN_MS));
}

static void test_holding_a_closed_window_leaves_it_closed(void)
{
	struct pairwin w = {0};

	CHECK(!pairwin_hold(&w, DEVICE_KIND_KEYBOARD, 0));
	CHECK(!pairwin_is_open(&w, 0));
	CHECK_EQ(pairwin_holder(&w), DEVICE_NO_KIND);
	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	CHECK(!pairwin_hold(&w, DEVICE_KIND_KEYBOARD, PAIRWIN_MS));
	CHECK(!pairwin_is_open(&w, PAIRWIN_MS));
	CHECK(pairwin_ran_out(&w, PAIRWIN_MS));
}

int main(void)
{
	RUN(test_a_window_is_closed_until_opened);
	RUN(test_a_window_runs_for_30_s);
	RUN(test_a_window_runs_across_a_clock_wrap);
	RUN(test_opening_again_restarts_the_30_s);
	RUN(test_with_nothing_paired_it_stays_open_until_a_pairing);
	RUN(test_a_pairing_closes_a_timed_window);
	RUN(test_the_keyboard_connecting_closes_a_boot_window);
	RUN(test_the_keyboard_connecting_leaves_a_requested_window_open);
	RUN(test_a_timed_window_running_out_is_reported_once);
	RUN(test_a_window_closed_early_does_not_run_out);
	RUN(test_a_window_until_paired_does_not_run_out);
	RUN(test_the_keyboard_connecting_leaves_a_window_open_until_paired);
	RUN(test_a_timed_window_counts_down_its_time_left);
	RUN(test_a_held_window_stays_open_past_its_30_s);
	RUN(test_the_holders_attempt_ending_closes_the_window_without_running_out);
	RUN(test_another_devices_attempt_ending_leaves_the_hold);
	RUN(test_closing_ends_a_held_window);
	RUN(test_opening_again_drops_the_hold);
	RUN(test_holding_a_closed_window_leaves_it_closed);
	return TEST_EXIT();
}
