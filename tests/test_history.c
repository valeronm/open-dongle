#include "test.h"
#include "history.h"

#define MINUTE HISTORY_MINUTE_MS

static struct history h;

static void test_a_closed_minute_holds_its_counter_changes_and_signal(void)
{
	struct history_totals t = {.packets = 100, .invalid = 1};
	struct history_minute m;
	uint32_t first;

	history_init(&h, 0, &t);
	t.packets = 400;
	t.invalid = 3;
	t.link_losses = 1;
	t.gaps = 2;
	history_rssi(&h, -60);
	history_rssi(&h, -80);
	history_rssi(&h, -71);
	history_tick(&h, MINUTE - 1, &t);
	CHECK_EQ(history_minutes(&h, 0, &first, &m, 1), 0);
	history_tick(&h, MINUTE, &t);
	CHECK_EQ(history_minutes(&h, 0, &first, &m, 1), 1);
	CHECK_EQ(first, 0);
	CHECK_EQ(m.packets, 300);
	CHECK_EQ(m.invalid, 2);
	CHECK_EQ(m.link_losses, 1);
	CHECK_EQ(m.gaps, 2);
	CHECK_EQ(m.rssi_min, -80);
	CHECK_EQ(m.rssi_max, -60);
	CHECK_EQ(m.rssi_avg, -70);
}

static void test_a_minute_without_packets_has_zero_signal(void)
{
	struct history_totals t = {0};
	struct history_minute m[2];
	uint32_t first;

	history_init(&h, 0, &t);
	history_rssi(&h, -50);
	history_tick(&h, 2 * MINUTE, &t);
	CHECK_EQ(history_minutes(&h, 0, &first, m, 2), 2);
	CHECK_EQ(m[0].rssi_min, -50);
	CHECK_EQ(m[1].rssi_min, 0);
	CHECK_EQ(m[1].rssi_avg, 0);
	CHECK_EQ(m[1].rssi_max, 0);
}

static void test_a_closed_minute_holds_its_noise(void)
{
	struct history_totals t = {0};
	struct noise_tally n = {0};
	struct history_minute m[2];
	uint32_t first;

	history_init(&h, 0, &t);
	noise_add(&n, -96);
	noise_add(&n, -50);
	history_noise(&h, &n, true);
	history_tick(&h, 2 * MINUTE, &t);
	CHECK_EQ(history_minutes(&h, 0, &first, m, 2), 2);
	CHECK_EQ(m[0].noise.peak, -50);
	CHECK_EQ(m[0].noise.busy, 5000);
	CHECK_EQ(m[0].noise.strong, 5000);
	CHECK_EQ(m[1].noise.peak, 0);
	CHECK_EQ(m[1].noise.busy, 0);
}

static void test_counts_saturate(void)
{
	struct history_totals t = {0};
	struct history_minute m;
	uint32_t first;

	history_init(&h, 0, &t);
	t.packets = 70000;
	t.dropped = 300;
	history_tick(&h, MINUTE, &t);
	history_minutes(&h, 0, &first, &m, 1);
	CHECK_EQ(m.packets, 65535);
	CHECK_EQ(m.dropped, 255);
}

static void test_the_ring_keeps_the_newest_day(void)
{
	struct history_totals t = {0};
	struct history_minute m;
	uint32_t first;

	history_init(&h, 0, &t);
	for (uint32_t i = 1; i <= 1500; i++) {
		t.packets = i;
		history_tick(&h, (uint64_t)i * MINUTE, &t);
	}
	CHECK_EQ(h.minute_next, 1500);
	CHECK_EQ(h.minute_oldest, 60);
	CHECK_EQ(history_minutes(&h, 0, &first, &m, 1), 1);
	CHECK_EQ(first, 60);
	CHECK_EQ(history_minutes(&h, 1500, &first, &m, 1), 0);
}

static void test_a_boot_late_in_uptime_starts_at_that_minute(void)
{
	struct history_totals t = {0};

	history_init(&h, 5 * MINUTE + 10, &t);
	CHECK_EQ(h.minute_next, 5);
	CHECK_EQ(h.minute_oldest, 5);
}

static void test_events_keep_the_newest_512_in_order(void)
{
	struct history_totals t = {0};
	struct history_event e[2];
	uint32_t first;

	history_init(&h, 0, &t);
	for (uint32_t i = 0; i < 600; i++) {
		history_event(&h, &(struct history_event){.time_ms = i, .type = HISTORY_LINK_LOST});
	}
	CHECK_EQ(h.events_total, 600);
	CHECK_EQ(h.events_oldest, 88);
	CHECK_EQ(history_events(&h, 0, &first, e, 2), 2);
	CHECK_EQ(first, 88);
	CHECK_EQ(e[0].time_ms, 88);
	CHECK_EQ(e[1].time_ms, 89);
}

