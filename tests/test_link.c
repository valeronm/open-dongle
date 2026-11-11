#include <string.h>
#include "test.h"
#include "link.h"

static const uint8_t dongle_id[4] = {0x26, 0xc1, 0x21, 0x59};
static const uint8_t keyboard_id[4] = {0x70, 0x15, 0xa3, 0x29};
static const uint8_t previous_id[4] = {0x96, 0xbe, 0x5c, 0x89};
static const uint8_t home[3] = {26, 30, 34};
static const uint8_t surveyed[3] = {30, 40, 60};

static struct link l;
static struct link_out out;

static struct nuphy_msg pairing(enum nuphy_kind kind, const uint8_t *dongle)
{
	struct nuphy_msg m = {.kind = kind};

	memcpy(m.ids.keyboard, keyboard_id, 4);
	memcpy(m.ids.dongle, dongle, 4);
	return m;
}

static void init(const uint8_t *own)
{
	out = (struct link_out){0};
	link_init(&l, dongle_id, own, 0, &out);
}

static void deliver(const struct nuphy_msg *m, uint8_t pipe, uint32_t now)
{
	out = (struct link_out){0};
	link_on_message(&l, m, pipe, now, &out);
}

static void message(enum nuphy_kind kind, uint8_t pipe, uint32_t now)
{
	struct nuphy_msg m = {.kind = kind};

	deliver(&m, pipe, now);
}

static void survey(const uint8_t channels[3], uint32_t now)
{
	out = (struct link_out){0};
	link_on_survey(&l, channels, now, &out);
}

static void survey_failed(uint32_t now)
{
	out = (struct link_out){0};
	link_survey_failed(&l, now, &out);
}

static void tick(uint32_t now)
{
	out = (struct link_out){0};
	link_tick(&l, now, &out);
}

static void expect_status(uint8_t seq, uint8_t leds)
{
	uint8_t expected[NUPHY_FRAME_MAX];
	size_t len = nuphy_status(expected, seq, dongle_id, leds);

	CHECK(out.reply);
	CHECK_EQ(out.reply_pipe, 1);
	CHECK_EQ(out.reply_len, len);
	CHECK_MEM(out.reply_buf, expected, len);
}

static void expect_pair_reply_offering(uint8_t seq, const uint8_t channels[3])
{
	uint8_t expected[NUPHY_FRAME_MAX];
	size_t len = nuphy_pair_reply(expected, seq, dongle_id, keyboard_id, channels);

	CHECK(out.reply);
	CHECK_EQ(out.reply_pipe, 0);
	CHECK_EQ(out.reply_len, len);
	CHECK_MEM(out.reply_buf, expected, len);
}

static void expect_pair_reply(uint8_t seq)
{
	expect_pair_reply_offering(seq, surveyed);
}

static void expect_suspend(uint8_t seq)
{
	uint8_t expected[NUPHY_FRAME_MAX];
	size_t len = nuphy_suspend(expected, seq);

	CHECK(out.reply);
	CHECK_EQ(out.reply_len, len);
	CHECK_MEM(out.reply_buf, expected, len);
}

static void connect_and_suspend(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	link_set_host_suspended(&l, true);
	message(NUPHY_KEEPALIVE, 1, 50);
	expect_suspend(2);
}

static void test_never_paired_idle_alternates_fixed_channels_with_channel_2(void)
{
	init(NULL);
	CHECK_EQ(out.channel, 16);
	tick(399);
	CHECK(!out.set_channel);
	tick(400);
	CHECK_EQ(out.channel, 2);
	tick(799);
	CHECK(!out.set_channel);
	tick(800);
	CHECK_EQ(out.channel, 22);
	CHECK_EQ(link_led(&l), LINK_LED_IDLE);
}

static void test_paired_idle_alternates_home_with_channel_2(void)
{
	init(home);
	CHECK_EQ(out.channel, 26);
	tick(400);
	CHECK_EQ(out.channel, 2);
	tick(799);
	CHECK(!out.set_channel);
	tick(800);
	CHECK_EQ(out.channel, 26);
}

static void test_pairing_offers_surveyed_channels_and_moves_home_once_confirmed(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(NULL);
	tick(400);
	deliver(&request, 0, 450);
	CHECK(out.start_survey);
	CHECK_EQ(link_led(&l), LINK_LED_PAIRING);

	survey(surveyed, 2000);
	CHECK(out.set_channel);
	CHECK_EQ(out.channel, 2);
	expect_pair_reply(1);

	deliver(&request, 0, 2040);
	expect_pair_reply(2);

	deliver(&confirm, 0, 2080);
	CHECK(out.save_channels);
	CHECK_MEM(l.own, surveyed, 3);
	expect_status(3, 0);

	message(NUPHY_KEEPALIVE, 1, 2120);
	CHECK(out.set_channel);
	CHECK_EQ(out.channel, 30);
	CHECK_EQ(link_led(&l), LINK_LED_CONNECTED);
}

