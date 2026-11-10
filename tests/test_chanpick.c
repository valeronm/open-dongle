#include "test.h"
#include "chanpick.h"

static struct chan_survey s;

static void fill(int first, int last, int8_t dbm)
{
	for (int ch = first; ch <= last; ch++) {
		for (int i = 0; i < CHAN_SAMPLES; i++) {
			s.dbm[ch - CHAN_FIRST][i] = dbm;
		}
	}
}

static void test_picks_quiet_channels_at_least_10_apart_avoiding_wifi(void)
{
	uint8_t out[3] = {0};

	fill(CHAN_FIRST, CHAN_LAST, -95);
	fill(2, 22, -40);
	fill(37, 57, -60);
	chan_pick(&s, out);
	CHECK_EQ(out[0], 28);
	CHECK_EQ(out[1], 63);
	CHECK_EQ(out[2], 73);
}

static void test_steady_quiet_beats_bursty(void)
{
	uint8_t out[3] = {0};

	fill(CHAN_FIRST, CHAN_LAST, -50);
	fill(60, 80, -85);
	fill(20, 40, -95);
	for (int ch = 20; ch <= 40; ch++) {
		for (int i = 0; i < 4; i++) {
			s.dbm[ch - CHAN_FIRST][i] = -40;
		}
	}
	chan_pick(&s, out);
	CHECK_EQ(out[0], 65);
}

static void test_a_quiet_channel_inside_a_busy_band_is_not_picked(void)
{
	uint8_t out[3] = {0};

	fill(CHAN_FIRST, CHAN_LAST, -95);
	fill(2, 30, -60);
	fill(16, 16, -100);
	chan_pick(&s, out);
	CHECK_EQ(out[0], 36);
}

static void test_pairing_and_ble_advertising_channels_are_never_picked(void)
{
	uint8_t out[3] = {0};

	fill(CHAN_FIRST, CHAN_LAST, -60);
	fill(2, 7, -100);
	fill(21, 31, -100);
	fill(75, 80, -100);
	chan_pick(&s, out);
	for (int i = 0; i < 3; i++) {
		CHECK(out[i] != 2 && out[i] != 26 && out[i] != 80);
	}
}

int main(void)
{
	RUN(test_picks_quiet_channels_at_least_10_apart_avoiding_wifi);
	RUN(test_steady_quiet_beats_bursty);
	RUN(test_a_quiet_channel_inside_a_busy_band_is_not_picked);
	RUN(test_pairing_and_ble_advertising_channels_are_never_picked);
	return TEST_EXIT();
}