static void test_clear_drops_what_is_held_and_keeps_counting(void)
{
	struct history_totals t = {.packets = 50};
	struct history_minute m;
	struct history_event e;
	uint32_t first;

	history_init(&h, 0, &t);
	history_event(&h, &(struct history_event){.type = HISTORY_BOOT});
	t.packets = 80;
	history_tick(&h, 3 * MINUTE, &t);
	history_noise(&h, &(struct noise_tally){.readings = 1, .busy = 1, .strong = 1, .peak = -40},
		      true);
	t = (struct history_totals){0};
	history_clear(&h, 3 * MINUTE + 5, &t);
	CHECK_EQ(history_minutes(&h, 0, &first, &m, 1), 0);
	CHECK_EQ(history_events(&h, 0, &first, &e, 1), 0);
	CHECK_EQ(h.events_total, 1);
	t.packets = 7;
	history_tick(&h, 4 * MINUTE, &t);
	CHECK_EQ(history_minutes(&h, 0, &first, &m, 1), 1);
	CHECK_EQ(first, 3);
	CHECK_EQ(m.packets, 7);
	CHECK_EQ(m.noise.peak, 0);
	history_event(&h, &(struct history_event){.type = HISTORY_LINK_GAP});
	CHECK_EQ(history_events(&h, 0, &first, &e, 1), 1);
	CHECK_EQ(e.noise.peak, 0);
}

static struct history_event last_event(void)
{
	struct history_event e;
	uint32_t first;

	history_events(&h, h.events_total - 1, &first, &e, 1);
	return e;
}

static void test_readings_off_the_home_channel_reach_a_gap_but_not_the_minute(void)
{
	struct history_totals t = {0};
	struct noise_tally n = {0};
	struct history_minute m;
	uint32_t first;

	history_init(&h, 0, &t);
	noise_add(&n, -50);
	history_noise(&h, &n, false);
	history_event(&h, &(struct history_event){.type = HISTORY_LINK_GAP});
	CHECK_EQ(last_event().noise.peak, -50);
	CHECK_EQ(last_event().noise.strong, 10000);
	history_tick(&h, MINUTE, &t);
	CHECK_EQ(history_minutes(&h, 0, &first, &m, 1), 1);
	CHECK_EQ(m.noise.peak, 0);
}

static void test_a_gap_records_the_readings_since_the_last_keyboard_packet(void)
{
	struct history_totals t = {0};
	struct noise_tally before = {0};
	struct noise_tally after = {0};

	history_init(&h, 0, &t);
	noise_add(&before, -30);
	history_noise(&h, &before, true);
	history_rssi(&h, -60);
	noise_add(&after, -96);
	noise_add(&after, -80);
	history_noise(&h, &after, true);
	history_event(&h, &(struct history_event){.type = HISTORY_LINK_GAP});
	CHECK_EQ(last_event().noise.peak, -80);
	CHECK_EQ(last_event().noise.busy, 5000);
	CHECK_EQ(last_event().noise.strong, 0);
	history_rssi(&h, -60);
	history_event(&h, &(struct history_event){.type = HISTORY_LINK_GAP});
	CHECK_EQ(last_event().noise.peak, 0);
}

static void test_a_loss_records_the_readings_and_starts_over(void)
{
	struct history_totals t = {0};
	struct noise_tally n = {0};

	history_init(&h, 0, &t);
	noise_add(&n, -70);
	history_noise(&h, &n, true);
	history_event(&h, &(struct history_event){.type = HISTORY_LINK_LOST});
	CHECK_EQ(last_event().noise.peak, -70);
	CHECK_EQ(last_event().noise.busy, 10000);
	history_event(&h, &(struct history_event){.type = HISTORY_LINK_GAP});
	CHECK_EQ(last_event().noise.peak, 0);
}

static void test_other_events_leave_the_readings_for_the_next_gap(void)
{
	struct history_totals t = {0};
	struct noise_tally n = {0};

	history_init(&h, 0, &t);
	noise_add(&n, -70);
	history_noise(&h, &n, true);
	history_event(&h, &(struct history_event){.type = HISTORY_PAIRED});
	CHECK_EQ(last_event().noise.peak, 0);
	CHECK_EQ(last_event().noise.busy, 0);
	history_event(&h, &(struct history_event){.type = HISTORY_LINK_GAP});
	CHECK_EQ(last_event().noise.peak, -70);
}

int main(void)
{
	RUN(test_a_closed_minute_holds_its_counter_changes_and_signal);
	RUN(test_a_minute_without_packets_has_zero_signal);
	RUN(test_a_closed_minute_holds_its_noise);
	RUN(test_counts_saturate);
	RUN(test_the_ring_keeps_the_newest_day);
	RUN(test_a_boot_late_in_uptime_starts_at_that_minute);
	RUN(test_events_keep_the_newest_512_in_order);
	RUN(test_clear_drops_what_is_held_and_keeps_counting);
	RUN(test_readings_off_the_home_channel_reach_a_gap_but_not_the_minute);
	RUN(test_a_gap_records_the_readings_since_the_last_keyboard_packet);
	RUN(test_a_loss_records_the_readings_and_starts_over);
	RUN(test_other_events_leave_the_readings_for_the_next_gap);
	return TEST_EXIT();
}