static void test_confirm_for_another_dongle_is_ignored(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, previous_id);
	struct nuphy_msg ours = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(NULL);
	deliver(&request, 0, 10);
	survey(surveyed, 1600);
	deliver(&confirm, 0, 1640);
	CHECK(!out.save_channels);
	CHECK(!l.own_known);

	deliver(&ours, 0, 1680);
	CHECK(out.save_channels);
}

static void pairing_times_out_after_a_survey(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(NULL);
	deliver(&request, 0, 10);
	survey(surveyed, 1600);
	tick(2600);
}

static void test_a_request_soon_after_a_survey_reuses_its_channels(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	pairing_times_out_after_a_survey();
	deliver(&request, 0, 3000);
	CHECK(!out.start_survey);
	expect_pair_reply(2);
	deliver(&confirm, 0, 3040);
	CHECK(out.save_channels);
	CHECK_MEM(l.own, surveyed, 3);
}

static void test_a_request_10_s_after_a_survey_surveys_again(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	pairing_times_out_after_a_survey();
	deliver(&request, 0, 11600);
	CHECK(out.start_survey);
}

static void test_failed_survey_returns_to_idle(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(NULL);
	deliver(&request, 0, 10);
	survey_failed(1600);
	CHECK_EQ(link_led(&l), LINK_LED_IDLE);
	CHECK(out.set_channel);
	CHECK_EQ(out.channel, 16);
}

static void test_rebooted_dongle_connects_on_home_and_sends_status_every_100_ms(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK(!out.set_channel);
	expect_status(1, 0);
	message(NUPHY_KEYBOARD, 1, 60);
	CHECK(!out.reply);
	message(NUPHY_KEEPALIVE, 1, 110);
	expect_status(2, 0);
}

static void test_status_carries_host_leds(void)
{
	init(home);
	link_set_host_leds(&l, 0x02);
	message(NUPHY_KEEPALIVE, 1, 10);
	expect_status(1, 0x02);
}

static void test_suspend_goes_out_on_the_next_packet_then_status_resumes(void)
{
	connect_and_suspend();
	message(NUPHY_KEEPALIVE, 1, 90);
	CHECK(!out.reply);
	message(NUPHY_KEEPALIVE, 1, 150);
	expect_status(3, 0);
}

static void test_suspend_is_resent_after_a_reconnect(void)
{
	connect_and_suspend();
	tick(1050);
	CHECK(out.lost);
	message(NUPHY_KEEPALIVE, 1, 1100);
	expect_status(3, 0);
	message(NUPHY_KEEPALIVE, 1, 1140);
	expect_suspend(4);
}

static void test_resume_then_suspend_sends_it_again(void)
{
	connect_and_suspend();
	link_set_host_suspended(&l, false);
	message(NUPHY_KEEPALIVE, 1, 150);
	expect_status(3, 0);
	link_set_host_suspended(&l, true);
	message(NUPHY_KEEPALIVE, 1, 160);
	expect_suspend(4);
}

static void test_silence_for_1_s_loses_the_link(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	tick(1009);
	CHECK(!out.lost);
	tick(1010);
	CHECK(out.lost);
	CHECK_EQ(out.channel, 26);
	CHECK_EQ(link_led(&l), LINK_LED_IDLE);
}

static void test_invalid_message_does_not_keep_the_link(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	message(NUPHY_INVALID, 1, 900);
	tick(1010);
	CHECK(out.lost);
}

static void test_pipe0_traffic_does_not_keep_a_connected_link(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, dongle_id);

	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	deliver(&request, 0, 900);
	tick(1010);
	CHECK(out.lost);
}

static void test_a_radio_pause_restarts_the_silence_timer(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	link_radio_resumed(&l, 1600);
	tick(2599);
	CHECK(!out.lost);
	tick(2600);
	CHECK(out.lost);
}

static void test_traffic_after_a_loss_reports_the_link_up(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK(out.up);
	tick(1010);
	CHECK(out.lost);
	message(NUPHY_KEEPALIVE, 1, 5000);
	CHECK(out.up);
	message(NUPHY_KEEPALIVE, 1, 5010);
	CHECK(!out.up);
}

static void test_a_confirmed_pairing_does_not_report_the_link_up(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, dongle_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(NULL);
	deliver(&request, 0, 10);
	survey(surveyed, 20);
	deliver(&confirm, 0, 30);
	message(NUPHY_KEEPALIVE, 1, 40);
	CHECK(!out.up);
	CHECK(!out.moved);
	CHECK_EQ(link_led(&l), LINK_LED_CONNECTED);
}

