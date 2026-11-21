#include <string.h>
#include "test.h"
#include "link.h"

static const uint8_t dongle_id[4] = {0x26, 0xc1, 0x21, 0x59};
static const uint8_t keyboard_id[4] = {0x70, 0x15, 0xa3, 0x29};
static const uint8_t previous_id[4] = {0x96, 0xbe, 0x5c, 0x89};
static const uint8_t stored_id[4] = {0x11, 0x22, 0x33, 0x44};
static const uint8_t home[3] = {26, 30, 34};
static const uint8_t surveyed[3] = {30, 40, 60};

static struct link l;
static struct link_out out;
static uint32_t last_ms;
static bool accepted;

static struct nuphy_msg pairing(enum nuphy_kind kind, const uint8_t *dongle)
{
	struct nuphy_msg m = {.kind = kind};

	memcpy(m.ids.keyboard, keyboard_id, 4);
	memcpy(m.ids.dongle, dongle, 4);
	return m;
}

static void init(const uint8_t *own)
{
	struct link_record record = {0};

	out = (struct link_out){0};
	last_ms = 0;
	if (own != NULL) {
		memcpy(record.own, own, LINK_OWN_CHANNELS);
		memcpy(record.keyboard_id, stored_id, NUPHY_ID_LEN);
	}
	link_init(&l, dongle_id, own == NULL ? NULL : &record, 0, &out);
	link_open_pairing(&l, PAIRWIN_BOOT, 0);
}

static void deliver(const struct nuphy_msg *m, uint8_t pipe, uint32_t now)
{
	out = (struct link_out){0};
	last_ms = now;
	accepted = link_on_message(&l, m, pipe, now, &out);
}

static void message(enum nuphy_kind kind, uint8_t pipe, uint32_t now)
{
	struct nuphy_msg m = {.kind = kind};

	deliver(&m, pipe, now);
}

static void survey(const uint8_t channels[3], uint32_t now)
{
	out = (struct link_out){0};
	last_ms = now;
	link_on_survey(&l, channels, now, &out);
}

static void survey_failed(uint32_t now)
{
	out = (struct link_out){0};
	last_ms = now;
	link_survey_failed(&l, now, &out);
}

static void tick(uint32_t now)
{
	out = (struct link_out){0};
	last_ms = now;
	link_tick(&l, now, &out);
}

static void open_pairing(uint32_t now)
{
	last_ms = now;
	link_open_pairing(&l, PAIRWIN_REQUEST, now);
}

static enum link_led led(void)
{
	return link_led(&l, last_ms);
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
	CHECK_EQ(led(), LINK_LED_PAIRING);

	survey(surveyed, 2000);
	CHECK(out.set_channel);
	CHECK_EQ(out.channel, 2);
	expect_pair_reply(1);

	deliver(&request, 0, 2040);
	expect_pair_reply(2);

	deliver(&confirm, 0, 2080);
	CHECK(out.save_pairing);
	CHECK_MEM(l.own, surveyed, 3);
	expect_status(3, 0);

	message(NUPHY_KEEPALIVE, 1, 2120);
	CHECK(out.set_channel);
	CHECK_EQ(out.channel, 30);
	CHECK_EQ(led(), LINK_LED_CONNECTED);
}

static const uint8_t name_chunks[][NUPHY_NAME_CHUNK] = {
	{0x2c, 0x03, 'N', 0, 'u', 0, 'P', 0, 'h', 0, 'y', 0, ' ', 0, 'A', 0, 'i', 0, 'r', 0},
	{'7', 0, '5', 0, ' ', 0, 'V', 0, '2', 0, ' ', 0, 'D', 0, 'o', 0, 'n', 0, 'g', 0},
	{'l', 0, 'e', 0},
};
static const uint8_t no_name[NUPHY_NAME_MAX];

static void send_name(uint8_t pipe, uint32_t now)
{
	for (size_t i = 0; i < sizeof(name_chunks) / sizeof(name_chunks[0]); i++) {
		struct nuphy_msg m = {.kind = NUPHY_NAME};

		memcpy(m.name_chunk, name_chunks[i], NUPHY_NAME_CHUNK);
		deliver(&m, pipe, now + (uint32_t)i);
	}
}

