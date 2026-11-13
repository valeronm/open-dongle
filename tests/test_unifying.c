#include <string.h>
#include "test.h"
#include "unifying.h"

static const struct unifying_receiver receiver = {
	.base = {0xa1, 0xb2, 0xc3, 0xd4},
	.device_octet = 0x07,
	.wpid = {0x88, 0x02},
};
static const uint8_t nonce[4] = {0x11, 0x22, 0x33, 0x44};
static const uint8_t other_nonce[4] = {0x99, 0x99, 0x99, 0x99};

static void seal(uint8_t *frame, size_t len)
{
	uint8_t sum = 0;

	for (size_t i = 0; i + 1 < len; i++) {
		sum += frame[i];
	}
	frame[len - 1] = (uint8_t)(0 - sum);
}

static void request_1(uint8_t *f, uint8_t id, uint8_t caps)
{
	static const uint8_t head[] = {0x00, 0x5f, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
				       0x08, 0x40, 0x8b, 0x04, 0x00, 0x01};

	memset(f, 0, UNIFYING_FRAME_MAX);
	memcpy(f, head, sizeof(head));
	f[0] = id;
	f[14] = caps;
	seal(f, UNIFYING_FRAME_MAX);
}

static void request_2(uint8_t *f, uint8_t id)
{
	static const uint8_t head[] = {0x00, 0x5f, 0x02, 0xd1, 0xd2, 0xd3, 0xd4,
				       0x51, 0x52, 0x53, 0x54, 0x1e, 0x40, 0x00, 0x00, 0x01};

	memset(f, 0, UNIFYING_FRAME_MAX);
	memcpy(f, head, sizeof(head));
	f[0] = id;
	seal(f, UNIFYING_FRAME_MAX);
}

static void request_3(uint8_t *f, uint8_t id)
{
	memset(f, 0, UNIFYING_FRAME_MAX);
	f[0] = id;
	f[1] = 0x5f;
	f[2] = 0x03;
	f[3] = 0x01;
	f[4] = 7;
	memcpy(&f[5], "MX Keys", 7);
	seal(f, UNIFYING_FRAME_MAX);
}

static size_t feed_ev(struct unifying_pairing *p, const uint8_t *f, size_t len, uint64_t now,
		      uint8_t *reply, enum unifying_rx_event *ev)
{
	return unifying_pairing_rx(p, &receiver, nonce, f, len, now, reply, ev);
}

static size_t feed(struct unifying_pairing *p, const uint8_t *f, size_t len, uint64_t now,
		   uint8_t *reply)
{
	enum unifying_rx_event ev;

	return feed_ev(p, f, len, now, reply, &ev);
}

static struct unifying_pairing open_pairing(void)
{
	struct unifying_pairing p = {0};

	unifying_pairing_open(&p, 0);
	return p;
}

static void pair_through_request_2(struct unifying_pairing *p)
{
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];

	request_1(f, 0xe1, 0x05);
	feed(p, f, sizeof(f), 0, reply);
	request_2(f, 0xe1);
	feed(p, f, sizeof(f), 10, reply);
}

static void pair_fully(struct unifying_pairing *p)
{
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];

	pair_through_request_2(p);
	request_3(f, 0x00);
	feed(p, f, sizeof(f), 20, reply);
}

static void restore_paired(struct unifying_pairing *p, struct unifying_device *d)
{
	struct unifying_pairing done = open_pairing();

	pair_fully(&done);
	*d = done.device;
	unifying_pairing_restore(p, d);
	unifying_pairing_open(p, 0);
}

static void start_second_attempt(struct unifying_pairing *p)
{
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	request_1(f, 0xe7, 0x05);
	unifying_pairing_rx(p, &receiver, other_nonce, f, sizeof(f), 1000, reply, &ev);
	request_2(f, 0xe7);
	feed(p, f, sizeof(f), 1010, reply);
}

static void test_checksum_is_the_negated_sum(void)
{
	static const uint8_t bytes[] = {0x01, 0x02, 0x03};

	CHECK_EQ(unifying_checksum(bytes, sizeof(bytes)), 0xfa);
}

static void test_checksum_check_tolerates_a_wrong_low_bit(void)
{
	uint8_t f[UNIFYING_FRAME_MAX];

	request_1(f, 0xe1, 0x05);
	CHECK(unifying_checksum_ok(f, sizeof(f)));
	f[21] ^= 0x01;
	CHECK(unifying_checksum_ok(f, sizeof(f)));
	f[21] ^= 0x02;
	CHECK(!unifying_checksum_ok(f, sizeof(f)));
}

static void test_key_permutes_and_masks_the_key_data(void)
{
	static const uint8_t base[4] = {0, 1, 2, 3};
	static const uint8_t device_wpid[2] = {4, 5};
	static const uint8_t dongle_wpid[2] = {6, 7};
	static const uint8_t device_nonce[4] = {8, 9, 10, 11};
	static const uint8_t dongle_nonce[4] = {12, 13, 14, 15};
	static const uint8_t expected[16] = {0x07, 0xfe, 0x00, 0x03, 0x0a, 0xfd, 0x5c, 0x0e,
					     0x08, 0x06, 0xf3, 0x05, 0x0d, 0x5a, 0x04, 0x0b};
	uint8_t key[16];

	unifying_derive_key(key, base, device_wpid, dongle_wpid, device_nonce, dongle_nonce);
	CHECK_MEM(key, expected, 16);
}

static void test_request_1_is_answered_with_the_assigned_address(void)
{
	static const uint8_t expected[UNIFYING_FRAME_MAX] = {
		0xe1, 0x1f, 0x01, 0xa1, 0xb2, 0xc3, 0xd4, 0x07, 0x08, 0x88, 0x02,
		0x00, 0x00, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX], want[UNIFYING_FRAME_MAX];
	size_t n;

	memcpy(want, expected, sizeof(want));
	seal(want, sizeof(want));
	request_1(f, 0xe1, 0x05);
	n = feed(&p, f, sizeof(f), 0, reply);
	CHECK_EQ(n, UNIFYING_FRAME_MAX);
	CHECK_EQ(p.attempt.state, UNIFYING_ANSWERED_1);
	CHECK_MEM(reply, want, UNIFYING_FRAME_MAX);
	CHECK_EQ(p.attempt.device_type, 0x01);
	CHECK_EQ(p.attempt.caps, 0x05);
	CHECK_EQ(p.attempt.dev.wpid[0], 0x40);
	CHECK_EQ(p.attempt.dev.wpid[1], 0x8b);
}

static void test_request_1_without_the_unifying_bit_is_refused(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	request_1(f, 0xe1, 0x01);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 0, reply, &ev), 0);
	CHECK_EQ(ev, UNIFYING_RX_REFUSED);
	CHECK_EQ(p.attempt.state, UNIFYING_IDLE);
}

static void test_request_2_is_answered_with_the_nonce_and_derives_the_key(void)
{
	static const uint8_t expected[UNIFYING_FRAME_MAX] = {
		0xe1, 0x1f, 0x02, 0x11, 0x22, 0x33, 0x44, 0x51, 0x52, 0x53, 0x54,
		0x1e, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	static const uint8_t device_wpid[2] = {0x40, 0x8b};
	static const uint8_t device_nonce[4] = {0xd1, 0xd2, 0xd3, 0xd4};
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX], want[UNIFYING_FRAME_MAX];
	uint8_t key[16];
	enum unifying_rx_event ev;

	memcpy(want, expected, sizeof(want));
	seal(want, sizeof(want));
	request_1(f, 0xe1, 0x05);
	feed_ev(&p, f, sizeof(f), 0, reply, &ev);
	request_2(f, 0xe1);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 10, reply, &ev), UNIFYING_FRAME_MAX);
	CHECK_EQ(ev, UNIFYING_RX_ANSWERED_2);
	CHECK_EQ(p.attempt.state, UNIFYING_ANSWERED_2);
	CHECK_MEM(reply, want, UNIFYING_FRAME_MAX);
	unifying_derive_key(key, receiver.base, device_wpid, receiver.wpid, device_nonce, nonce);
	CHECK_MEM(p.attempt.dev.key, key, 16);
}

static void test_request_3_gets_the_final_response_and_completes(void)
{
	static const uint8_t expected[10] = {0xe1, 0x0f, 0x06, 0xd1, 0xd2, 0xd3,
					     0xd4, 0x11, 0x22, 0x00};
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX], want[10];
	enum unifying_rx_event ev;

	memcpy(want, expected, sizeof(want));
	seal(want, sizeof(want));
	pair_through_request_2(&p);
	request_3(f, 0xe1);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 20, reply, &ev), 10);
	CHECK_EQ(ev, UNIFYING_RX_PAIRED);
	CHECK_MEM(reply, want, 10);
	CHECK(unifying_has_device(&p));
	CHECK_MEM(p.device.key, p.attempt.dev.key, UNIFYING_KEY_LEN);
	CHECK_EQ(p.attempt.state, UNIFYING_IDLE);
	CHECK_EQ(p.attempt.name_len, 7);
	CHECK_MEM(p.attempt.name, "MX Keys", 7);
}

static void test_request_3_is_accepted_though_its_id_byte_is_zero(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];

	pair_through_request_2(&p);
	request_3(f, 0x00);
	CHECK_EQ(feed(&p, f, sizeof(f), 20, reply), 10);
	CHECK(unifying_has_device(&p));
	CHECK_MEM(p.attempt.name, "MX Keys", 7);
}