static void hear_keyboard_on_the_pairing_channel(const uint8_t *own)
{
	init(own);
	tick(400);
	CHECK_EQ(out.channel, 2);
	message(NUPHY_KEEPALIVE, 1, 410);
}

static void test_a_keyboard_heard_on_the_pairing_channel_is_moved_home(void)
{
	hear_keyboard_on_the_pairing_channel(home);
	CHECK(out.up);
	CHECK(!out.set_channel);
	expect_status(1, 0);
	CHECK_EQ(link_led(&l), LINK_LED_CONNECTED);
	message(NUPHY_KEEPALIVE, 1, 450);
	CHECK(!out.moved);
	CHECK(!out.set_channel);
	CHECK(!out.reply);
	message(NUPHY_KEEPALIVE, 1, 510);
	CHECK(!out.set_channel);
	expect_status(2, 0);
	message(NUPHY_HELLO, 1, 520);
	CHECK(out.moved);
	CHECK_EQ(out.moved_from, 2);
	CHECK(out.set_channel);
	CHECK_EQ(out.channel, home[0]);
	expect_status(3, 0);
	CHECK_EQ(link_led(&l), LINK_LED_CONNECTED);
}

static void test_silence_after_the_move_home_starts_loses_the_link(void)
{
	hear_keyboard_on_the_pairing_channel(home);
	tick(1410);
	CHECK(out.lost);
}

static void test_a_keyboard_heard_on_the_pairing_channel_of_a_never_paired_dongle_stays(void)
{
	hear_keyboard_on_the_pairing_channel(NULL);
	CHECK(out.up);
	CHECK(!out.set_channel);
	CHECK_EQ(link_led(&l), LINK_LED_CONNECTED);
}

static void test_a_250_ms_silence_while_connected_is_a_gap(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK_EQ(out.gap_ms, 0);
	message(NUPHY_KEEPALIVE, 1, 259);
	CHECK_EQ(out.gap_ms, 0);
	message(NUPHY_KEEPALIVE, 1, 509);
	CHECK_EQ(out.gap_ms, 250);
	CHECK_EQ(out.gap_channel, 26);
}

static void test_a_radio_pause_is_not_a_gap(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	link_radio_resumed(&l, 1600);
	message(NUPHY_KEEPALIVE, 1, 1610);
	CHECK_EQ(out.gap_ms, 0);
}

static void test_the_packet_that_brings_the_link_up_is_not_a_gap(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	tick(1010);
	message(NUPHY_KEEPALIVE, 1, 5000);
	CHECK(out.up);
	CHECK_EQ(out.gap_ms, 0);
}

static void test_the_first_packet_after_a_pairing_confirm_is_not_a_gap(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, dongle_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(NULL);
	deliver(&request, 0, 10);
	survey(surveyed, 20);
	deliver(&confirm, 0, 30);
	message(NUPHY_KEEPALIVE, 1, 400);
	CHECK_EQ(out.gap_ms, 0);
}

static void test_a_gap_ended_by_the_move_home_is_on_the_pairing_channel(void)
{
	hear_keyboard_on_the_pairing_channel(home);
	message(NUPHY_HELLO, 1, 700);
	CHECK(out.moved);
	CHECK_EQ(out.gap_ms, 290);
	CHECK_EQ(out.gap_channel, 2);
}

static void test_the_wait_for_the_keyboard_after_a_move_home_is_a_gap(void)
{
	hear_keyboard_on_the_pairing_channel(home);
	message(NUPHY_HELLO, 1, 520);
	CHECK(out.moved);
	message(NUPHY_KEEPALIVE, 1, 780);
	CHECK_EQ(out.gap_ms, 260);
	CHECK_EQ(out.gap_channel, home[0]);
}

static const uint8_t forced[3] = {62, 52, 44};

static void test_forced_channels_are_offered_without_a_survey_and_become_home(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(home);
	CHECK(link_force_channels(&l, forced));
	deliver(&request, 0, 450);
	CHECK(!out.start_survey);
	expect_pair_reply_offering(1, forced);

	deliver(&confirm, 0, 490);
	CHECK(out.save_channels);
	CHECK_MEM(l.own, forced, 3);

	message(NUPHY_KEEPALIVE, 1, 530);
	CHECK_EQ(out.channel, 62);
}

static void test_forced_channels_win_over_a_recent_survey(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	pairing_times_out_after_a_survey();
	CHECK(link_force_channels(&l, forced));
	deliver(&request, 0, 3000);
	CHECK(!out.start_survey);
	expect_pair_reply_offering(2, forced);
}

