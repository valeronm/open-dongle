#include "btpolicy.h"
#include "test.h"

static struct btpolicy policy;

static struct btpolicy_advert stored_advert(uint8_t last)
{
	return (struct btpolicy_advert){
		.addr = {1, 1, 2, 3, 4, 5, last},
		.connectable = true,
		.stored = true,
	};
}

static struct btpolicy_advert new_advert(uint8_t last)
{
	return (struct btpolicy_advert){
		.addr = {1, 9, 9, 9, 9, 9, last},
		.connectable = true,
		.hid_service = true,
	};
}

static void a_stored_keyboard_is_wanted_whatever_its_advert_carries(void)
{
	struct btpolicy_advert a = stored_advert(1);

	btpolicy_init(&policy);
	CHECK(btpolicy_wants(&policy, &a, false, 1, 0));
	a.directed = true;
	CHECK(btpolicy_wants(&policy, &a, false, 1, 0));
	a.connectable = false;
	CHECK(!btpolicy_wants(&policy, &a, false, 1, 0));
}

static void an_unknown_keyboard_needs_an_open_window(void)
{
	struct btpolicy_advert a = new_advert(1);

	btpolicy_init(&policy);
	CHECK(!btpolicy_wants(&policy, &a, false, 0, 0));
	CHECK(btpolicy_wants(&policy, &a, true, 0, 0));
}

static void an_unknown_keyboard_needs_the_hid_service_undirected(void)
{
	struct btpolicy_advert a = new_advert(1);

	btpolicy_init(&policy);
	CHECK(btpolicy_wants(&policy, &a, true, 0, 0));
	a.hid_service = false;
	CHECK(!btpolicy_wants(&policy, &a, true, 0, 0));
	a.hid_service = true;
	a.directed = true;
	CHECK(!btpolicy_wants(&policy, &a, true, 0, 0));
	a.directed = false;
	a.connectable = false;
	CHECK(!btpolicy_wants(&policy, &a, true, 0, 0));
}

static void a_full_list_takes_no_new_keyboard(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	CHECK(btpolicy_wants(&policy, &fresh, true, BTPOLICY_STORED_MAX - 1, 0));
	CHECK(!btpolicy_wants(&policy, &fresh, true, BTPOLICY_STORED_MAX, 0));
	CHECK(btpolicy_wants(&policy, &known, true, BTPOLICY_STORED_MAX, 0));
}

static void nothing_is_wanted_while_connecting_or_connected(void)
{
	struct btpolicy_advert first = stored_advert(1);
	struct btpolicy_advert second = stored_advert(2);

	btpolicy_init(&policy);
	CHECK(btpolicy_scanning(&policy));
	btpolicy_connecting(&policy, &first);
	CHECK(!btpolicy_scanning(&policy));
	CHECK(!btpolicy_wants(&policy, &second, true, 2, 0));
	btpolicy_connected(&policy);
	CHECK(!btpolicy_scanning(&policy));
	CHECK(!btpolicy_wants(&policy, &second, true, 2, 0));
	btpolicy_ready(&policy);
	CHECK(btpolicy_is_ready(&policy));
	CHECK(!btpolicy_wants(&policy, &second, true, 2, 0));
	btpolicy_disconnected(&policy, 500);
	CHECK(!btpolicy_is_ready(&policy));
	CHECK(btpolicy_scanning(&policy));
	CHECK(btpolicy_wants(&policy, &second, false, 2, 500));
}

static void a_failed_stored_keyboard_waits_before_the_next_attempt(void)
{
	struct btpolicy_advert known = stored_advert(1);
	struct btpolicy_advert other = stored_advert(2);
	struct btpolicy_advert fresh = new_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	btpolicy_disconnected(&policy, 10000);
	CHECK(!btpolicy_wants(&policy, &known, false, 2, 10000 + BTPOLICY_RETRY_MS - 1));
	CHECK(!btpolicy_wants(&policy, &other, false, 2, 10000 + BTPOLICY_RETRY_MS - 1));
	CHECK(btpolicy_wants(&policy, &fresh, true, 2, 10000));
	CHECK(btpolicy_wants(&policy, &known, false, 2, 10000 + BTPOLICY_RETRY_MS));
}

static void a_connection_that_ended_normally_is_retried_at_once(void)
{
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	btpolicy_connected(&policy);
	btpolicy_ready(&policy);
	btpolicy_disconnected(&policy, 10000);
	CHECK(btpolicy_wants(&policy, &known, false, 1, 10000));
}

static void a_failed_new_keyboard_sets_no_wait(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &fresh);
	btpolicy_disconnected(&policy, 10000);
	CHECK(btpolicy_wants(&policy, &known, true, 1, 10000));
	CHECK(btpolicy_wants(&policy, &fresh, true, 1, 10000));
}