static void test_a_repeated_request_gets_no_second_reply(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	request_1(f, 0xe1, 0x05);
	feed_ev(&p, f, sizeof(f), 0, reply, &ev);
	CHECK_EQ(ev, UNIFYING_RX_ANSWERED_1);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 5, reply, &ev), 0);
	CHECK_EQ(p.attempt.state, UNIFYING_ANSWERED_1);
}

static void test_a_request_out_of_order_or_with_another_id_is_ignored(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];

	request_2(f, 0xe1);
	CHECK_EQ(feed(&p, f, sizeof(f), 0, reply), 0);
	request_1(f, 0xe1, 0x05);
	feed(&p, f, sizeof(f), 0, reply);
	request_2(f, 0xe2);
	CHECK_EQ(feed(&p, f, sizeof(f), 5, reply), 0);
	request_3(f, 0xe1);
	CHECK_EQ(feed(&p, f, sizeof(f), 5, reply), 0);
	CHECK_EQ(p.attempt.state, UNIFYING_ANSWERED_1);
}

static void test_a_frame_with_a_bad_checksum_is_ignored(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];

	request_1(f, 0xe1, 0x05);
	f[21] ^= 0x10;
	CHECK_EQ(feed(&p, f, sizeof(f), 0, reply), 0);
	CHECK_EQ(p.attempt.state, UNIFYING_IDLE);
}

static void test_request_1_with_a_new_id_restarts_with_a_fresh_nonce(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	pair_through_request_2(&p);
	request_1(f, 0xe7, 0x05);
	CHECK_EQ(unifying_pairing_rx(&p, &receiver, other_nonce, f, sizeof(f), 30, reply, &ev),
		 UNIFYING_FRAME_MAX);
	CHECK_EQ(reply[0], 0xe7);
	CHECK_EQ(p.attempt.state, UNIFYING_ANSWERED_1);
	request_2(f, 0xe7);
	unifying_pairing_rx(&p, &receiver, nonce, f, sizeof(f), 40, reply, &ev);
	CHECK_MEM(&reply[3], other_nonce, 4);
}

static void test_an_attempt_silent_for_the_timeout_returns_to_idle(void)
{
	struct unifying_pairing p = open_pairing();

	pair_through_request_2(&p);
	CHECK(!unifying_pairing_expired(&p, 10 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK_EQ(p.attempt.state, UNIFYING_ANSWERED_2);
	CHECK(unifying_pairing_expired(&p, 11 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK_EQ(p.attempt.state, UNIFYING_IDLE);
	CHECK(!unifying_pairing_expired(&p, 20000));
}

static void test_a_completed_pairing_does_not_time_out(void)
{
	struct unifying_pairing p = open_pairing();

	pair_fully(&p);
	CHECK(!unifying_pairing_expired(&p, 60000));
	CHECK(unifying_has_device(&p));
}

static void test_encrypted_keyboard_frame_is_recognized(void)
{
	uint8_t f[UNIFYING_FRAME_MAX] = {0x07, 0xd3};

	seal(f, sizeof(f));
	CHECK(unifying_is_encrypted_keyboard(f, sizeof(f)));
	f[1] = 0x93;
	seal(f, sizeof(f));
	CHECK(unifying_is_encrypted_keyboard(f, sizeof(f)));
	f[1] = 0xc1;
	seal(f, sizeof(f));
	CHECK(!unifying_is_encrypted_keyboard(f, sizeof(f)));
	f[1] = 0xd3;
	seal(f, sizeof(f));
	CHECK(!unifying_is_encrypted_keyboard(f, 10));
}

static void test_pairing_address_and_channels_are_the_fixed_ones(void)
{
	static const uint8_t address[5] = {0xbb, 0x0a, 0xdc, 0xa5, 0x75};
	static const uint8_t channels[11] = {5, 8, 17, 32, 35, 41, 44, 62, 65, 71, 74};

	CHECK_MEM(unifying_pairing_address, address, 5);
	CHECK_MEM(unifying_pairing_channels, channels, 11);
}

static void test_device_address_is_the_base_then_the_device_octet(void)
{
	static const uint8_t expected[5] = {0xa1, 0xb2, 0xc3, 0xd4, 0x07};
	uint8_t address[5];

	unifying_device_address(&receiver, address);
	CHECK_MEM(address, expected, 5);
}

static void test_idle_sweep_steps_through_the_pairing_channels(void)
{
	struct unifying_sweep s = {0};
	struct unifying_pairing p = open_pairing();
	uint64_t now = 1000;

	CHECK_EQ(unifying_sweep_channel(&s, &p, now), unifying_pairing_channels[1]);
	CHECK_EQ(unifying_sweep_channel(&s, &p, now + UNIFYING_SWEEP_DWELL_MS - 1),
		 unifying_pairing_channels[1]);
	for (int i = 2; i <= UNIFYING_PAIRING_CHANNELS; i++) {
		now += UNIFYING_SWEEP_DWELL_MS;
		CHECK_EQ(unifying_sweep_channel(&s, &p, now),
			 unifying_pairing_channels[i % UNIFYING_PAIRING_CHANNELS]);
	}
}

static void test_sweep_holds_the_channel_from_the_first_request(void)
{
	struct unifying_sweep s = {0};
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	uint8_t held;

	held = unifying_sweep_channel(&s, &p, 1000);
	CHECK_EQ(held, unifying_pairing_channels[1]);
	request_1(f, 0xe1, 0x05);
	feed(&p, f, sizeof(f), 1005, reply);
	CHECK_EQ(unifying_sweep_channel(&s, &p, 1100), held);
	request_2(f, 0xe1);
	feed(&p, f, sizeof(f), 1110, reply);
	request_3(f, 0x00);
	feed(&p, f, sizeof(f), 1120, reply);
	CHECK_EQ(unifying_sweep_channel(&s, &p, 90000), held);
}

static void test_sweep_resumes_once_an_attempt_expires(void)
{
	struct unifying_sweep s = {0};
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	uint8_t held;

	held = unifying_sweep_channel(&s, &p, 1000);
	request_1(f, 0xe1, 0x05);
	feed(&p, f, sizeof(f), 1005, reply);
	CHECK(unifying_pairing_expired(&p, 1006 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK(unifying_sweep_channel(&s, &p, 1006 + UNIFYING_PAIRING_TIMEOUT_MS) != held);
}

static void test_request_1_after_the_window_is_ignored(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	request_1(f, 0xe1, 0x05);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), UNIFYING_PAIRING_WINDOW_MS, reply, &ev), 0);
	CHECK_EQ(ev, UNIFYING_RX_NONE);
	CHECK_EQ(p.attempt.state, UNIFYING_IDLE);
}

static void test_an_attempt_started_in_the_window_finishes_after_it(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	uint64_t t = UNIFYING_PAIRING_WINDOW_MS - 10;

	request_1(f, 0xe1, 0x05);
	CHECK_EQ(feed(&p, f, sizeof(f), t, reply), UNIFYING_FRAME_MAX);
	request_2(f, 0xe1);
	CHECK_EQ(feed(&p, f, sizeof(f), t + 20, reply), UNIFYING_FRAME_MAX);
	request_3(f, 0x00);
	CHECK_EQ(feed(&p, f, sizeof(f), t + 30, reply), 10);
	CHECK(unifying_has_device(&p));
}

static void test_sweep_holds_once_the_window_closes(void)
{
	struct unifying_sweep s = {0};
	struct unifying_pairing p = open_pairing();
	uint8_t held = unifying_sweep_channel(&s, &p, UNIFYING_PAIRING_WINDOW_MS - 5);

	CHECK_EQ(unifying_sweep_channel(&s, &p, UNIFYING_PAIRING_WINDOW_MS + 100), held);
	CHECK_EQ(unifying_sweep_channel(&s, &p, UNIFYING_PAIRING_WINDOW_MS + 5000), held);
}

static void test_request_1_before_pairing_opens_is_ignored(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	request_1(f, 0xe1, 0x05);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 100, reply, &ev), 0);
	CHECK_EQ(ev, UNIFYING_RX_NONE);
}

static void test_the_window_runs_30_s_from_opening(void)
{
	struct unifying_pairing p = {0};

	unifying_pairing_open(&p, 5000);
	CHECK(unifying_pairing_window_open(&p, 5000));
	CHECK(unifying_pairing_window_open(&p, 5000 + 29999));
	CHECK(!unifying_pairing_window_open(&p, 5000 + 30000));
}

static void test_pairing_closes_the_window(void)
{
	struct unifying_pairing p = open_pairing();

	pair_fully(&p);
	CHECK(unifying_has_device(&p));
	CHECK(!unifying_pairing_window_open(&p, 30));
}

static void test_a_restored_device_leaves_the_window_closed(void)
{
	struct unifying_pairing p = open_pairing();
	struct unifying_device d = {0};

	unifying_pairing_restore(&p, &d);
	CHECK(!unifying_pairing_window_open(&p, 10));
}

static void test_a_restored_record_leaves_the_window_closed(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t rec[UNIFYING_RECORD_LEN] = {0};

	unifying_record_restore(&p, rec);
	CHECK(!unifying_pairing_window_open(&p, 10));
}

static void test_a_paired_dongle_sweeps_while_pairing_is_open(void)
{
	static const struct unifying_device d = {0};
	struct unifying_sweep s = {0};
	struct unifying_pairing p;
	uint8_t first;

	unifying_pairing_restore(&p, &d);
	unifying_pairing_open(&p, 1000);
	first = unifying_sweep_channel(&s, &p, 1000);
	CHECK(unifying_sweep_channel(&s, &p, 1000 + UNIFYING_SWEEP_DWELL_MS) != first);
}

static void test_a_paired_dongle_holds_its_channel_with_pairing_closed(void)
{
	static const struct unifying_device d = {0};
	struct unifying_sweep s = {0};
	struct unifying_pairing p;
	uint8_t first;

	unifying_pairing_restore(&p, &d);
	first = unifying_sweep_channel(&s, &p, 1000);
	CHECK_EQ(unifying_sweep_channel(&s, &p, 1000 + 5 * UNIFYING_SWEEP_DWELL_MS), first);
}

static void test_a_restored_device_is_paired_and_a_new_request_still_starts_an_attempt(void)
{
	struct unifying_pairing q;
	struct unifying_device d;
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];

	restore_paired(&q, &d);
	CHECK(unifying_has_device(&q));
	CHECK_MEM(&q.device, &d, sizeof(d));
	request_1(f, 0xe7, 0x05);
	CHECK_EQ(feed(&q, f, sizeof(f), 1000, reply), UNIFYING_FRAME_MAX);
	CHECK_EQ(q.attempt.state, UNIFYING_ANSWERED_1);
}

