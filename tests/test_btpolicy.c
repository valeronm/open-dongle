#include "btpolicy.h"
#include "test.h"

static struct btpolicy policy;

static struct btpolicy_advert stored_advert(uint8_t last)
{
	return (struct btpolicy_advert){
		.addr = {1, 1, 2, 3, 4, 5, last},
		.connectable = true,
		.stored = true,
		.kind = BTPOLICY_KEYBOARD,
	};
}

static struct btpolicy_advert new_advert(uint8_t last)
{
	return (struct btpolicy_advert){
		.addr = {1, 9, 9, 9, 9, 9, last},
		.connectable = true,
		.hid_service = true,
		.kind = BTPOLICY_KEYBOARD,
	};
}

static void a_stored_keyboard_is_wanted_whatever_its_advert_carries(void)
{
	struct btpolicy_advert a = stored_advert(1);

	btpolicy_init(&policy);
	CHECK_EQ(btpolicy_wants(&policy, &a, false, 1, 0), BTPOLICY_CONNECT);
	a.directed = true;
	CHECK_EQ(btpolicy_wants(&policy, &a, false, 1, 0), BTPOLICY_CONNECT);
	a.connectable = false;
	CHECK_EQ(btpolicy_wants(&policy, &a, false, 1, 0), BTPOLICY_IGNORE);
}

static void a_stored_keyboard_waits_while_a_window_is_open(void)
{
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	CHECK_EQ(btpolicy_wants(&policy, &known, true, 1, 0), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 0), BTPOLICY_CONNECT);
}

static void an_unknown_keyboard_needs_an_open_window(void)
{
	struct btpolicy_advert a = new_advert(1);

	btpolicy_init(&policy);
	CHECK_EQ(btpolicy_wants(&policy, &a, false, 0, 0), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &a, true, 0, 0), BTPOLICY_CONNECT);
}

static void an_unknown_keyboard_needs_the_hid_service_undirected(void)
{
	struct btpolicy_advert a = new_advert(1);

	btpolicy_init(&policy);
	CHECK_EQ(btpolicy_wants(&policy, &a, true, 0, 0), BTPOLICY_CONNECT);
	a.hid_service = false;
	CHECK_EQ(btpolicy_wants(&policy, &a, true, 0, 0), BTPOLICY_IGNORE);
	a.hid_service = true;
	a.directed = true;
	CHECK_EQ(btpolicy_wants(&policy, &a, true, 0, 0), BTPOLICY_IGNORE);
	a.directed = false;
	a.connectable = false;
	CHECK_EQ(btpolicy_wants(&policy, &a, true, 0, 0), BTPOLICY_IGNORE);
}

static void a_full_list_takes_no_new_keyboard(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, BTPOLICY_STORED_MAX - 1, 0), BTPOLICY_CONNECT);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, BTPOLICY_STORED_MAX, 0), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, BTPOLICY_STORED_MAX, 0), BTPOLICY_CONNECT);
}

static void a_ready_keyboard_is_kept_from_stored_ones_and_replaced_by_a_new_one(void)
{
	struct btpolicy_advert first = stored_advert(1);
	struct btpolicy_advert second = stored_advert(2);
	struct btpolicy_advert fresh = new_advert(3);

	btpolicy_init(&policy);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_PASSIVE);
	CHECK(btpolicy_peer(&policy, BTPOLICY_KEYBOARD) == NULL);
	btpolicy_connecting(&policy, &first);
	CHECK_MEM(btpolicy_peer(&policy, BTPOLICY_KEYBOARD), first.addr, BTPOLICY_ADDR_LEN);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_OFF);
	CHECK_EQ(btpolicy_wants(&policy, &second, false, 2, 0), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 2, 0), BTPOLICY_IGNORE);
	btpolicy_connected(&policy, BTPOLICY_KEYBOARD);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_OFF);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 2, 0), BTPOLICY_IGNORE);
	btpolicy_ready(&policy, BTPOLICY_KEYBOARD);
	CHECK(btpolicy_is_ready(&policy, BTPOLICY_KEYBOARD));
	CHECK_EQ(btpolicy_wants(&policy, &second, false, 2, 0), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 2, 0), BTPOLICY_REPLACE);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 500, false);
	CHECK(!btpolicy_is_ready(&policy, BTPOLICY_KEYBOARD));
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_PASSIVE);
	CHECK_EQ(btpolicy_wants(&policy, &second, false, 2, 500), BTPOLICY_CONNECT);
}

