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
	CHECK_EQ(out[0], 23);
	CHECK_EQ(out[1], 33);
	CHECK_EQ(out[2], 58);
}

static void test_steady_quiet_beats_bursty(void)
{
	uint8_t out[3] = {0};

	fill(CHAN_FIRST, CHAN_LAST, -50);
	fill(40, 40, -85);
	fill(30, 30, -95);
	for (int i = 0; i < 4; i++) {
		s.dbm[30 - CHAN_FIRST][i] = -40;
	}
	chan_pick(&s, out);
	CHECK_EQ(out[0], 40);
}

int main(void)
{
	RUN(test_picks_quiet_channels_at_least_10_apart_avoiding_wifi);
	RUN(test_steady_quiet_beats_bursty);
	return TEST_EXIT();
}