static void test_a_refused_request_keeps_the_restored_device_paired(void)
{
	struct unifying_pairing p;
	struct unifying_device d;
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	restore_paired(&p, &d);
	request_1(f, 0xe7, 0x01);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 1000, reply, &ev), 0);
	CHECK_EQ(ev, UNIFYING_RX_REFUSED);
	CHECK(unifying_has_device(&p));
	CHECK_MEM(&p.device, &d, sizeof(d));
}

static void test_an_expired_attempt_returns_to_the_restored_device(void)
{
	struct unifying_pairing p;
	struct unifying_device d;

	restore_paired(&p, &d);
	start_second_attempt(&p);
	CHECK(unifying_pairing_expired(&p, 1011 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK(unifying_has_device(&p));
	CHECK_MEM(&p.device, &d, sizeof(d));
}

static void test_an_expired_attempt_can_be_retried_with_its_id(void)
{
	struct unifying_pairing p;
	struct unifying_device d;
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];

	restore_paired(&p, &d);
	request_1(f, 0xe7, 0x05);
	feed(&p, f, sizeof(f), 1000, reply);
	CHECK(unifying_pairing_expired(&p, 1001 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK_EQ(feed(&p, f, sizeof(f), 7000, reply), UNIFYING_FRAME_MAX);
	CHECK_EQ(p.attempt.state, UNIFYING_ANSWERED_1);
}

static void test_the_paired_device_is_kept_while_an_attempt_runs(void)
{
	struct unifying_pairing p;
	struct unifying_device d;

	restore_paired(&p, &d);
	start_second_attempt(&p);
	CHECK_EQ(p.attempt.state, UNIFYING_ANSWERED_2);
	CHECK(unifying_has_device(&p));
	CHECK_MEM(&p.device, &d, sizeof(d));
}

static void test_a_refused_request_during_an_attempt_keeps_the_paired_device(void)
{
	struct unifying_pairing p;
	struct unifying_device d;
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	restore_paired(&p, &d);
	start_second_attempt(&p);
	request_1(f, 0xe8, 0x01);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 1020, reply, &ev), 0);
	CHECK_EQ(ev, UNIFYING_RX_REFUSED);
	CHECK(unifying_has_device(&p));
	CHECK_MEM(&p.device, &d, sizeof(d));
	request_3(f, 0x00);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 1030, reply, &ev), 0);
	CHECK_MEM(&p.device, &d, sizeof(d));
}

static void test_request_1_with_the_id_of_a_completed_attempt_starts_a_new_one(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	pair_fully(&p);
	unifying_pairing_open(&p, 1000);
	request_1(f, 0xe1, 0x05);
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 1000, reply, &ev), UNIFYING_FRAME_MAX);
	CHECK_EQ(ev, UNIFYING_RX_ANSWERED_1);
}

