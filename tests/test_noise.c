#include "test.h"
#include "noise.h"

static void test_noise_counts_readings_at_both_levels(void)
{
	struct noise_tally t = {0};
	struct noise_record r;

	for (int i = 0; i < 97; i++) {
		noise_add(&t, -96);
	}
	noise_add(&t, -85);
	noise_add(&t, -70);
	noise_add(&t, -60);
	r = noise_record(&t);
	CHECK_EQ(t.readings, 100);
	CHECK_EQ(r.peak, -60);
	CHECK_EQ(r.busy, 300);
	CHECK_EQ(r.strong, 100);
}

static void test_noise_without_readings_records_zero(void)
{
	struct noise_tally t = {0};
	struct noise_record r = noise_record(&t);

	CHECK_EQ(r.peak, 0);
	CHECK_EQ(r.busy, 0);
	CHECK_EQ(r.strong, 0);
}

static void test_merged_noise_keeps_the_louder_peak(void)
{
	struct noise_tally a = {0};
	struct noise_tally b = {0};
	struct noise_tally none = {0};

	noise_add(&a, -90);
	noise_add(&b, -50);
	noise_add(&b, -95);
	noise_merge(&a, &b);
	noise_merge(&a, &none);
	CHECK_EQ(a.readings, 3);
	CHECK_EQ(a.busy, 1);
	CHECK_EQ(a.strong, 1);
	CHECK_EQ(a.peak, -50);
	noise_merge(&none, &a);
	CHECK_EQ(none.peak, -50);
}

static void test_noise_shares_round_down(void)
{
	struct noise_tally t = {0};

	noise_add(&t, -80);
	noise_add(&t, -96);
	noise_add(&t, -96);
	CHECK_EQ(noise_record(&t).busy, 3333);
}

int main(void)
{
	RUN(test_noise_counts_readings_at_both_levels);
	RUN(test_noise_without_readings_records_zero);
	RUN(test_merged_noise_keeps_the_louder_peak);
	RUN(test_noise_shares_round_down);
	return TEST_EXIT();
}
