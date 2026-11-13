#include "test.h"
#include "chanwatch.h"

static const uint8_t two[] = {5, 8};

static struct noise_tally home_noise(uint32_t harmful, uint32_t total)
{
	struct noise_tally t = {0};

	for (uint32_t i = 0; i < total; i++) {
		noise_add(&t, i < harmful ? -80 : -95);
	}
	return t;
}

static void with_home(struct chanwatch *w, const struct noise_tally *home)
{
	chanwatch_init(w, two, 2, 0);
	chanwatch_home(w, 5, home);
}

static void primed(struct chanwatch *w, const struct noise_tally *home, int8_t dbm8)
{
	with_home(w, home);
	for (int i = 0; i < CHANWATCH_MIN_SAMPLES; i++) {
		chanwatch_round(w, (const int8_t[]){0, dbm8}, 5, 0);
	}
}

static bool rounds_until_move(struct chanwatch *w, int rounds, int every, uint64_t now,
			      struct chanwatch_move *m)
{
	for (int i = 1; i <= rounds; i++) {
		int8_t dbm[2] = {0, every != 0 && i % every == 0 ? -80 : -95};

		chanwatch_round(w, dbm, 5, now);
		if (chanwatch_decide(w, 5, -70, now, m)) {
			return true;
		}
	}
	return false;
}

static void test_a_much_quieter_channel_wins_once_it_has_enough_samples(void)
{
	struct chanwatch w;
	struct chanwatch_move m;
	struct noise_tally home = home_noise(46, 100);

	with_home(&w, &home);
	CHECK(!rounds_until_move(&w, CHANWATCH_MIN_SAMPLES - 1, 0, 0, &m));
	CHECK(rounds_until_move(&w, 1, 0, 0, &m));
	CHECK_EQ(m.to, 8);
	CHECK_EQ(m.home_share, 4600);
	CHECK_EQ(m.to_share, 0);
}

static void test_a_smaller_difference_needs_more_samples(void)
{
	struct chanwatch w;
	struct chanwatch_move m;
	struct noise_tally home = home_noise(10, 100);

	with_home(&w, &home);
	CHECK(!rounds_until_move(&w, 400, 50, 0, &m));
	CHECK(rounds_until_move(&w, 50, 50, 0, &m));
	CHECK_EQ(m.to, 8);
}

static void test_a_quiet_looking_candidate_needs_evidence_beyond_its_luck(void)
{
	struct chanwatch w;
	struct chanwatch_move m;
	struct noise_tally home = home_noise(4, 100);

	with_home(&w, &home);
	CHECK(!rounds_until_move(&w, 400, 0, 0, &m));
	CHECK(rounds_until_move(&w, 100, 0, 0, &m));
}

static void test_nearly_equal_channels_do_not_trade_places(void)
{
	struct chanwatch w;
	struct chanwatch_move m;
	struct noise_tally home = home_noise(25, 1000);

	with_home(&w, &home);
	CHECK(!rounds_until_move(&w, 5000, 50, 0, &m));
}

static void test_a_second_move_waits_for_the_hold(void)
{
	struct chanwatch w;
	struct chanwatch_move m;
	struct noise_tally noisy = home_noise(46, 100);
	struct noise_tally now_noisy = home_noise(4600, 10000);

	with_home(&w, &noisy);
	CHECK(rounds_until_move(&w, CHANWATCH_MIN_SAMPLES, 0, 0, &m));
	CHECK_EQ(m.to, 8);
	chanwatch_home(&w, 8, &now_noisy);
	for (int i = 0; i < 2000; i++) {
		int8_t dbm[2] = {-95, 0};

		chanwatch_round(&w, dbm, 8, 0);
	}
	CHECK(!chanwatch_decide(&w, 8, -70, CHANWATCH_HOLD_MS - 1, &m));
	CHECK(chanwatch_decide(&w, 8, -70, CHANWATCH_HOLD_MS, &m));
	CHECK_EQ(m.to, 5);
}

static void test_only_noise_near_the_keyboard_signal_counts(void)
{
	struct chanwatch w;
	struct chanwatch_move m;
	struct noise_tally home = {0};

	for (int i = 0; i < 100; i++) {
		noise_add(&home, i < 40 ? -70 : -95);
	}
	primed(&w, &home, -80);
	CHECK(!chanwatch_decide(&w, 5, -40, 0, &m));
	CHECK(!chanwatch_decide(&w, 5, -80, 0, &m));
	CHECK(chanwatch_decide(&w, 5, -60, 0, &m));
}

static void test_noise_older_than_the_decay_time_stops_counting(void)
{
	struct chanwatch w;
	struct chanwatch_move m;
	struct noise_tally home = home_noise(46, 100);

	with_home(&w, &home);
	for (int i = 0; i < 100; i++) {
		chanwatch_round(&w, (const int8_t[]){0, -60}, 5, 0);
	}
	CHECK(!rounds_until_move(&w, CHANWATCH_MIN_SAMPLES, 0, 0, &m));
	/* This round's decay clears home's readings too, so they are added again after it. */
	chanwatch_round(&w, (const int8_t[]){0, -95}, 5, CHANWATCH_DECAY_MS);
	chanwatch_home(&w, 5, &home);
	CHECK(rounds_until_move(&w, CHANWATCH_MIN_SAMPLES - 1, 0, CHANWATCH_DECAY_MS, &m));
	CHECK_EQ(m.to, 8);
}

static void test_no_decision_off_the_candidate_list(void)
{
	struct chanwatch w;
	struct chanwatch_move m;
	struct noise_tally home = home_noise(46, 100);

	primed(&w, &home, -95);
	CHECK(!chanwatch_decide(&w, 38, -70, 0, &m));
}

int main(void)
{
	RUN(test_a_much_quieter_channel_wins_once_it_has_enough_samples);
	RUN(test_a_smaller_difference_needs_more_samples);
	RUN(test_a_quiet_looking_candidate_needs_evidence_beyond_its_luck);
	RUN(test_nearly_equal_channels_do_not_trade_places);
	RUN(test_a_second_move_waits_for_the_hold);
	RUN(test_only_noise_near_the_keyboard_signal_counts);
	RUN(test_noise_older_than_the_decay_time_stops_counting);
	RUN(test_no_decision_off_the_candidate_list);
	return TEST_EXIT();
}