static void a_failed_stored_keyboard_waits_before_the_next_attempt(void)
{
	struct btpolicy_advert known = stored_advert(1);
	struct btpolicy_advert other = stored_advert(2);
	struct btpolicy_advert fresh = new_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, false);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 2, 10000 + BTPOLICY_RETRY_MS - 1), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &other, false, 2, 10000 + BTPOLICY_RETRY_MS - 1), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 2, 10000), BTPOLICY_CONNECT);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 2, 10000 + BTPOLICY_RETRY_MS), BTPOLICY_CONNECT);
}

static void a_connection_that_ended_normally_is_retried_at_once(void)
{
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	btpolicy_connected(&policy, BTPOLICY_KEYBOARD);
	btpolicy_ready(&policy, BTPOLICY_KEYBOARD);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, false);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000), BTPOLICY_CONNECT);
}

static void a_stored_keyboard_that_never_joined_is_retried_at_once(void)
{
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	btpolicy_connected(&policy, BTPOLICY_KEYBOARD);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, true);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000), BTPOLICY_CONNECT);
}

static void a_keyboard_that_keeps_not_joining_waits_after_the_quick_retries(void)
{
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	for (int i = 0; i < BTPOLICY_QUICK_RETRIES; i++) {
		btpolicy_connecting(&policy, &known);
		btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, true);
		CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000), BTPOLICY_CONNECT);
	}
	btpolicy_connecting(&policy, &known);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, true);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000 + BTPOLICY_RETRY_MS - 1), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000 + BTPOLICY_RETRY_MS), BTPOLICY_CONNECT);
	btpolicy_connecting(&policy, &known);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 20000, true);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 20000), BTPOLICY_CONNECT);
}

static void a_keyboard_that_got_ready_has_its_quick_retries_again(void)
{
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	for (int i = 0; i < BTPOLICY_QUICK_RETRIES; i++) {
		btpolicy_connecting(&policy, &known);
		btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, true);
	}
	btpolicy_connecting(&policy, &known);
	btpolicy_connected(&policy, BTPOLICY_KEYBOARD);
	btpolicy_ready(&policy, BTPOLICY_KEYBOARD);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, false);
	btpolicy_connecting(&policy, &known);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, true);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000), BTPOLICY_CONNECT);
}

static void a_failed_new_keyboard_sets_no_wait(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &fresh);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, false);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000), BTPOLICY_CONNECT);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 1, 10000), BTPOLICY_CONNECT);
}

static void a_keyboard_paired_in_this_connection_waits_after_a_failure(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &fresh);
	CHECK_EQ(btpolicy_paired(&policy, BTPOLICY_KEYBOARD, false), BTPOLICY_PAIRING_KEPT);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, false);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000 + BTPOLICY_RETRY_MS), BTPOLICY_CONNECT);
}

static void the_wait_holds_across_the_clock_wrap(void)
{
	struct btpolicy_advert known = stored_advert(1);
	uint32_t before_wrap = UINT32_MAX - 100;

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, before_wrap, false);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, before_wrap + 500), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, before_wrap + BTPOLICY_RETRY_MS), BTPOLICY_CONNECT);
}

static void a_refused_keyboard_is_ignored_until_the_window_closes(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert another = new_advert(2);

	btpolicy_init(&policy);
	btpolicy_window(&policy, true, false);
	btpolicy_connecting(&policy, &fresh);
	CHECK_EQ(btpolicy_security_failed(&policy, BTPOLICY_KEYBOARD, BTPOLICY_FAILURE_REFUSAL), BTPOLICY_PAIRING_REFUSED);
	CHECK_EQ(btpolicy_security_failed(&policy, BTPOLICY_KEYBOARD, BTPOLICY_FAILURE_REFUSAL), BTPOLICY_REFUSED_BEFORE);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 100, false);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 0, 100), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &another, true, 0, 100), BTPOLICY_CONNECT);
	btpolicy_window(&policy, true, false);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 0, 100), BTPOLICY_IGNORE);
	btpolicy_window(&policy, false, false);
	btpolicy_window(&policy, true, false);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 0, 100), BTPOLICY_CONNECT);
}