static void a_keyboard_paired_in_this_connection_waits_after_a_failure(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &fresh);
	btpolicy_paired(&policy);
	btpolicy_disconnected(&policy, 10000);
	CHECK(!btpolicy_wants(&policy, &known, false, 1, 10000));
	CHECK(btpolicy_wants(&policy, &known, false, 1, 10000 + BTPOLICY_RETRY_MS));
}

static void the_wait_holds_across_the_clock_wrap(void)
{
	struct btpolicy_advert known = stored_advert(1);
	uint32_t before_wrap = UINT32_MAX - 100;

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	btpolicy_disconnected(&policy, before_wrap);
	CHECK(!btpolicy_wants(&policy, &known, false, 1, before_wrap + 500));
	CHECK(btpolicy_wants(&policy, &known, false, 1, before_wrap + BTPOLICY_RETRY_MS));
}

static void a_refused_keyboard_is_ignored_until_the_window_closes(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert another = new_advert(2);

	btpolicy_init(&policy);
	btpolicy_window(&policy, true);
	btpolicy_connecting(&policy, &fresh);
	CHECK_EQ(btpolicy_security_failed(&policy, false), BTPOLICY_PAIRING_REFUSED);
	CHECK_EQ(btpolicy_security_failed(&policy, false), BTPOLICY_REFUSED_BEFORE);
	btpolicy_disconnected(&policy, 100);
	CHECK(!btpolicy_wants(&policy, &fresh, true, 0, 100));
	CHECK(btpolicy_wants(&policy, &another, true, 0, 100));
	btpolicy_window(&policy, true);
	CHECK(!btpolicy_wants(&policy, &fresh, true, 0, 100));
	btpolicy_window(&policy, false);
	btpolicy_window(&policy, true);
	CHECK(btpolicy_wants(&policy, &fresh, true, 0, 100));
}

static void more_refusals_than_fit_are_still_counted_once_each(void)
{
	btpolicy_init(&policy);
	for (uint8_t i = 0; i < BTPOLICY_STORED_MAX + 2; i++) {
		struct btpolicy_advert a = new_advert(i);

		btpolicy_connecting(&policy, &a);
		CHECK_EQ(btpolicy_security_failed(&policy, false), BTPOLICY_PAIRING_REFUSED);
		btpolicy_disconnected(&policy, 0);
	}
	CHECK_EQ(policy.refused_count, BTPOLICY_STORED_MAX);
}

static void a_stored_keyboard_without_its_key_lost_its_bond(void)
{
	struct btpolicy_advert known = stored_advert(1);
	struct btpolicy_advert fresh = new_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	CHECK_EQ(btpolicy_security_failed(&policy, true), BTPOLICY_BOND_LOST);
	CHECK_EQ(btpolicy_security_failed(&policy, false), BTPOLICY_SECURITY_FAILED);
	btpolicy_disconnected(&policy, 0);
	btpolicy_connecting(&policy, &fresh);
	btpolicy_paired(&policy);
	CHECK_EQ(btpolicy_security_failed(&policy, false), BTPOLICY_SECURITY_FAILED);
}

static void only_connectable_stored_or_hid_adverts_are_of_interest(void)
{
	struct btpolicy_advert a = new_advert(1);

	CHECK(btpolicy_interesting(&a));
	a.hid_service = false;
	CHECK(!btpolicy_interesting(&a));
	a.stored = true;
	CHECK(btpolicy_interesting(&a));
	a.connectable = false;
	CHECK(!btpolicy_interesting(&a));
}

int main(void)
{
	RUN(a_stored_keyboard_is_wanted_whatever_its_advert_carries);
	RUN(an_unknown_keyboard_needs_an_open_window);
	RUN(an_unknown_keyboard_needs_the_hid_service_undirected);
	RUN(a_full_list_takes_no_new_keyboard);
	RUN(nothing_is_wanted_while_connecting_or_connected);
	RUN(a_failed_stored_keyboard_waits_before_the_next_attempt);
	RUN(a_connection_that_ended_normally_is_retried_at_once);
	RUN(a_failed_new_keyboard_sets_no_wait);
	RUN(a_keyboard_paired_in_this_connection_waits_after_a_failure);
	RUN(the_wait_holds_across_the_clock_wrap);
	RUN(a_refused_keyboard_is_ignored_until_the_window_closes);
	RUN(more_refusals_than_fit_are_still_counted_once_each);
	RUN(a_stored_keyboard_without_its_key_lost_its_bond);
	RUN(only_connectable_stored_or_hid_adverts_are_of_interest);
	return TEST_EXIT();
}