static void test_a_completed_attempt_replaces_the_paired_device(void)
{
	struct unifying_pairing p;
	struct unifying_device old, now;
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	enum unifying_rx_event ev;

	restore_paired(&p, &old);
	start_second_attempt(&p);
	request_3(f, 0x00);
	feed_ev(&p, f, sizeof(f), 1020, reply, &ev);
	CHECK_EQ(ev, UNIFYING_RX_PAIRED);
	CHECK(unifying_has_device(&p));
	CHECK_EQ(feed_ev(&p, f, sizeof(f), 1030, reply, &ev), 0);
	CHECK_EQ(ev, UNIFYING_RX_NONE);
	now = p.device;
	CHECK(memcmp(now.key, old.key, UNIFYING_KEY_LEN) != 0);
	unifying_pairing_open(&p, 2000);
	request_1(f, 0xe8, 0x05);
	feed_ev(&p, f, sizeof(f), 2000, reply, &ev);
	CHECK(unifying_pairing_expired(&p, 2001 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK(unifying_has_device(&p));
	CHECK_MEM(&p.device, &now, sizeof(now));
}

static void test_request_1_after_the_uptime_passes_32_bits_is_ignored(void)
{
	struct unifying_pairing p = open_pairing();
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	uint64_t now = (1ULL << 32) + 10;
	enum unifying_rx_event ev;

	request_1(f, 0xe1, 0x05);
	CHECK_EQ(unifying_pairing_rx(&p, &receiver, nonce, f, sizeof(f), now, reply, &ev), 0);
	CHECK_EQ(p.attempt.state, UNIFYING_IDLE);
}

static const uint8_t aes_key[UNIFYING_KEY_LEN] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
						  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t stream[UNIFYING_AES_BLOCK_LEN] = {
	0x3a, 0x91, 0x07, 0xc4, 0x55, 0xe2, 0x18, 0x6b, 0xf0, 0x2d, 0x8e, 0x73, 0x49, 0xb6, 0x0c, 0xa5};
static const uint8_t press_a[REPORT_KEYBOARD_LEN] = {0x00, 0x04, 0x00, 0x00,
						     0x00, 0x00, 0x00, 0xc9};
static const uint8_t media_volume_up[] = {0x00, 0xc3, 0xe9, 0x00, 0x00,
					  0x00, 0x00, 0x00, 0x00, 0x54};
static const uint8_t media_volume_down[] = {0x00, 0xc3, 0xea, 0x00, 0x00,
					    0x00, 0x00, 0x00, 0x00, 0x53};
static const uint8_t media_mute[] = {0x00, 0xc3, 0xcd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70};
static const uint8_t media_release[] = {0x00, 0xc3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3d};
static const uint8_t keep_alive_1100[] = {0x00, 0x40, 0x04, 0x4c, 0x70};
static const uint8_t set_keep_alive_88[] = {0x00, 0x4f, 0x00, 0x00, 0x58,
					    0x32, 0x14, 0x00, 0x00, 0x13};
static const uint8_t pairing_complete[] = {0x00, 0x4f, 0x06, 0x01, 0x00,
					   0x00, 0x00, 0x00, 0x00, 0xaa};

static uint8_t aes_seen_key[UNIFYING_KEY_LEN];
static uint8_t aes_seen_input[UNIFYING_AES_BLOCK_LEN];
static int aes_calls;

static void fake_aes(const uint8_t key[UNIFYING_KEY_LEN], const uint8_t in[UNIFYING_AES_BLOCK_LEN],
		     uint8_t out[UNIFYING_AES_BLOCK_LEN])
{
	memcpy(aes_seen_key, key, UNIFYING_KEY_LEN);
	memcpy(aes_seen_input, in, UNIFYING_AES_BLOCK_LEN);
	memcpy(out, stream, UNIFYING_AES_BLOCK_LEN);
	aes_calls++;
}

static void keystroke(uint8_t *f, uint32_t counter, const uint8_t plain[REPORT_KEYBOARD_LEN])
{
	memset(f, 0, UNIFYING_FRAME_MAX);
	f[1] = 0xd3;
	for (int i = 0; i < REPORT_KEYBOARD_LEN; i++) {
		f[2 + i] = plain[i] ^ stream[i];
	}
	f[10] = counter >> 24;
	f[11] = counter >> 16;
	f[12] = counter >> 8;
	f[13] = counter;
	seal(f, UNIFYING_FRAME_MAX);
}

static size_t receive_keystrokes_at(struct unifying_link *l, uint32_t counter,
				    const uint8_t plain[REPORT_KEYBOARD_LEN], uint64_t now,
				    struct report out[UNIFYING_KEYSTROKE_REPORTS])
{
	uint8_t f[UNIFYING_FRAME_MAX];

	keystroke(f, counter, plain);
	return unifying_keystroke(l, aes_key, f, sizeof(f), now, fake_aes, out);
}

static bool receive_keystroke_at(struct unifying_link *l, uint32_t counter,
				 const uint8_t plain[REPORT_KEYBOARD_LEN], uint64_t now,
				 struct report *r)
{
	struct report out[UNIFYING_KEYSTROKE_REPORTS];
	size_t n = receive_keystrokes_at(l, counter, plain, now, out);

	CHECK(n <= 1);
	if (n != 0) {
		*r = out[0];
	}
	return n != 0;
}

static bool receive_keystroke(struct unifying_link *l, uint32_t counter,
			      const uint8_t plain[REPORT_KEYBOARD_LEN], struct report *r)
{
	return receive_keystroke_at(l, counter, plain, 0, r);
}

static const uint8_t release[REPORT_KEYBOARD_LEN] = {0x00, 0x00, 0x00, 0x00,
						     0x00, 0x00, 0x00, 0xc9};

static void sync_at(struct unifying_link *l, uint32_t next_counter)
{
	struct report r;

	CHECK(receive_keystroke(l, next_counter - 1, release, &r));
	CHECK_EQ(l->next_counter, next_counter);
}

static void test_keystroke_passes_the_key_and_the_counter_block_to_aes(void)
{
	static const uint8_t expected[UNIFYING_AES_BLOCK_LEN] = {
		0x04, 0x14, 0x1d, 0x1f, 0x27, 0x28, 0x0d, 0x5f, 0x66, 0xf4, 0x59, 0x0a, 0x0d, 0x13, 0x26, 0x0e};
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	sync_at(&l, 0x5f66f459);
	aes_calls = 0;
	CHECK(receive_keystroke(&l, 0x5f66f459, press_a, &r));
	CHECK_EQ(aes_calls, 1);
	CHECK_MEM(aes_seen_input, expected, UNIFYING_AES_BLOCK_LEN);
	CHECK_MEM(aes_seen_key, aes_key, UNIFYING_KEY_LEN);
}

static void test_keystroke_report_is_the_decrypted_modifier_and_keys(void)
{
	static const struct {
		uint8_t plain[REPORT_KEYBOARD_LEN];
		uint8_t want[REPORT_KEYBOARD_LEN];
	} cases[] = {
		{{0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc9},
		 {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}},
		{{0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc9},
		 {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}},
		{{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc9},
		 {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
		{{0x00, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc9},
		 {0x00, 0x00, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00}},
	};
	struct unifying_link l;
	struct report r;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		unifying_link_reset(&l);
		sync_at(&l, 1);
		CHECK(receive_keystroke(&l, 1, cases[i].plain, &r));
		CHECK_EQ(r.kind, REPORT_KEYBOARD);
		CHECK_MEM(r.keyboard, cases[i].want, REPORT_KEYBOARD_LEN);
		CHECK(l.keyboard_down);
	}
}

static void test_a_counter_below_the_next_expected_one_is_refused(void)
{
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	sync_at(&l, 1000);
	CHECK(receive_keystroke(&l, 1000, press_a, &r));
	aes_calls = 0;
	CHECK(!receive_keystroke(&l, 1000, press_a, &r));
	CHECK(!receive_keystroke(&l, 999, press_a, &r));
	CHECK_EQ(aes_calls, 0);
	CHECK(receive_keystroke(&l, 1001, press_a, &r));
	CHECK(!receive_keystroke(&l, 1001, press_a, &r));
	unifying_link_reset(&l);
	aes_calls = 0;
	CHECK(!receive_keystroke(&l, 1000, press_a, &r));
	CHECK_EQ(aes_calls, 1);
}

static const uint8_t forged[REPORT_KEYBOARD_LEN] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void accept_counter_1000(struct unifying_link *l)
{
	struct report r;

	unifying_link_reset(l);
	sync_at(l, 1000);
	CHECK(receive_keystroke(l, 1000, press_a, &r));
}

static void test_counters_inside_the_window_are_accepted_up_to_its_edge(void)
{
	struct unifying_link l;
	struct report r;

	accept_counter_1000(&l);
	CHECK(receive_keystroke(&l, 1001 + UNIFYING_COUNTER_WINDOW - 1, press_a, &r));
	CHECK_EQ(l.next_counter, 1001 + UNIFYING_COUNTER_WINDOW);
	CHECK(!receive_keystroke(&l, 1001 + 2 * UNIFYING_COUNTER_WINDOW, press_a, &r));
	CHECK_EQ(l.next_counter, 1001 + UNIFYING_COUNTER_WINDOW);
	CHECK(receive_keystroke(&l, 1001 + 2 * UNIFYING_COUNTER_WINDOW + 1, press_a, &r));
}

static void test_a_far_jump_alone_is_not_typed(void)
{
	struct unifying_link l;
	struct report r = {.kind = REPORT_SYSTEM};

	accept_counter_1000(&l);
	CHECK(!receive_keystroke(&l, 6000, press_a, &r));
	CHECK_EQ(r.kind, REPORT_SYSTEM);
	CHECK_EQ(l.next_counter, 1001);
}

static void test_a_far_jump_confirmed_by_the_next_counter_is_typed(void)
{
	struct unifying_link l;
	struct report r;

	accept_counter_1000(&l);
	CHECK(!receive_keystroke(&l, 6000, press_a, &r));
	CHECK(receive_keystroke(&l, 6001, press_a, &r));
	CHECK_EQ(r.kind, REPORT_KEYBOARD);
	CHECK_EQ(l.next_counter, 6002);
	CHECK(!receive_keystroke(&l, 6000, press_a, &r));
}

static void test_a_far_jump_followed_by_another_counter_is_dropped(void)
{
	struct unifying_link l;
	struct report r;

	accept_counter_1000(&l);
	CHECK(!receive_keystroke(&l, 6000, press_a, &r));
	CHECK(receive_keystroke(&l, 1005, press_a, &r));
	CHECK(!receive_keystroke(&l, 6001, press_a, &r));
	CHECK_EQ(l.next_counter, 1006);
	CHECK(!receive_keystroke(&l, 7000, press_a, &r));
	CHECK(!receive_keystroke(&l, 8000, press_a, &r));
	CHECK(!receive_keystroke(&l, 7001, press_a, &r));
	CHECK_EQ(l.next_counter, 1006);
}

static void test_a_refused_counter_keeps_the_jump_candidate(void)
{
	struct unifying_link l;
	struct report r;

	accept_counter_1000(&l);
	CHECK(!receive_keystroke(&l, 6000, press_a, &r));
	CHECK(!receive_keystroke(&l, 900, press_a, &r));
	CHECK(receive_keystroke(&l, 6001, press_a, &r));
}

static const uint8_t press_b[REPORT_KEYBOARD_LEN] = {0x00, 0x05, 0x00, 0x00,
						     0x00, 0x00, 0x00, 0xc9};

#define ATTACK_START 10000

static void start_attack(struct unifying_link *l)
{
	struct report r;

	accept_counter_1000(l);
	CHECK(!receive_keystroke_at(l, 1001, forged, ATTACK_START, &r));
}

static void test_under_attack_a_lone_valid_frame_types_nothing(void)
{
	struct unifying_link l;
	struct report r;

	start_attack(&l);
	CHECK(!receive_keystroke_at(&l, 1001, press_a, ATTACK_START + 10, &r));
	CHECK_EQ(l.next_counter, 1001);
}

static void test_under_attack_a_consecutive_pair_types_the_first(void)
{
	struct unifying_link l;
	struct report r = {.kind = REPORT_SYSTEM};

	start_attack(&l);
	CHECK(!receive_keystroke_at(&l, 1001, press_a, ATTACK_START + 10, &r));
	CHECK(receive_keystroke_at(&l, 1002, press_b, ATTACK_START + 20, &r));
	CHECK_EQ(r.kind, REPORT_KEYBOARD);
	CHECK_EQ(r.keyboard[2], 0x04);
	CHECK_EQ(l.next_counter, 1002);
	CHECK(receive_keystroke_at(&l, 1003, press_a, ATTACK_START + 30, &r));
	CHECK_EQ(r.keyboard[2], 0x05);
	CHECK_EQ(l.next_counter, 1003);
}

static void test_under_attack_a_pair_with_a_gap_types_nothing(void)
{
	struct unifying_link l;
	struct report r;

	start_attack(&l);
	CHECK(!receive_keystroke_at(&l, 1001, press_a, ATTACK_START + 10, &r));
	CHECK(!receive_keystroke_at(&l, 1003, press_a, ATTACK_START + 20, &r));
	CHECK(!receive_keystroke_at(&l, 1005, press_a, ATTACK_START + 30, &r));
	CHECK_EQ(l.next_counter, 1001);
}

static void test_under_attack_a_jump_is_refused_even_with_its_successor(void)
{
	struct unifying_link l;
	struct report r;

	start_attack(&l);
	CHECK(!receive_keystroke_at(&l, 6000, press_a, ATTACK_START + 10, &r));
	CHECK(!receive_keystroke_at(&l, 6001, press_b, ATTACK_START + 20, &r));
	CHECK(!receive_keystroke_at(&l, 6002, press_b, ATTACK_START + 30, &r));
	CHECK_EQ(l.next_counter, 1001);
}

static void test_under_attack_a_pair_ending_at_the_window_edge_types_the_first(void)
{
	struct unifying_link l;
	struct report r;

	start_attack(&l);
	CHECK(!receive_keystroke_at(&l, 1001 + UNIFYING_COUNTER_WINDOW - 1, press_a,
				    ATTACK_START + 10, &r));
	CHECK(receive_keystroke_at(&l, 1001 + UNIFYING_COUNTER_WINDOW, press_b, ATTACK_START + 20, &r));
	CHECK_EQ(r.keyboard[2], 0x04);
	CHECK_EQ(l.next_counter, 1001 + UNIFYING_COUNTER_WINDOW);
}

static void test_a_marker_failure_drops_the_pending_frame(void)
{
	struct unifying_link l;
	struct report r;

	start_attack(&l);
	CHECK(!receive_keystroke_at(&l, 1001, press_a, ATTACK_START + 10, &r));
	CHECK(!receive_keystroke_at(&l, 1002, forged, ATTACK_START + 20, &r));
	CHECK(!receive_keystroke_at(&l, 1002, press_a, ATTACK_START + 30, &r));
	CHECK_EQ(l.next_counter, 1001);
}

static void test_a_frame_pending_when_the_attack_ends_is_dropped(void)
{
	struct unifying_link l;
	struct report r;

	start_attack(&l);
	CHECK(!receive_keystroke_at(&l, 1001, press_a, ATTACK_START + 10, &r));
	CHECK(receive_keystroke_at(&l, 1002, press_b, ATTACK_START + UNIFYING_ATTACK_MS, &r));
	CHECK_EQ(r.keyboard[2], 0x05);
	CHECK_EQ(l.next_counter, 1003);
}

static void test_a_forged_frame_after_a_marker_failure_does_not_raise_the_counter(void)
{
	struct unifying_link l;
	struct report r;

	start_attack(&l);
	CHECK(!receive_keystroke_at(&l, 1016, press_a, ATTACK_START + 10, &r));
	CHECK_EQ(l.next_counter, 1001);
}

static void test_under_attack_a_press_confirmed_by_its_release_types_both(void)
{
	static const uint8_t no_keys[REPORT_KEYBOARD_LEN] = {0};
	struct unifying_link l;
	struct report out[UNIFYING_KEYSTROKE_REPORTS];

	start_attack(&l);
	CHECK_EQ(receive_keystrokes_at(&l, 1001, press_a, ATTACK_START + 10, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 1002, release, ATTACK_START + 20, out), 2);
	CHECK_EQ(out[0].keyboard[2], 0x04);
	CHECK_EQ(out[1].kind, REPORT_KEYBOARD);
	CHECK_MEM(out[1].keyboard, no_keys, REPORT_KEYBOARD_LEN);
	CHECK_EQ(l.next_counter, 1003);
	CHECK(!l.keyboard_down);
	CHECK_EQ(receive_keystrokes_at(&l, 1004, press_a, ATTACK_START + 30, out), 0);
}

static void test_under_attack_a_lone_release_is_typed_at_once(void)
{
	struct unifying_link l;
	struct report out[UNIFYING_KEYSTROKE_REPORTS];

	start_attack(&l);
	CHECK_EQ(receive_keystrokes_at(&l, 1001, release, ATTACK_START + 10, out), 1);
	CHECK_EQ(l.next_counter, 1002);
	CHECK_EQ(receive_keystrokes_at(&l, 1001 + 1 + UNIFYING_COUNTER_WINDOW, release,
				       ATTACK_START + 20, out),
		 0);
}

static void test_a_first_lone_press_after_reset_types_nothing(void)
{
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	CHECK(!receive_keystroke(&l, 500, press_a, &r));
	CHECK_EQ(l.next_counter, 0);
}

static void test_a_first_press_confirmed_by_the_next_counter_types_both(void)
{
	struct unifying_link l;
	struct report out[UNIFYING_KEYSTROKE_REPORTS];

	unifying_link_reset(&l);
	CHECK_EQ(receive_keystrokes_at(&l, 500, press_a, 0, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 501, press_b, 0, out), 2);
	CHECK_EQ(out[0].keyboard[2], 0x04);
	CHECK_EQ(out[1].keyboard[2], 0x05);
	CHECK_EQ(l.next_counter, 502);
	CHECK_EQ(receive_keystrokes_at(&l, 502, press_a, 0, out), 1);
	CHECK_EQ(out[0].keyboard[2], 0x04);
}

static void test_a_first_release_after_reset_is_typed_at_once(void)
{
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	CHECK(receive_keystroke(&l, 500, release, &r));
	CHECK_EQ(l.next_counter, 501);
}

static void test_counters_near_the_top_are_refused_before_aes(void)
{
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	aes_calls = 0;
	CHECK(!receive_keystroke(&l, 0xffffffff, release, &r));
	CHECK(!receive_keystroke(&l, 0xffffffff - (1 << 20) + 1, release, &r));
	CHECK_EQ(aes_calls, 0);
	CHECK_EQ(l.next_counter, 0);
	sync_at(&l, 0xfff00000 - UNIFYING_COUNTER_WINDOW);
	aes_calls = 0;
	CHECK(!receive_keystroke(&l, 0xfff00000, release, &r));
	CHECK_EQ(aes_calls, 0);
	CHECK(receive_keystroke(&l, 0xfff00000 - 1, press_a, &r));
	CHECK_EQ(l.next_counter, 0xfff00000);
}

static void test_with_no_counter_an_attack_refuses_every_press(void)
{
	struct unifying_link l;
	struct report out[UNIFYING_KEYSTROKE_REPORTS];
	uint64_t t = 10000;

	unifying_link_reset(&l);
	CHECK_EQ(receive_keystrokes_at(&l, 700, forged, t, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 800, press_a, t + 10, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 801, press_b, t + 20, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 802, press_a, t + 30, out), 0);
	CHECK_EQ(l.next_counter, 0);
	CHECK_EQ(receive_keystrokes_at(&l, 800, press_a, t + UNIFYING_ATTACK_MS, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 801, press_b, t + UNIFYING_ATTACK_MS + 10, out), 2);
	CHECK_EQ(l.next_counter, 802);
}

static void test_with_no_counter_an_attack_still_types_a_release(void)
{
	struct unifying_link l;
	struct report out[UNIFYING_KEYSTROKE_REPORTS];

	unifying_link_reset(&l);
	CHECK_EQ(receive_keystrokes_at(&l, 700, forged, 10000, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 800, release, 10010, out), 1);
	CHECK_EQ(l.next_counter, 801);
}

static const uint8_t connect_22[UNIFYING_FRAME_MAX] = {
	0x07, 0x51, 0x07, 0x04, 0x00, 0x01, 0x01, 0x01, [21] = 0x9a};
static const uint8_t connect_10[] = {0x07, 0x4f, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa3};

static void test_after_a_reconnect_a_counter_below_the_old_one_is_confirmed_by_the_next(void)
{
	struct unifying_link l;
	struct report out[UNIFYING_KEYSTROKE_REPORTS];

	unifying_link_reset(&l);
	sync_at(&l, 3703468736u);
	unifying_link_reconnect(&l, 0x07, connect_22, sizeof(connect_22));
	CHECK(!l.synced);
	CHECK_EQ(receive_keystrokes_at(&l, 0x69c4de07, press_a, 0, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 0x69c4de08, release, 0, out), 2);
	CHECK_EQ(out[0].keyboard[2], 0x04);
	CHECK_EQ(l.next_counter, 0x69c4de09);
}

static void test_a_lone_press_after_a_reconnect_is_held(void)
{
	struct unifying_link l;
	struct report r;

	accept_counter_1000(&l);
	unifying_link_reconnect(&l, 0x07, connect_10, sizeof(connect_10));
	CHECK(!l.synced);
	CHECK(!receive_keystroke(&l, 1001, press_a, &r));
	CHECK(!l.synced);
}

static void test_a_reconnect_drops_a_held_jump(void)
{
	struct unifying_link l;
	struct report r;

	accept_counter_1000(&l);
	CHECK(!receive_keystroke(&l, 6000, press_a, &r));
	unifying_link_reconnect(&l, 0x07, connect_22, sizeof(connect_22));
	CHECK(!l.synced);
	CHECK(!l.has_candidate);
	CHECK(!receive_keystroke(&l, 6001, press_a, &r));
}

static void test_a_reconnect_releases_held_keys(void)
{
	struct unifying_link l;
	struct report r;

	accept_counter_1000(&l);
	CHECK(l.keyboard_down);
	CHECK(unifying_link_reconnect(&l, 0x07, connect_22, sizeof(connect_22)));
	CHECK(!l.keyboard_down);
	CHECK(!unifying_link_reconnect(&l, 0x07, connect_10, sizeof(connect_10)));
	unifying_media_report(&l, media_volume_up, sizeof(media_volume_up), &r);
	CHECK(unifying_link_reconnect(&l, 0x07, connect_10, sizeof(connect_10)));
	CHECK(!l.media_down);
}

static void test_a_reconnect_with_no_key_held_releases_nothing(void)
{
	struct unifying_link l;

	unifying_link_reset(&l);
	sync_at(&l, 1000);
	CHECK(!unifying_link_reconnect(&l, 0x07, connect_22, sizeof(connect_22)));
	CHECK(!l.synced);
}

static void test_other_receiver_frames_leave_the_counter(void)
{
	uint8_t bad[sizeof(connect_10)];
	struct unifying_link l;
	struct report r;

	accept_counter_1000(&l);
	memcpy(bad, connect_10, sizeof(bad));
	bad[sizeof(bad) - 1] ^= 0x10;
	CHECK(!unifying_link_reconnect(&l, 0x07, bad, sizeof(bad)));
	CHECK(!unifying_link_reconnect(&l, 0x07, keep_alive_1100, sizeof(keep_alive_1100)));
	CHECK(!unifying_link_reconnect(&l, 0x07, pairing_complete, sizeof(pairing_complete)));
	CHECK(!unifying_link_reconnect(&l, 0x06, connect_22, sizeof(connect_22)));
	CHECK(!unifying_link_reconnect(&l, 0x06, connect_10, sizeof(connect_10)));
	CHECK(receive_keystroke(&l, 1001, press_a, &r));
}

static void test_an_attack_after_a_reconnect_refuses_every_press(void)
{
	struct unifying_link l;
	struct report out[UNIFYING_KEYSTROKE_REPORTS];

	accept_counter_1000(&l);
	unifying_link_reconnect(&l, 0x07, connect_22, sizeof(connect_22));
	CHECK(!l.synced);
	CHECK_EQ(receive_keystrokes_at(&l, 50, forged, ATTACK_START, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 60, press_a, ATTACK_START + 10, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 61, press_a, ATTACK_START + 20, out), 0);
	CHECK(!l.synced);
}

static void test_the_first_counter_may_be_zero(void)
{
	struct unifying_link l;
	struct report out[UNIFYING_KEYSTROKE_REPORTS];

	unifying_link_reset(&l);
	CHECK_EQ(receive_keystrokes_at(&l, 0, press_a, 0, out), 0);
	CHECK_EQ(receive_keystrokes_at(&l, 1, press_a, 0, out), 2);
	CHECK_EQ(l.next_counter, 2);
	CHECK_EQ(receive_keystrokes_at(&l, 0, press_a, 0, out), 0);
}

static void test_a_plaintext_without_the_marker_byte_is_rejected_and_keeps_the_counter(void)
{
	struct unifying_link l;
	struct report r = {.kind = REPORT_SYSTEM};

	unifying_link_reset(&l);
	sync_at(&l, 1000);
	CHECK(!receive_keystroke(&l, 1000, forged, &r));
	CHECK_EQ(r.kind, REPORT_SYSTEM);
	CHECK(!l.keyboard_down);
	CHECK_EQ(l.next_counter, 1000);
	CHECK(receive_keystroke_at(&l, 1000, press_a, UNIFYING_ATTACK_MS, &r));
	CHECK(l.keyboard_down);
	CHECK_EQ(l.next_counter, 1001);
}

static void test_an_unencrypted_keyboard_frame_is_refused(void)
{
	struct unifying_link l;
	struct report r[UNIFYING_KEYSTROKE_REPORTS];
	uint8_t f[UNIFYING_FRAME_MAX] = {0x00, 0xc1};

	unifying_link_reset(&l);
	aes_calls = 0;
	seal(f, sizeof(f));
	CHECK_EQ(unifying_keystroke(&l, aes_key, f, sizeof(f), 0, fake_aes, r), 0);
	CHECK_EQ(aes_calls, 0);
	CHECK_EQ(l.next_counter, 0);
}

static void test_media_frames_carry_the_usage_in_bytes_2_and_3(void)
{
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	CHECK(unifying_media_report(&l, media_volume_up, sizeof(media_volume_up), &r));
	CHECK_EQ(r.kind, REPORT_CONSUMER);
	CHECK_EQ(r.usage, 0x00e9);
	CHECK(l.media_down);
	CHECK(unifying_media_report(&l, media_volume_down, sizeof(media_volume_down), &r));
	CHECK_EQ(r.usage, 0x00ea);
	CHECK(unifying_media_report(&l, media_mute, sizeof(media_mute), &r));
	CHECK_EQ(r.usage, 0x00cd);
	CHECK(unifying_media_report(&l, media_release, sizeof(media_release), &r));
	CHECK_EQ(r.kind, REPORT_CONSUMER);
	CHECK_EQ(r.usage, 0);
	CHECK(!l.media_down);
}

static void test_a_media_frame_with_a_bad_checksum_or_another_type_is_refused(void)
{
	uint8_t f[sizeof(media_volume_up)];
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	memcpy(f, media_volume_up, sizeof(f));
	f[sizeof(f) - 1] ^= 0x10;
	CHECK(!unifying_media_report(&l, f, sizeof(f), &r));
	f[1] = 0xc1;
	seal(f, sizeof(f));
	CHECK(!unifying_media_report(&l, f, sizeof(f), &r));
	f[1] = 0xc3;
	seal(f, sizeof(f));
	CHECK(!unifying_media_report(&l, f, sizeof(f) - 1, &r));
	CHECK(!l.media_down);
}

static void test_the_led_frame_carries_the_led_bits(void)
{
	static const uint8_t expected[UNIFYING_LED_FRAME_LEN] = {0x00, 0x0e, 0x02, 0x00, 0x00,
								 0x00, 0x00, 0x00, 0x00, 0xf0};
	uint8_t f[UNIFYING_LED_FRAME_LEN];

	CHECK_EQ(unifying_led_frame(0x02, f), UNIFYING_LED_FRAME_LEN);
	CHECK_MEM(f, expected, UNIFYING_LED_FRAME_LEN);
}

static void hold_a_key(struct unifying_link *l)
{
	struct report r;

	sync_at(l, 1);
	receive_keystroke(l, 1, press_a, &r);
}

static void test_keep_alive_frames_set_the_timeout(void)
{
	struct unifying_link l;

	unifying_link_reset(&l);
	CHECK_EQ(l.keepalive_ms, UNIFYING_KEEPALIVE_DEFAULT_MS);
	unifying_link_heard(&l, set_keep_alive_88, sizeof(set_keep_alive_88), 5);
	CHECK_EQ(l.keepalive_ms, 88);
	CHECK_EQ(l.last_ms, 5);
	unifying_link_heard(&l, keep_alive_1100, sizeof(keep_alive_1100), 9);
	CHECK_EQ(l.keepalive_ms, 1100);
	CHECK_EQ(l.last_ms, 9);
}

static void test_other_frames_leave_the_timeout_but_count_as_heard(void)
{
	struct unifying_link l;
	uint8_t bad[sizeof(keep_alive_1100)];

	unifying_link_reset(&l);
	unifying_link_heard(&l, pairing_complete, sizeof(pairing_complete), 7);
	CHECK_EQ(l.keepalive_ms, UNIFYING_KEEPALIVE_DEFAULT_MS);
	CHECK_EQ(l.last_ms, 7);
	memcpy(bad, keep_alive_1100, sizeof(bad));
	bad[sizeof(bad) - 1] ^= 0x10;
	unifying_link_heard(&l, bad, sizeof(bad), 8);
	CHECK_EQ(l.keepalive_ms, UNIFYING_KEEPALIVE_DEFAULT_MS);
}

static void test_a_held_key_goes_stuck_after_twice_the_timeout_once(void)
{
	struct unifying_link l;
	uint64_t t = 10000;

	unifying_link_reset(&l);
	unifying_link_heard(&l, set_keep_alive_88, sizeof(set_keep_alive_88), t);
	hold_a_key(&l);
	CHECK(!unifying_link_silent(&l, t + 176));
	CHECK(unifying_link_silent(&l, t + 177));
	CHECK(!unifying_link_silent(&l, t + 1000));
	CHECK(!l.keyboard_down);
}

static void test_a_frame_heard_in_time_postpones_the_stuck_key(void)
{
	struct unifying_link l;

	unifying_link_reset(&l);
	unifying_link_heard(&l, set_keep_alive_88, sizeof(set_keep_alive_88), 0);
	hold_a_key(&l);
	unifying_link_heard(&l, keep_alive_1100, sizeof(keep_alive_1100), 100);
	CHECK(!unifying_link_silent(&l, 1000));
	CHECK(!unifying_link_silent(&l, 100 + 2 * 1100));
	CHECK(unifying_link_silent(&l, 100 + 2 * 1100 + 1));
}

static void test_nothing_held_is_never_silent(void)
{
	struct unifying_link l;

	unifying_link_reset(&l);
	unifying_link_heard(&l, set_keep_alive_88, sizeof(set_keep_alive_88), 0);
	CHECK(!unifying_link_silent(&l, 100000));
}

static void test_a_release_report_clears_the_held_state(void)
{
	static const uint8_t released[REPORT_KEYBOARD_LEN] = {0x00, 0x00, 0x00, 0x00,
							      0x00, 0x00, 0x00, 0xc9};
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	unifying_link_heard(&l, set_keep_alive_88, sizeof(set_keep_alive_88), 0);
	hold_a_key(&l);
	receive_keystroke(&l, 2, released, &r);
	CHECK(!unifying_link_silent(&l, 100000));
	unifying_media_report(&l, media_volume_up, sizeof(media_volume_up), &r);
	CHECK(unifying_link_silent(&l, 100000));
	unifying_media_report(&l, media_volume_up, sizeof(media_volume_up), &r);
	unifying_media_report(&l, media_release, sizeof(media_release), &r);
	CHECK(!unifying_link_silent(&l, 200000));
}

static void test_a_known_counter_refuses_counters_below_it_and_bounds_the_window(void)
{
	struct unifying_link l;
	struct report r;

	unifying_link_reset(&l);
	sync_at(&l, 5000);
	CHECK(!receive_keystroke(&l, 4999, press_a, &r));
	CHECK(!receive_keystroke(&l, 5000 + UNIFYING_COUNTER_WINDOW, press_a, &r));
	CHECK(receive_keystroke(&l, 5000, press_a, &r));
}

static void test_the_record_is_the_wpid_then_the_key(void)
{
	struct unifying_pairing p;
	struct unifying_device d;
	uint8_t rec[UNIFYING_RECORD_LEN];

	CHECK_EQ(UNIFYING_RECORD_LEN, 18);
	restore_paired(&p, &d);
	unifying_record_of(&p, rec);
	CHECK_MEM(&rec[0], d.wpid, 2);
	CHECK_MEM(&rec[2], d.key, UNIFYING_KEY_LEN);
}

static void test_a_restored_record_pairs_the_device(void)
{
	struct unifying_pairing p = open_pairing(), q;
	uint8_t rec[UNIFYING_RECORD_LEN];

	pair_fully(&p);
	unifying_record_of(&p, rec);
	unifying_record_restore(&q, rec);
	CHECK(unifying_has_device(&q));
	CHECK_MEM(&q.device, &p.device, sizeof(p.device));
	CHECK_EQ(q.attempt.state, UNIFYING_IDLE);
}

static void check_due(struct unifying_led *led, uint32_t taken, uint8_t want)
{
	uint8_t leds = 0xff;

	CHECK(unifying_led_due(led, true, taken, &leds));
	CHECK_EQ(leds, want);
}

static void test_no_led_frame_is_due_before_a_change_or_resend(void)
{
	struct unifying_led led = {0};
	uint8_t leds;

	CHECK(!unifying_led_due(&led, true, 0, &leds));
}

static void test_an_led_change_is_sent_at_once(void)
{
	struct unifying_led led = {0};
	uint8_t leds;

	unifying_led_want(&led, 0x02);
	check_due(&led, 7, 0x02);
	unifying_led_sent(&led, 0x02, 7);
	CHECK(!unifying_led_due(&led, true, 7, &leds));
	CHECK(!unifying_led_due(&led, true, 8, &leds));
}

static void test_the_first_led_state_is_sent_even_when_it_is_zero(void)
{
	struct unifying_led led = {0};

	unifying_led_want(&led, 0x00);
	check_due(&led, 0, 0x00);
}

static void test_an_led_change_while_one_is_outstanding_waits_until_it_is_taken(void)
{
	struct unifying_led led = {0};
	uint8_t leds;

	unifying_led_want(&led, 0x02);
	unifying_led_sent(&led, 0x02, 7);
	unifying_led_want(&led, 0x00);
	unifying_led_want(&led, 0x03);
	CHECK(!unifying_led_due(&led, true, 7, &leds));
	check_due(&led, 8, 0x03);
	unifying_led_sent(&led, 0x03, 8);
	CHECK(!unifying_led_due(&led, true, 9, &leds));
}

static void test_changes_back_to_the_outstanding_state_send_nothing_more(void)
{
	struct unifying_led led = {0};
	uint8_t leds;

	unifying_led_want(&led, 0x02);
	unifying_led_sent(&led, 0x02, 7);
	unifying_led_want(&led, 0x00);
	unifying_led_want(&led, 0x02);
	CHECK(!unifying_led_due(&led, true, 8, &leds));
}

static void test_resends_while_outstanding_are_sent_once_after_it_is_taken(void)
{
	struct unifying_led led = {0};
	uint8_t leds;

	unifying_led_want(&led, 0x02);
	unifying_led_sent(&led, 0x02, 7);
	unifying_led_resend(&led);
	unifying_led_resend(&led);
	CHECK(!unifying_led_due(&led, true, 7, &leds));
	check_due(&led, 8, 0x02);
	unifying_led_sent(&led, 0x02, 8);
	CHECK(!unifying_led_due(&led, true, 9, &leds));
}

static void test_a_resend_sends_the_wanted_state(void)
{
	struct unifying_led led = {0};

	unifying_led_resend(&led);
	check_due(&led, 0, 0x00);
}

static void test_an_led_frame_not_queued_stays_due(void)
{
	struct unifying_led led = {0};

	unifying_led_want(&led, 0x02);
	check_due(&led, 0, 0x02);
	check_due(&led, 0, 0x02);
}

static void test_a_dropped_led_frame_is_sent_again(void)
{
	struct unifying_led led = {0};
	uint8_t leds;

	unifying_led_want(&led, 0x02);
	unifying_led_sent(&led, 0x02, 7);
	unifying_led_dropped(&led);
	check_due(&led, 7, 0x02);
	unifying_led_sent(&led, 0x02, 7);
	CHECK(!unifying_led_due(&led, true, 8, &leds));
	unifying_led_dropped(&led);
	CHECK(!unifying_led_due(&led, true, 8, &leds));
}

static void test_no_led_frame_is_due_without_a_paired_device(void)
{
	struct unifying_led led = {0};
	uint8_t leds;

	unifying_led_want(&led, 0x02);
	CHECK(!unifying_led_due(&led, false, 0, &leds));
	check_due(&led, 0, 0x02);
}

static void session_paired(struct unifying_session *s)
{
	struct unifying_pairing done = open_pairing();
	uint8_t record[UNIFYING_RECORD_LEN];

	pair_fully(&done);
	unifying_record_of(&done, record);
	unifying_session_init(s, &receiver, record, 0);
}

static void test_a_session_without_a_record_is_not_paired(void)
{
	struct unifying_session s;

	unifying_session_init(&s, &receiver, NULL, 0);
	CHECK_EQ(unifying_session_status(&s, 0), UNIFYING_NOT_PAIRED);
}

static void test_a_session_answers_pairing_on_the_frames_pipe(void)
{
	struct unifying_session s;
	struct unifying_out out;
	uint8_t f[UNIFYING_FRAME_MAX];

	unifying_session_init(&s, &receiver, NULL, 0);
	unifying_session_open_pairing(&s, 0);
	CHECK_EQ(unifying_session_status(&s, 10), UNIFYING_PAIRING);
	request_1(f, 0xe1, 0x05);
	unifying_session_frame(&s, UNIFYING_PIPE_PAIRING, f, sizeof(f), -50, 10, nonce, NULL, &out);
	CHECK_EQ(out.event, UNIFYING_RX_ANSWERED_1);
	CHECK_EQ(out.reply_pipe, UNIFYING_PIPE_PAIRING);
	CHECK_EQ(out.reply_len, UNIFYING_FRAME_MAX);
}

static void test_a_refused_request_gets_no_reply_from_the_session(void)
{
	struct unifying_session s;
	struct unifying_out out;
	uint8_t f[UNIFYING_FRAME_MAX];

	unifying_session_init(&s, &receiver, NULL, 0);
	unifying_session_open_pairing(&s, 0);
	request_1(f, 0xe1, 0x01);
	unifying_session_frame(&s, UNIFYING_PIPE_PAIRING, f, sizeof(f), 0, 10, nonce, NULL, &out);
	CHECK_EQ(out.event, UNIFYING_RX_REFUSED);
	CHECK_EQ(out.reply_len, 0);
}

static void test_completing_pairing_through_the_session_closes_the_window(void)
{
	struct unifying_session s;
	struct unifying_out out;
	uint8_t f[UNIFYING_FRAME_MAX];

	unifying_session_init(&s, &receiver, NULL, 0);
	unifying_session_open_pairing(&s, 0);
	request_1(f, 0xe1, 0x05);
	unifying_session_frame(&s, UNIFYING_PIPE_PAIRING, f, sizeof(f), 0, 0, nonce, NULL, &out);
	request_2(f, 0xe1);
	unifying_session_frame(&s, UNIFYING_PIPE_DEVICE, f, sizeof(f), 0, 10, nonce, fake_aes, &out);
	CHECK_EQ(out.event, UNIFYING_RX_ANSWERED_2);
	request_3(f, 0x00);
	unifying_session_frame(&s, UNIFYING_PIPE_DEVICE, f, sizeof(f), 0, 20, nonce, fake_aes, &out);
	CHECK_EQ(out.event, UNIFYING_RX_PAIRED);
	CHECK(unifying_session_status(&s, 30) != UNIFYING_PAIRING);
}

static void test_a_connect_frame_on_the_receiver_pipe_resends_the_leds(void)
{
	struct unifying_session s;
	struct unifying_out out;

	session_paired(&s);
	unifying_session_tick(&s, 0, true, 0x02, 0, &out);
	CHECK(out.send_led);
	unifying_session_led_queued(&s, out.leds, 0);
	unifying_session_tick(&s, 10, false, 0x02, 1, &out);
	CHECK(!out.send_led);
	unifying_session_frame(&s, UNIFYING_PIPE_RECEIVER, connect_10, sizeof(connect_10), 0, 20,
			       nonce, fake_aes, &out);
	unifying_session_tick(&s, 30, false, 0x02, 1, &out);
	CHECK(out.send_led);
	CHECK_EQ(out.leds, 0x02);
}

static void test_a_heard_paired_keyboard_is_connected_then_silent(void)
{
	struct unifying_session s;
	struct unifying_out out;

	session_paired(&s);
	CHECK_EQ(unifying_session_status(&s, 0), UNIFYING_SILENT);
	unifying_session_frame(&s, UNIFYING_PIPE_DEVICE, keep_alive_1100, sizeof(keep_alive_1100),
			       -60, 1000, nonce, fake_aes, &out);
	CHECK_EQ(s.rssi, -60);
	CHECK_EQ(unifying_session_status(&s, 1000 + 2 * s.link.keepalive_ms - 1),
		 UNIFYING_CONNECTED);
	CHECK_EQ(unifying_session_status(&s, 1000 + 2 * s.link.keepalive_ms), UNIFYING_SILENT);
}

static void test_the_tick_moves_the_channel_while_pairing_is_open(void)
{
	struct unifying_session s;
	struct unifying_out out;

	unifying_session_init(&s, &receiver, NULL, 0);
	unifying_session_open_pairing(&s, 0);
	unifying_session_tick(&s, UNIFYING_SWEEP_DWELL_MS, false, 0, 0, &out);
	CHECK(out.set_channel);
	CHECK_EQ(out.channel, s.channel);
	unifying_session_tick(&s, UNIFYING_SWEEP_DWELL_MS + 1, false, 0, 0, &out);
	CHECK(!out.set_channel);
}

static void test_a_session_types_a_keystroke_from_the_device_pipe(void)
{
	struct unifying_session s;
	struct unifying_out out;
	uint8_t f[UNIFYING_FRAME_MAX];

	session_paired(&s);
	keystroke(f, 0x5f66f458, release);
	unifying_session_frame(&s, UNIFYING_PIPE_DEVICE, f, sizeof(f), 0, 10, nonce, fake_aes, &out);
	CHECK_EQ(out.reports, 1);
	keystroke(f, 0x5f66f459, press_a);
	unifying_session_frame(&s, UNIFYING_PIPE_DEVICE, f, sizeof(f), 0, 20, nonce, fake_aes, &out);
	CHECK_EQ(out.reports, 1);
	CHECK_EQ(out.report[0].kind, REPORT_KEYBOARD);
	CHECK_EQ(out.report[0].keyboard[2], 0x04);
}

int main(void)
{
	RUN(test_checksum_is_the_negated_sum);
	RUN(test_checksum_check_tolerates_a_wrong_low_bit);
	RUN(test_key_permutes_and_masks_the_key_data);
	RUN(test_request_1_is_answered_with_the_assigned_address);
	RUN(test_request_1_without_the_unifying_bit_is_refused);
	RUN(test_request_2_is_answered_with_the_nonce_and_derives_the_key);
	RUN(test_request_3_gets_the_final_response_and_completes);
	RUN(test_request_3_is_accepted_though_its_id_byte_is_zero);
	RUN(test_a_repeated_request_gets_no_second_reply);
	RUN(test_a_request_out_of_order_or_with_another_id_is_ignored);
	RUN(test_a_frame_with_a_bad_checksum_is_ignored);
	RUN(test_request_1_with_a_new_id_restarts_with_a_fresh_nonce);
	RUN(test_an_attempt_silent_for_the_timeout_returns_to_idle);
	RUN(test_a_completed_pairing_does_not_time_out);
	RUN(test_encrypted_keyboard_frame_is_recognized);
	RUN(test_pairing_address_and_channels_are_the_fixed_ones);
	RUN(test_device_address_is_the_base_then_the_device_octet);
	RUN(test_idle_sweep_steps_through_the_pairing_channels);
	RUN(test_sweep_holds_the_channel_from_the_first_request);
	RUN(test_sweep_resumes_once_an_attempt_expires);
	RUN(test_request_1_after_the_window_is_ignored);
	RUN(test_an_attempt_started_in_the_window_finishes_after_it);
	RUN(test_sweep_holds_once_the_window_closes);
	RUN(test_request_1_before_pairing_opens_is_ignored);
	RUN(test_the_window_runs_30_s_from_opening);
	RUN(test_pairing_closes_the_window);
	RUN(test_a_restored_device_leaves_the_window_closed);
	RUN(test_a_restored_record_leaves_the_window_closed);
	RUN(test_a_paired_dongle_sweeps_while_pairing_is_open);
	RUN(test_a_paired_dongle_holds_its_channel_with_pairing_closed);
	RUN(test_a_restored_device_is_paired_and_a_new_request_still_starts_an_attempt);
	RUN(test_a_refused_request_keeps_the_restored_device_paired);
	RUN(test_an_expired_attempt_returns_to_the_restored_device);
	RUN(test_an_expired_attempt_can_be_retried_with_its_id);
	RUN(test_the_paired_device_is_kept_while_an_attempt_runs);
	RUN(test_a_refused_request_during_an_attempt_keeps_the_paired_device);
	RUN(test_request_1_with_the_id_of_a_completed_attempt_starts_a_new_one);
	RUN(test_a_completed_attempt_replaces_the_paired_device);
	RUN(test_request_1_after_the_uptime_passes_32_bits_is_ignored);
	RUN(test_keystroke_passes_the_key_and_the_counter_block_to_aes);
	RUN(test_keystroke_report_is_the_decrypted_modifier_and_keys);
	RUN(test_a_counter_below_the_next_expected_one_is_refused);
	RUN(test_counters_inside_the_window_are_accepted_up_to_its_edge);
	RUN(test_a_far_jump_alone_is_not_typed);
	RUN(test_a_far_jump_confirmed_by_the_next_counter_is_typed);
	RUN(test_a_far_jump_followed_by_another_counter_is_dropped);
	RUN(test_a_refused_counter_keeps_the_jump_candidate);
	RUN(test_under_attack_a_lone_valid_frame_types_nothing);
	RUN(test_under_attack_a_consecutive_pair_types_the_first);
	RUN(test_under_attack_a_pair_with_a_gap_types_nothing);
	RUN(test_under_attack_a_jump_is_refused_even_with_its_successor);
	RUN(test_under_attack_a_pair_ending_at_the_window_edge_types_the_first);
	RUN(test_a_marker_failure_drops_the_pending_frame);
	RUN(test_a_frame_pending_when_the_attack_ends_is_dropped);
	RUN(test_a_forged_frame_after_a_marker_failure_does_not_raise_the_counter);
	RUN(test_under_attack_a_press_confirmed_by_its_release_types_both);
	RUN(test_under_attack_a_lone_release_is_typed_at_once);
	RUN(test_a_first_lone_press_after_reset_types_nothing);
	RUN(test_a_first_press_confirmed_by_the_next_counter_types_both);
	RUN(test_a_first_release_after_reset_is_typed_at_once);
	RUN(test_counters_near_the_top_are_refused_before_aes);
	RUN(test_with_no_counter_an_attack_refuses_every_press);
	RUN(test_with_no_counter_an_attack_still_types_a_release);
	RUN(test_after_a_reconnect_a_counter_below_the_old_one_is_confirmed_by_the_next);
	RUN(test_a_lone_press_after_a_reconnect_is_held);
	RUN(test_a_reconnect_drops_a_held_jump);
	RUN(test_a_reconnect_releases_held_keys);
	RUN(test_a_reconnect_with_no_key_held_releases_nothing);
	RUN(test_other_receiver_frames_leave_the_counter);
	RUN(test_an_attack_after_a_reconnect_refuses_every_press);
	RUN(test_the_first_counter_may_be_zero);
	RUN(test_a_plaintext_without_the_marker_byte_is_rejected_and_keeps_the_counter);
	RUN(test_an_unencrypted_keyboard_frame_is_refused);
	RUN(test_media_frames_carry_the_usage_in_bytes_2_and_3);
	RUN(test_a_media_frame_with_a_bad_checksum_or_another_type_is_refused);
	RUN(test_the_led_frame_carries_the_led_bits);
	RUN(test_keep_alive_frames_set_the_timeout);
	RUN(test_other_frames_leave_the_timeout_but_count_as_heard);
	RUN(test_a_held_key_goes_stuck_after_twice_the_timeout_once);
	RUN(test_a_frame_heard_in_time_postpones_the_stuck_key);
	RUN(test_nothing_held_is_never_silent);
	RUN(test_a_release_report_clears_the_held_state);
	RUN(test_a_known_counter_refuses_counters_below_it_and_bounds_the_window);
	RUN(test_the_record_is_the_wpid_then_the_key);
	RUN(test_a_restored_record_pairs_the_device);
	RUN(test_no_led_frame_is_due_before_a_change_or_resend);
	RUN(test_an_led_change_is_sent_at_once);
	RUN(test_the_first_led_state_is_sent_even_when_it_is_zero);
	RUN(test_an_led_change_while_one_is_outstanding_waits_until_it_is_taken);
	RUN(test_changes_back_to_the_outstanding_state_send_nothing_more);
	RUN(test_resends_while_outstanding_are_sent_once_after_it_is_taken);
	RUN(test_a_resend_sends_the_wanted_state);
	RUN(test_an_led_frame_not_queued_stays_due);
	RUN(test_a_dropped_led_frame_is_sent_again);
	RUN(test_no_led_frame_is_due_without_a_paired_device);
	RUN(test_a_session_without_a_record_is_not_paired);
	RUN(test_a_session_answers_pairing_on_the_frames_pipe);
	RUN(test_a_refused_request_gets_no_reply_from_the_session);
	RUN(test_completing_pairing_through_the_session_closes_the_window);
	RUN(test_a_connect_frame_on_the_receiver_pipe_resends_the_leds);
	RUN(test_a_heard_paired_keyboard_is_connected_then_silent);
	RUN(test_the_tick_moves_the_channel_while_pairing_is_open);
	RUN(test_a_session_types_a_keystroke_from_the_device_pipe);
	return TEST_EXIT();
}