static void more_refusals_than_fit_are_still_counted_once_each(void)
{
	btpolicy_init(&policy);
	for (uint8_t i = 0; i < BTPOLICY_STORED_MAX + 2; i++) {
		struct btpolicy_advert a = new_advert(i);

		btpolicy_connecting(&policy, &a);
		CHECK_EQ(btpolicy_security_failed(&policy, BTPOLICY_KEYBOARD, BTPOLICY_FAILURE_REFUSAL), BTPOLICY_PAIRING_REFUSED);
		btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 0, false);
	}
	CHECK_EQ(policy.refused_count, BTPOLICY_STORED_MAX);
}

static void a_pairing_cut_short_by_a_lost_link_is_not_a_refusal(void)
{
	struct btpolicy_advert fresh = new_advert(1);

	btpolicy_init(&policy);
	btpolicy_window(&policy, true, false);
	btpolicy_connecting(&policy, &fresh);
	CHECK_EQ(btpolicy_security_failed(&policy, BTPOLICY_KEYBOARD, BTPOLICY_FAILURE_LINK), BTPOLICY_SECURITY_FAILED);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 100, false);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 0, 100), BTPOLICY_CONNECT);
}

static void a_pairing_without_the_demanded_passkey_is_forgotten(void)
{
	struct btpolicy_advert fresh = new_advert(1);

	btpolicy_init(&policy);
	btpolicy_window(&policy, true, true);
	btpolicy_connecting(&policy, &fresh);
	CHECK_EQ(btpolicy_paired(&policy, BTPOLICY_KEYBOARD, false), BTPOLICY_PAIRING_FORGOTTEN);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 100, false);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 0, 100), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 0, 100 + BTPOLICY_RETRY_MS), BTPOLICY_IGNORE);
	btpolicy_window(&policy, false, false);
	btpolicy_window(&policy, true, true);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 0, 5000), BTPOLICY_CONNECT);
}

static void a_pairing_with_the_demanded_passkey_is_kept(void)
{
	struct btpolicy_advert fresh = new_advert(1);
	struct btpolicy_advert known = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_window(&policy, true, true);
	btpolicy_connecting(&policy, &fresh);
	CHECK_EQ(btpolicy_paired(&policy, BTPOLICY_KEYBOARD, true), BTPOLICY_PAIRING_KEPT);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, false);
	btpolicy_window(&policy, false, false);
	CHECK_EQ(btpolicy_wants(&policy, &known, false, 1, 10000), BTPOLICY_IGNORE);
}

static void a_stored_keyboard_without_its_key_lost_its_bond(void)
{
	struct btpolicy_advert known = stored_advert(1);
	struct btpolicy_advert fresh = new_advert(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &known);
	CHECK_EQ(btpolicy_security_failed(&policy, BTPOLICY_KEYBOARD, BTPOLICY_FAILURE_KEY_MISSING), BTPOLICY_BOND_LOST);
	CHECK_EQ(btpolicy_security_failed(&policy, BTPOLICY_KEYBOARD, BTPOLICY_FAILURE_REFUSAL), BTPOLICY_SECURITY_FAILED);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 0, false);
	btpolicy_connecting(&policy, &fresh);
	CHECK_EQ(btpolicy_paired(&policy, BTPOLICY_KEYBOARD, false), BTPOLICY_PAIRING_KEPT);
	CHECK_EQ(btpolicy_security_failed(&policy, BTPOLICY_KEYBOARD, BTPOLICY_FAILURE_REFUSAL), BTPOLICY_SECURITY_FAILED);
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

static struct btpolicy_advert new_mouse(uint8_t last)
{
	struct btpolicy_advert a = new_advert(last);

	a.addr[1] = 8;
	a.kind = BTPOLICY_MOUSE;
	return a;
}

