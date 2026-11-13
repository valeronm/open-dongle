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
	pairwin_paired(&w);
	CHECK(!pairwin_is_open(&w, 10 * PAIRWIN_MS));
}

static void test_a_pairing_closes_a_timed_window(void)
{
	struct pairwin w = {0};

	pairwin_open(&w, PAIRWIN_REQUEST, true, 0);
	pairwin_paired(&w);
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
	pairwin_paired(&w);
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
	return TEST_EXIT();
}