static void test_a_pairing_on_forced_channels_is_not_reused_once_they_are_cleared(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	CHECK(link_force_channels(&l, forced));
	deliver(&request, 0, 10);
	tick(1010);
	CHECK(link_force_channels(&l, NULL));
	deliver(&request, 0, 1100);
	CHECK(out.start_survey);
}

static void test_channels_forced_during_a_pairing_do_not_change_what_it_saves(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(NULL);
	deliver(&request, 0, 10);
	survey(surveyed, 1600);
	CHECK(link_force_channels(&l, forced));
	deliver(&confirm, 0, 1640);
	CHECK(out.save_channels);
	CHECK_MEM(l.own, surveyed, 3);
}

static void test_forcing_during_a_forced_pairing_does_not_change_what_it_saves(void)
{
	static const uint8_t other[3] = {64, 54, 46};
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(NULL);
	CHECK(link_force_channels(&l, forced));
	deliver(&request, 0, 10);
	CHECK(link_force_channels(&l, other));
	deliver(&confirm, 0, 50);
	CHECK_MEM(l.own, forced, 3);
}

static void expect_refused(uint8_t a, uint8_t b, uint8_t c)
{
	const uint8_t channels[3] = {a, b, c};

	const uint8_t *kept;

	CHECK(!link_force_channels(&l, channels));
	kept = link_forced_channels(&l);
	CHECK(kept != NULL && memcmp(kept, forced, 3) == 0);
}

static void test_pairing_channels_repeats_and_out_of_range_channels_are_refused(void)
{
	init(home);
	CHECK(link_force_channels(&l, forced));
	expect_refused(2, 52, 44);
	expect_refused(62, 50, 44);
	expect_refused(62, 52, 80);
	expect_refused(62, 62, 44);
	expect_refused(62, 52, 62);
	expect_refused(1, 52, 44);
	expect_refused(62, 52, 81);
	CHECK(link_force_channels(&l, (const uint8_t[3]){26, 3, 79}));
}

static void test_a_dongle_with_nothing_forced_reports_no_forced_channels(void)
{
	init(home);
	CHECK(link_forced_channels(&l) == NULL);
}

int main(void)
{
	RUN(test_never_paired_idle_alternates_fixed_channels_with_channel_2);
	RUN(test_paired_idle_alternates_home_with_channel_2);
	RUN(test_pairing_offers_surveyed_channels_and_moves_home_once_confirmed);
	RUN(test_confirm_for_another_dongle_is_ignored);
	RUN(test_a_request_soon_after_a_survey_reuses_its_channels);
	RUN(test_a_request_10_s_after_a_survey_surveys_again);
	RUN(test_failed_survey_returns_to_idle);
	RUN(test_rebooted_dongle_connects_on_home_and_sends_status_every_100_ms);
	RUN(test_status_carries_host_leds);
	RUN(test_suspend_goes_out_on_the_next_packet_then_status_resumes);
	RUN(test_suspend_is_resent_after_a_reconnect);
	RUN(test_resume_then_suspend_sends_it_again);
	RUN(test_silence_for_1_s_loses_the_link);
	RUN(test_invalid_message_does_not_keep_the_link);
	RUN(test_pipe0_traffic_does_not_keep_a_connected_link);
	RUN(test_a_radio_pause_restarts_the_silence_timer);
	RUN(test_traffic_after_a_loss_reports_the_link_up);
	RUN(test_a_confirmed_pairing_does_not_report_the_link_up);
	RUN(test_a_keyboard_heard_on_the_pairing_channel_is_moved_home);
	RUN(test_silence_after_the_move_home_starts_loses_the_link);
	RUN(test_a_keyboard_heard_on_the_pairing_channel_of_a_never_paired_dongle_stays);
	RUN(test_a_250_ms_silence_while_connected_is_a_gap);
	RUN(test_a_radio_pause_is_not_a_gap);
	RUN(test_the_packet_that_brings_the_link_up_is_not_a_gap);
	RUN(test_the_first_packet_after_a_pairing_confirm_is_not_a_gap);
	RUN(test_a_gap_ended_by_the_move_home_is_on_the_pairing_channel);
	RUN(test_the_wait_for_the_keyboard_after_a_move_home_is_a_gap);
	RUN(test_forced_channels_are_offered_without_a_survey_and_become_home);
	RUN(test_forced_channels_win_over_a_recent_survey);
	RUN(test_a_pairing_on_forced_channels_is_not_reused_once_they_are_cleared);
	RUN(test_channels_forced_during_a_pairing_do_not_change_what_it_saves);
	RUN(test_forcing_during_a_forced_pairing_does_not_change_what_it_saves);
	RUN(test_pairing_channels_repeats_and_out_of_range_channels_are_refused);
	RUN(test_a_dongle_with_nothing_forced_reports_no_forced_channels);
	return TEST_EXIT();
}