static void ready_keyboard(const struct btpolicy_advert *a)
{
	btpolicy_connecting(&policy, a);
	btpolicy_connected(&policy, BTPOLICY_KEYBOARD);
	btpolicy_ready(&policy, BTPOLICY_KEYBOARD);
}

static void the_appearance_gives_the_kind(void)
{
	CHECK_EQ(btpolicy_kind_of(0x03c1), BTPOLICY_KEYBOARD);
	CHECK_EQ(btpolicy_kind_of(0x03c2), BTPOLICY_MOUSE);
	CHECK_EQ(btpolicy_kind_of(0x0000), BTPOLICY_NO_KIND);
	CHECK_EQ(btpolicy_kind_of(0x03c0), BTPOLICY_NO_KIND);
	CHECK_EQ(btpolicy_kind_of(0x03c4), BTPOLICY_NO_KIND);
}

static void a_new_device_of_no_kind_is_refused(void)
{
	struct btpolicy_advert a = new_advert(1);

	a.kind = BTPOLICY_NO_KIND;
	btpolicy_init(&policy);
	CHECK(!btpolicy_interesting(&a));
	CHECK_EQ(btpolicy_wants(&policy, &a, true, 0, 0), BTPOLICY_IGNORE);
	a.stored = true;
	CHECK(btpolicy_interesting(&a));
	CHECK_EQ(btpolicy_wants(&policy, &a, false, 1, 0), BTPOLICY_IGNORE);
}

static void a_mouse_connects_beside_a_ready_keyboard(void)
{
	struct btpolicy_advert keyboard = stored_advert(1);
	struct btpolicy_advert mouse = new_mouse(1);

	btpolicy_init(&policy);
	ready_keyboard(&keyboard);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_PASSIVE);
	CHECK_EQ(btpolicy_wants(&policy, &mouse, true, 1, 0), BTPOLICY_CONNECT);
	btpolicy_connecting(&policy, &mouse);
	CHECK_MEM(btpolicy_peer(&policy, BTPOLICY_MOUSE), mouse.addr, BTPOLICY_ADDR_LEN);
	CHECK_MEM(btpolicy_peer(&policy, BTPOLICY_KEYBOARD), keyboard.addr, BTPOLICY_ADDR_LEN);
	CHECK(btpolicy_is_ready(&policy, BTPOLICY_KEYBOARD));
	CHECK(!btpolicy_is_ready(&policy, BTPOLICY_MOUSE));
}

static void one_device_is_set_up_at_a_time(void)
{
	struct btpolicy_advert keyboard = stored_advert(1);
	struct btpolicy_advert mouse = new_mouse(1);

	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &keyboard);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_OFF);
	CHECK_EQ(btpolicy_wants(&policy, &mouse, true, 1, 0), BTPOLICY_IGNORE);
	btpolicy_connected(&policy, BTPOLICY_KEYBOARD);
	CHECK_EQ(btpolicy_wants(&policy, &mouse, true, 1, 0), BTPOLICY_IGNORE);
	btpolicy_ready(&policy, BTPOLICY_KEYBOARD);
	CHECK_EQ(btpolicy_wants(&policy, &mouse, true, 1, 0), BTPOLICY_CONNECT);
}

static void a_slot_being_dropped_wants_nothing_until_it_is_idle(void)
{
	struct btpolicy_advert keyboard = stored_advert(1);
	struct btpolicy_advert fresh = new_advert(2);

	btpolicy_init(&policy);
	ready_keyboard(&keyboard);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 1, 0), BTPOLICY_REPLACE);
	btpolicy_dropping(&policy, BTPOLICY_KEYBOARD);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 1, 0), BTPOLICY_IGNORE);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 100, false);
	CHECK_EQ(btpolicy_wants(&policy, &fresh, true, 1, 100), BTPOLICY_CONNECT);
}

static void both_slots_ready_scan_only_while_the_window_is_open(void)
{
	struct btpolicy_advert keyboard = stored_advert(1);
	struct btpolicy_advert mouse = new_mouse(1);

	btpolicy_init(&policy);
	ready_keyboard(&keyboard);
	btpolicy_connecting(&policy, &mouse);
	btpolicy_connected(&policy, BTPOLICY_MOUSE);
	btpolicy_ready(&policy, BTPOLICY_MOUSE);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_OFF);
	btpolicy_window(&policy, true, false);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_ACTIVE);
}