static void test_the_paired_keyboard_sends_its_name_on_pipe_1(void)
{
	static const uint8_t expected[NUPHY_NAME_MAX] = "NuPhy Air75 V2";

	init(home);
	send_name(0, 10);
	CHECK_MEM(l.keyboard_name.name, no_name, sizeof(no_name));
	send_name(1, 20);
	CHECK_MEM(l.keyboard_name.name, expected, sizeof(expected));
}

static void test_a_new_name_from_the_paired_keyboard_is_saved(void)
{
	static const uint8_t expected[NUPHY_NAME_MAX] = "NuPhy Air75 V2";
	struct link_record record;

	init(home);
	send_name(1, 10);
	CHECK(out.save_name);
	link_record_of(&l, &record);
	CHECK_MEM(record.name, expected, sizeof(expected));
}

static void test_the_same_name_again_is_not_saved(void)
{
	init(home);
	send_name(1, 10);
	send_name(1, 20);
	CHECK(!out.save_name);
}

static void test_a_name_heard_before_any_pairing_is_not_saved(void)
{
	init(NULL);
	send_name(1, 10);
	CHECK(!out.save_name);
}

static void test_a_rebooted_dongle_knows_the_saved_name(void)
{
	static const uint8_t expected[NUPHY_NAME_MAX] = "NuPhy Air75 V2";
	struct link_record record;

	init(home);
	send_name(1, 10);
	link_record_of(&l, &record);
	link_init(&l, dongle_id, &record, 0, &out);
	CHECK_MEM(l.keyboard_name.name, expected, sizeof(expected));
}

static void test_a_confirmed_pairing_forgets_the_name(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(home);
	send_name(1, 10);
	tick(1100);
	open_pairing(1200);
	deliver(&request, 0, 1250);
	survey(surveyed, 2000);
	deliver(&confirm, 0, 2040);
	CHECK(out.save_pairing);
	CHECK_MEM(l.keyboard_name.name, no_name, sizeof(no_name));
}

static void test_a_rebooted_dongle_keeps_the_keyboard_pairing_confirmed(void)
{
	init(home);
	CHECK_MEM(l.keyboard_id, stored_id, 4);
}

static void test_the_record_is_the_paired_channels_then_the_keyboard(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);
	struct link_record record;

	init(NULL);
	deliver(&request, 0, 10);
	survey(surveyed, 1600);
	deliver(&confirm, 0, 1640);
	link_record_of(&l, &record);
	CHECK_MEM(record.own, surveyed, sizeof(record.own));
	CHECK_MEM(record.keyboard_id, keyboard_id, sizeof(record.keyboard_id));
}

static void test_a_confirmed_pairing_replaces_the_keyboard(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(home);
	deliver(&request, 0, 10);
	survey(surveyed, 1600);
	deliver(&confirm, 0, 1640);
	CHECK(out.save_pairing);
	CHECK_MEM(l.keyboard_id, keyboard_id, 4);
}

static void test_an_unconfirmed_pairing_keeps_the_paired_keyboard(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	deliver(&request, 0, 10);
	survey(surveyed, 1600);
	CHECK_MEM(l.keyboard_id, stored_id, 4);
	tick(2600);
	CHECK(out.pairing_expired);
	CHECK_MEM(l.keyboard_id, stored_id, 4);
}

static void forget(uint32_t now)
{
	out = (struct link_out){0};
	last_ms = now;
	link_forget(&l, now, &out);
}

static void test_a_forgotten_keyboard_is_dropped_until_it_pairs_again(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK(link_connected(&l));
	forget(20);
	CHECK(!l.own_known);
	CHECK(!link_connected(&l));
	CHECK_EQ(link_home(&l), CHANMOVE_NO_HOME);
	CHECK(link_pairing_open(&l, 100000));
	message(NUPHY_KEEPALIVE, 1, 30);
	CHECK(!accepted);
	deliver(&request, 0, 100000);
	CHECK(out.start_survey);
}