static void a_mouse_is_kept_in_a_window_that_demands_a_passkey(void)
{
	struct btpolicy_advert mouse = new_mouse(1);

	btpolicy_init(&policy);
	btpolicy_window(&policy, true, true);
	btpolicy_connecting(&policy, &mouse);
	CHECK_EQ(btpolicy_paired(&policy, BTPOLICY_MOUSE, false), BTPOLICY_PAIRING_KEPT);
}

static void a_failed_keyboard_leaves_the_mouse_slot_free_to_connect(void)
{
	struct btpolicy_advert keyboard = stored_advert(1);
	struct btpolicy_advert mouse = stored_advert(2);

	mouse.kind = BTPOLICY_MOUSE;
	btpolicy_init(&policy);
	btpolicy_connecting(&policy, &keyboard);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 10000, false);
	CHECK_EQ(btpolicy_wants(&policy, &keyboard, false, 2, 10000), BTPOLICY_IGNORE);
	CHECK_EQ(btpolicy_wants(&policy, &mouse, false, 2, 10000), BTPOLICY_CONNECT);
}

static void a_pairing_window_scans_actively_for_scan_responses(void)
{
	struct btpolicy_advert keyboard = stored_advert(1);

	btpolicy_init(&policy);
	btpolicy_window(&policy, true, false);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_ACTIVE);
	btpolicy_connecting(&policy, &keyboard);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_OFF);
	btpolicy_disconnected(&policy, BTPOLICY_KEYBOARD, 0, false);
	btpolicy_window(&policy, false, false);
	CHECK_EQ(btpolicy_scan(&policy), BTPOLICY_SCAN_PASSIVE);
}

int main(void)
{
	RUN(a_stored_keyboard_is_wanted_whatever_its_advert_carries);
	RUN(a_stored_keyboard_waits_while_a_window_is_open);
	RUN(an_unknown_keyboard_needs_an_open_window);
	RUN(an_unknown_keyboard_needs_the_hid_service_undirected);
	RUN(a_full_list_takes_no_new_keyboard);
	RUN(a_ready_keyboard_is_kept_from_stored_ones_and_replaced_by_a_new_one);
	RUN(a_failed_stored_keyboard_waits_before_the_next_attempt);
	RUN(a_connection_that_ended_normally_is_retried_at_once);
	RUN(a_stored_keyboard_that_never_joined_is_retried_at_once);
	RUN(a_keyboard_that_keeps_not_joining_waits_after_the_quick_retries);
	RUN(a_keyboard_that_got_ready_has_its_quick_retries_again);
	RUN(a_failed_new_keyboard_sets_no_wait);
	RUN(a_keyboard_paired_in_this_connection_waits_after_a_failure);
	RUN(the_wait_holds_across_the_clock_wrap);
	RUN(a_refused_keyboard_is_ignored_until_the_window_closes);
	RUN(more_refusals_than_fit_are_still_counted_once_each);
	RUN(a_pairing_cut_short_by_a_lost_link_is_not_a_refusal);
	RUN(a_pairing_without_the_demanded_passkey_is_forgotten);
	RUN(a_pairing_with_the_demanded_passkey_is_kept);
	RUN(a_stored_keyboard_without_its_key_lost_its_bond);
	RUN(only_connectable_stored_or_hid_adverts_are_of_interest);
	RUN(the_appearance_gives_the_kind);
	RUN(a_new_device_of_no_kind_is_refused);
	RUN(a_mouse_connects_beside_a_ready_keyboard);
	RUN(one_device_is_set_up_at_a_time);
	RUN(a_slot_being_dropped_wants_nothing_until_it_is_idle);
	RUN(both_slots_ready_scan_only_while_the_window_is_open);
	RUN(a_mouse_is_kept_in_a_window_that_demands_a_passkey);
	RUN(a_failed_keyboard_leaves_the_mouse_slot_free_to_connect);
	RUN(a_pairing_window_scans_actively_for_scan_responses);
	return TEST_EXIT();
}