static void test_forgetting_keeps_the_forced_channels_and_the_host_state(void)
{
	static const uint8_t forced[3] = {30, 40, 60};

	init(home);
	CHECK(link_force_channels(&l, forced));
	link_set_host_leds(&l, 0x02);
	link_set_host_suspended(&l, true);
	forget(20);
	CHECK(link_forced_channels(&l) != NULL);
	CHECK_MEM(link_forced_channels(&l), forced, 3);
	CHECK_EQ(l.host_leds, 0x02);
	CHECK(l.host_suspended);
	CHECK_MEM(l.dongle_id, dongle_id, 4);
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
	CHECK(!out.save_pairing);
	CHECK(!l.own_known);

	deliver(&ours, 0, 1680);
	CHECK(out.save_pairing);
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
	CHECK(out.save_pairing);
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

static void test_a_status_dropped_from_the_radio_goes_out_on_the_next_packet(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	expect_status(1, 0);
	link_replies_dropped(&l, 20);
	message(NUPHY_KEEPALIVE, 1, 50);
	expect_status(2, 0);
	message(NUPHY_KEEPALIVE, 1, 90);
	CHECK(!out.reply);
	message(NUPHY_KEEPALIVE, 1, 150);
	expect_status(3, 0);
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
	CHECK_EQ(led(), LINK_LED_IDLE);
}

static void test_the_link_is_connected_from_its_first_packet_until_it_is_lost(void)
{
	init(home);
	CHECK(!link_connected(&l));
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK(link_connected(&l));
	tick(1010);
	CHECK(!link_connected(&l));
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
	CHECK_EQ(led(), LINK_LED_CONNECTED);
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
	CHECK_EQ(led(), LINK_LED_CONNECTED);
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
	CHECK_EQ(led(), LINK_LED_CONNECTED);
}

static void test_the_link_is_connected_while_it_moves_home(void)
{
	hear_keyboard_on_the_pairing_channel(home);
	CHECK(link_connected(&l));
}

static void test_silence_after_the_move_home_starts_loses_the_link(void)
{
	hear_keyboard_on_the_pairing_channel(home);
	tick(1410);
	CHECK(out.lost);
}

static void test_a_never_paired_dongle_drops_a_keyboard_that_has_its_id(void)
{
	hear_keyboard_on_the_pairing_channel(NULL);
	CHECK(!accepted);
	CHECK(!out.up);
	CHECK(!out.reply);
	CHECK(!link_connected(&l));
	CHECK_EQ(led(), LINK_LED_PAIRING);
}

static void test_a_paired_dongle_takes_its_keyboard(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK(accepted);
	CHECK(out.up);
}

static void test_a_keyboard_dropped_by_a_never_paired_dongle_can_still_pair(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(NULL);
	message(NUPHY_KEEPALIVE, 1, 10);
	deliver(&request, 0, 100000);
	CHECK(accepted);
	CHECK(out.start_survey);
}

static enum chanmove_result move(uint8_t channel)
{
	return link_move(&l, channel, last_ms);
}

static void test_a_requested_move_holds_through_a_loss(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK_EQ(move(44), CHANMOVE_DONE);
	CHECK_EQ(l.channel, 44);
	message(NUPHY_KEEPALIVE, 1, 300);
	CHECK_EQ(out.gap_ms, 290);
	CHECK_EQ(out.gap_channel, 44);
	tick(1300);
	CHECK(out.lost);
	CHECK_EQ(l.channel, 44);
	tick(1700);
	CHECK(!out.set_channel);
	CHECK_EQ(l.channel, 44);
}

static void test_a_requested_move_takes_any_of_the_keyboards_channels(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK_EQ(move(home[2]), CHANMOVE_DONE);
	CHECK_EQ(l.channel, home[2]);
	CHECK_EQ(move(40), CHANMOVE_NOT_A_CHANNEL);
	CHECK_EQ(move(2), CHANMOVE_NOT_A_CHANNEL);
	CHECK_EQ(l.channel, home[2]);
}

static void test_a_requested_move_works_on_a_silent_paired_link(void)
{
	init(home);
	tick(PAIRWIN_MS);
	CHECK_EQ(l.channel, home[0]);
	CHECK_EQ(move(44), CHANMOVE_DONE);
	CHECK_EQ(l.channel, 44);
	tick(PAIRWIN_MS + 400);
	CHECK(!out.set_channel);
	message(NUPHY_KEEPALIVE, 1, PAIRWIN_MS + 410);
	CHECK(out.up);
	CHECK_EQ(l.channel, 44);
}

static int movable(void)
{
	return link_movable(&l, last_ms);
}

static void test_a_move_needs_a_paired_keyboard_and_pairing_closed(void)
{
	init(home);
	CHECK_EQ(move(44), CHANMOVE_NOT_MOVABLE);
	CHECK_EQ(movable(), CHANMOVE_NO_HOME);
	CHECK_EQ(link_home(&l), home[0]);
	hear_keyboard_on_the_pairing_channel(home);
	CHECK_EQ(move(44), CHANMOVE_NOT_MOVABLE);
	CHECK_EQ(movable(), CHANMOVE_NO_HOME);
	init(NULL);
	tick(PAIRWIN_MS);
	CHECK_EQ(move(44), CHANMOVE_NOT_MOVABLE);
	CHECK_EQ(movable(), CHANMOVE_NO_HOME);
	CHECK_EQ(link_home(&l), CHANMOVE_NO_HOME);
}

static void test_a_movable_link_reports_its_home_through_a_move_and_a_loss(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	CHECK_EQ(movable(), home[0]);
	CHECK_EQ(move(44), CHANMOVE_DONE);
	CHECK_EQ(movable(), 44);
	tick(1010);
	CHECK(out.lost);
	CHECK_EQ(movable(), 44);
}

static void test_no_move_while_the_host_is_suspended(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	link_set_host_suspended(&l, true);
	CHECK_EQ(movable(), CHANMOVE_NO_HOME);
	CHECK_EQ(move(44), CHANMOVE_NOT_MOVABLE);
	CHECK_EQ(l.channel, home[0]);
	link_set_host_suspended(&l, false);
	CHECK_EQ(movable(), home[0]);
	CHECK_EQ(move(44), CHANMOVE_DONE);
}

static void check_channels(const uint8_t *own, const uint8_t *expected, size_t n)
{
	const uint8_t *channels;

	init(own);
	CHECK_EQ(link_channels(&l, &channels), n);
	CHECK_MEM(channels, expected, n);
}

static void test_the_keyboards_channels_are_the_fixed_ones_and_the_paired_three(void)
{
	static const uint8_t expected[] = {16, 22, 44, 48, 58, 72, 78, 26, 30, 34};

	check_channels(home, expected, sizeof(expected));
}

static void test_a_paired_channel_that_is_also_fixed_is_listed_once(void)
{
	static const uint8_t own[3] = {44, 54, 64};
	static const uint8_t expected[] = {16, 22, 44, 48, 58, 72, 78, 54, 64};

	check_channels(own, expected, sizeof(expected));
}

static void test_a_never_paired_dongle_lists_the_fixed_channels(void)
{
	check_channels(NULL, diag_nuphy_fixed_channels, LINK_FIXED_CHANNELS);
}

static void test_a_paired_dongle_waits_on_home_while_pairing_is_closed(void)
{
	init(home);
	tick(PAIRWIN_MS - 100);
	CHECK_EQ(l.channel, 2);
	tick(PAIRWIN_MS);
	CHECK(out.set_channel);
	CHECK_EQ(out.channel, home[0]);
	tick(PAIRWIN_MS + 400);
	CHECK(!out.set_channel);
	tick(PAIRWIN_MS + 800);
	CHECK(!out.set_channel);
	CHECK_EQ(l.channel, home[0]);
}

static void test_a_lost_link_waits_on_home_once_pairing_has_closed(void)
{
	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	tick(1010);
	CHECK(out.lost);
	CHECK_EQ(l.channel, home[0]);
	tick(1410);
	CHECK(!out.set_channel);
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
	CHECK(out.save_pairing);
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
	CHECK(out.save_pairing);
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

static void test_a_paired_dongle_answers_a_request_in_its_first_30_s(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	deliver(&request, 0, 29999);
	CHECK(out.start_survey);
}

static void test_a_paired_dongle_ignores_a_request_after_its_first_30_s(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	tick(30000);
	CHECK_EQ(l.channel, home[0]);
	deliver(&request, 0, 30010);
	CHECK(!out.start_survey);
	CHECK(!out.reply);
}

static void test_the_keyboard_connecting_closes_pairing_opened_at_boot(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	message(NUPHY_KEEPALIVE, 1, 10);
	tick(1010);
	deliver(&request, 0, 1100);
	CHECK(!out.start_survey);
	tick(30000);
	CHECK(!out.window_closed);
}

static void test_opening_pairing_lets_a_paired_dongle_answer_for_30_s(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	tick(40000);
	open_pairing(50000);
	deliver(&request, 0, 50000 + 29999);
	CHECK(out.start_survey);
}

static void test_pairing_opened_on_request_ignores_the_keyboard_connecting(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	tick(40000);
	open_pairing(50000);
	message(NUPHY_KEEPALIVE, 1, 50010);
	tick(51010);
	deliver(&request, 0, 51100);
	CHECK(out.start_survey);
}

static void test_another_keyboard_does_not_keep_a_pairing_alive(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	deliver(&request, 0, 10);
	survey(surveyed, 20);
	for (uint32_t t = 60; t < 1020; t += 40) {
		message(NUPHY_KEEPALIVE, 1, t);
	}
	tick(1020);
	CHECK(out.pairing_expired);
}

static void test_stored_channels_holding_a_pairing_channel_count_as_never_paired(void)
{
	static const uint8_t stored[3] = {2, 30, 34};

	init(stored);
	CHECK_EQ(link_home(&l), CHANMOVE_NO_HOME);
}

static void test_a_never_paired_dongle_answers_a_request_at_any_time(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(NULL);
	tick(100000);
	deliver(&request, 0, 100010);
	CHECK(out.start_survey);
}

static void test_a_confirmed_pairing_closes_pairing(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(NULL);
	deliver(&request, 0, 10);
	survey(surveyed, 1600);
	deliver(&confirm, 0, 1640);
	message(NUPHY_KEEPALIVE, 1, 1680);
	tick(2680);
	deliver(&request, 0, 2700);
	CHECK(!out.start_survey);
	CHECK(!out.reply);
}

static void test_a_confirmed_pairing_is_not_reported_as_running_out(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);
	struct nuphy_msg confirm = pairing(NUPHY_PAIR_CONFIRM, dongle_id);

	init(home);
	deliver(&request, 0, 10);
	survey(surveyed, 1600);
	deliver(&confirm, 0, 1640);
	tick(1650);
	CHECK(!out.window_closed);
	message(NUPHY_KEEPALIVE, 1, 1680);
	tick(1690);
	CHECK(!out.window_closed);
	tick(40000);
	CHECK(!out.window_closed);
}

static void test_the_led_shows_pairing_while_it_is_open(void)
{
	init(home);
	CHECK_EQ(led(), LINK_LED_PAIRING);
	tick(30000);
	CHECK_EQ(led(), LINK_LED_IDLE);
	open_pairing(40000);
	CHECK_EQ(led(), LINK_LED_PAIRING);
	message(NUPHY_KEEPALIVE, 1, 40010);
	CHECK_EQ(led(), LINK_LED_PAIRING);
}

static void test_pairing_running_out_is_reported_once(void)
{
	init(home);
	tick(29999);
	CHECK(!out.window_closed);
	tick(30000);
	CHECK(out.window_closed);
	tick(30001);
	CHECK(!out.window_closed);
}

static void test_pairing_running_out_mid_attempt_is_reported_when_the_attempt_ends(void)
{
	struct nuphy_msg request = pairing(NUPHY_PAIR_REQUEST, previous_id);

	init(home);
	deliver(&request, 0, 29000);
	survey(surveyed, 29500);
	tick(30000);
	CHECK(!out.window_closed);
	tick(30500);
	CHECK(out.window_closed);
	CHECK(out.pairing_expired);
}

static void test_a_pairing_with_no_confirm_expires(void)
{
	pairing_times_out_after_a_survey();
	CHECK(out.pairing_expired);
	tick(2700);
	CHECK(!out.pairing_expired);
}

int main(void)
{
	RUN(test_never_paired_idle_alternates_fixed_channels_with_channel_2);
	RUN(test_paired_idle_alternates_home_with_channel_2);
	RUN(test_pairing_offers_surveyed_channels_and_moves_home_once_confirmed);
	RUN(test_confirm_for_another_dongle_is_ignored);
	RUN(test_a_forgotten_keyboard_is_dropped_until_it_pairs_again);
	RUN(test_forgetting_keeps_the_forced_channels_and_the_host_state);
	RUN(test_a_rebooted_dongle_keeps_the_keyboard_pairing_confirmed);
	RUN(test_the_record_is_the_paired_channels_then_the_keyboard);
	RUN(test_a_confirmed_pairing_replaces_the_keyboard);
	RUN(test_an_unconfirmed_pairing_keeps_the_paired_keyboard);
	RUN(test_a_request_soon_after_a_survey_reuses_its_channels);
	RUN(test_a_request_10_s_after_a_survey_surveys_again);
	RUN(test_failed_survey_returns_to_idle);
	RUN(test_rebooted_dongle_connects_on_home_and_sends_status_every_100_ms);
	RUN(test_a_status_dropped_from_the_radio_goes_out_on_the_next_packet);
	RUN(test_status_carries_host_leds);
	RUN(test_suspend_goes_out_on_the_next_packet_then_status_resumes);
	RUN(test_suspend_is_resent_after_a_reconnect);
	RUN(test_resume_then_suspend_sends_it_again);
	RUN(test_silence_for_1_s_loses_the_link);
	RUN(test_the_link_is_connected_from_its_first_packet_until_it_is_lost);
	RUN(test_the_link_is_connected_while_it_moves_home);
	RUN(test_invalid_message_does_not_keep_the_link);
	RUN(test_pipe0_traffic_does_not_keep_a_connected_link);
	RUN(test_a_radio_pause_restarts_the_silence_timer);
	RUN(test_traffic_after_a_loss_reports_the_link_up);
	RUN(test_a_confirmed_pairing_does_not_report_the_link_up);
	RUN(test_a_keyboard_heard_on_the_pairing_channel_is_moved_home);
	RUN(test_silence_after_the_move_home_starts_loses_the_link);
	RUN(test_a_never_paired_dongle_drops_a_keyboard_that_has_its_id);
	RUN(test_a_paired_dongle_takes_its_keyboard);
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
	RUN(test_a_keyboard_dropped_by_a_never_paired_dongle_can_still_pair);
	RUN(test_a_paired_dongle_answers_a_request_in_its_first_30_s);
	RUN(test_a_paired_dongle_ignores_a_request_after_its_first_30_s);
	RUN(test_the_keyboard_connecting_closes_pairing_opened_at_boot);
	RUN(test_opening_pairing_lets_a_paired_dongle_answer_for_30_s);
	RUN(test_pairing_opened_on_request_ignores_the_keyboard_connecting);
	RUN(test_another_keyboard_does_not_keep_a_pairing_alive);
	RUN(test_stored_channels_holding_a_pairing_channel_count_as_never_paired);
	RUN(test_a_never_paired_dongle_answers_a_request_at_any_time);
	RUN(test_a_confirmed_pairing_closes_pairing);
	RUN(test_a_confirmed_pairing_is_not_reported_as_running_out);
	RUN(test_the_led_shows_pairing_while_it_is_open);
	RUN(test_pairing_running_out_is_reported_once);
	RUN(test_pairing_running_out_mid_attempt_is_reported_when_the_attempt_ends);
	RUN(test_a_pairing_with_no_confirm_expires);
	RUN(test_the_paired_keyboard_sends_its_name_on_pipe_1);
	RUN(test_a_confirmed_pairing_forgets_the_name);
	RUN(test_a_new_name_from_the_paired_keyboard_is_saved);
	RUN(test_the_same_name_again_is_not_saved);
	RUN(test_a_name_heard_before_any_pairing_is_not_saved);
	RUN(test_a_rebooted_dongle_knows_the_saved_name);
	RUN(test_a_requested_move_holds_through_a_loss);
	RUN(test_a_requested_move_takes_any_of_the_keyboards_channels);
	RUN(test_a_requested_move_works_on_a_silent_paired_link);
	RUN(test_a_move_needs_a_paired_keyboard_and_pairing_closed);
	RUN(test_a_movable_link_reports_its_home_through_a_move_and_a_loss);
	RUN(test_no_move_while_the_host_is_suspended);
	RUN(test_the_keyboards_channels_are_the_fixed_ones_and_the_paired_three);
	RUN(test_a_paired_channel_that_is_also_fixed_is_listed_once);
	RUN(test_a_never_paired_dongle_lists_the_fixed_channels);
	RUN(test_a_paired_dongle_waits_on_home_while_pairing_is_closed);
	RUN(test_a_lost_link_waits_on_home_once_pairing_has_closed);
	return TEST_EXIT();
}
