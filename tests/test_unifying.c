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

static size_t feed(struct unifying_pairing *p, const uint8_t *f, size_t len, uint32_t now,
		   uint8_t *reply, bool *refused)
{
	return unifying_pairing_rx(p, &receiver, nonce, f, len, now, reply, refused);
}

static void pair_through_request_2(struct unifying_pairing *p)
{
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	request_1(f, 0xe1, 0x05);
	feed(p, f, sizeof(f), 0, reply, &refused);
	request_2(f, 0xe1);
	feed(p, f, sizeof(f), 10, reply, &refused);
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
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX], want[UNIFYING_FRAME_MAX];
	bool refused;
	size_t n;

	memcpy(want, expected, sizeof(want));
	seal(want, sizeof(want));
	request_1(f, 0xe1, 0x05);
	n = feed(&p, f, sizeof(f), 0, reply, &refused);
	CHECK_EQ(n, UNIFYING_FRAME_MAX);
	CHECK_EQ(p.state, UNIFYING_ANSWERED_1);
	CHECK_MEM(reply, want, UNIFYING_FRAME_MAX);
	CHECK_EQ(p.device_type, 0x01);
	CHECK_EQ(p.caps, 0x05);
	CHECK_EQ(p.device_wpid[0], 0x40);
	CHECK_EQ(p.device_wpid[1], 0x8b);
}

static void test_request_1_without_the_unifying_bit_is_refused(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	request_1(f, 0xe1, 0x01);
	CHECK_EQ(feed(&p, f, sizeof(f), 0, reply, &refused), 0);
	CHECK(refused);
	CHECK_EQ(p.state, UNIFYING_IDLE);
}

static void test_request_2_is_answered_with_the_nonce_and_derives_the_key(void)
{
	static const uint8_t expected[UNIFYING_FRAME_MAX] = {
		0xe1, 0x1f, 0x02, 0x11, 0x22, 0x33, 0x44, 0x51, 0x52, 0x53, 0x54,
		0x1e, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	static const uint8_t device_wpid[2] = {0x40, 0x8b};
	static const uint8_t device_nonce[4] = {0xd1, 0xd2, 0xd3, 0xd4};
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX], want[UNIFYING_FRAME_MAX];
	uint8_t key[16];
	bool refused;

	memcpy(want, expected, sizeof(want));
	seal(want, sizeof(want));
	request_1(f, 0xe1, 0x05);
	feed(&p, f, sizeof(f), 0, reply, &refused);
	request_2(f, 0xe1);
	CHECK_EQ(feed(&p, f, sizeof(f), 10, reply, &refused), UNIFYING_FRAME_MAX);
	CHECK_EQ(p.state, UNIFYING_ANSWERED_2);
	CHECK_MEM(reply, want, UNIFYING_FRAME_MAX);
	unifying_derive_key(key, receiver.base, device_wpid, receiver.wpid, device_nonce, nonce);
	CHECK_MEM(p.key, key, 16);
}

static void test_request_3_gets_the_final_response_and_completes(void)
{
	static const uint8_t expected[10] = {0xe1, 0x0f, 0x06, 0xd1, 0xd2, 0xd3,
					     0xd4, 0x11, 0x22, 0x00};
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX], want[10];
	bool refused;

	memcpy(want, expected, sizeof(want));
	seal(want, sizeof(want));
	pair_through_request_2(&p);
	request_3(f, 0xe1);
	CHECK_EQ(feed(&p, f, sizeof(f), 20, reply, &refused), 10);
	CHECK_MEM(reply, want, 10);
	CHECK_EQ(p.state, UNIFYING_PAIRED);
	CHECK_EQ(p.name_len, 7);
	CHECK_MEM(p.name, "MX Keys", 7);
}

static void test_request_3_is_accepted_though_its_id_byte_is_zero(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	pair_through_request_2(&p);
	request_3(f, 0x00);
	CHECK_EQ(feed(&p, f, sizeof(f), 20, reply, &refused), 10);
	CHECK_EQ(p.state, UNIFYING_PAIRED);
	CHECK_MEM(p.name, "MX Keys", 7);
}

static void test_a_repeated_request_gets_no_second_reply(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	request_1(f, 0xe1, 0x05);
	feed(&p, f, sizeof(f), 0, reply, &refused);
	CHECK(!refused);
	CHECK_EQ(feed(&p, f, sizeof(f), 5, reply, &refused), 0);
	CHECK_EQ(p.state, UNIFYING_ANSWERED_1);
}

static void test_a_request_out_of_order_or_with_another_id_is_ignored(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	request_2(f, 0xe1);
	CHECK_EQ(feed(&p, f, sizeof(f), 0, reply, &refused), 0);
	request_1(f, 0xe1, 0x05);
	feed(&p, f, sizeof(f), 0, reply, &refused);
	request_2(f, 0xe2);
	CHECK_EQ(feed(&p, f, sizeof(f), 5, reply, &refused), 0);
	request_3(f, 0xe1);
	CHECK_EQ(feed(&p, f, sizeof(f), 5, reply, &refused), 0);
	CHECK_EQ(p.state, UNIFYING_ANSWERED_1);
}

static void test_a_frame_with_a_bad_checksum_is_ignored(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	request_1(f, 0xe1, 0x05);
	f[21] ^= 0x10;
	CHECK_EQ(feed(&p, f, sizeof(f), 0, reply, &refused), 0);
	CHECK_EQ(p.state, UNIFYING_IDLE);
}

static void test_request_1_with_a_new_id_restarts_with_a_fresh_nonce(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	pair_through_request_2(&p);
	request_1(f, 0xe7, 0x05);
	CHECK_EQ(unifying_pairing_rx(&p, &receiver, other_nonce, f, sizeof(f), 30, reply, &refused),
		 UNIFYING_FRAME_MAX);
	CHECK_EQ(reply[0], 0xe7);
	CHECK_EQ(p.state, UNIFYING_ANSWERED_1);
	request_2(f, 0xe7);
	unifying_pairing_rx(&p, &receiver, nonce, f, sizeof(f), 40, reply, &refused);
	CHECK_MEM(&reply[3], other_nonce, 4);
}

static void test_an_attempt_silent_for_the_timeout_returns_to_idle(void)
{
	struct unifying_pairing p = {0};

	pair_through_request_2(&p);
	CHECK(!unifying_pairing_expired(&p, 10 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK_EQ(p.state, UNIFYING_ANSWERED_2);
	CHECK(unifying_pairing_expired(&p, 11 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK_EQ(p.state, UNIFYING_IDLE);
	CHECK(!unifying_pairing_expired(&p, 20000));
}

static void test_a_completed_pairing_does_not_time_out(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	pair_through_request_2(&p);
	request_3(f, 0xe1);
	feed(&p, f, sizeof(f), 20, reply, &refused);
	CHECK(!unifying_pairing_expired(&p, 60000));
	CHECK_EQ(p.state, UNIFYING_PAIRED);
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
	struct unifying_pairing p = {0};
	uint32_t now = 1000;

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
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	uint8_t held;
	bool refused;

	held = unifying_sweep_channel(&s, &p, 1000);
	CHECK_EQ(held, unifying_pairing_channels[1]);
	request_1(f, 0xe1, 0x05);
	feed(&p, f, sizeof(f), 1005, reply, &refused);
	CHECK_EQ(unifying_sweep_channel(&s, &p, 1100), held);
	request_2(f, 0xe1);
	feed(&p, f, sizeof(f), 1110, reply, &refused);
	request_3(f, 0x00);
	feed(&p, f, sizeof(f), 1120, reply, &refused);
	CHECK_EQ(unifying_sweep_channel(&s, &p, 90000), held);
}

static void test_sweep_resumes_once_an_attempt_expires(void)
{
	struct unifying_sweep s = {0};
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	uint8_t held;
	bool refused;

	held = unifying_sweep_channel(&s, &p, 1000);
	request_1(f, 0xe1, 0x05);
	feed(&p, f, sizeof(f), 1005, reply, &refused);
	CHECK(unifying_pairing_expired(&p, 1006 + UNIFYING_PAIRING_TIMEOUT_MS));
	CHECK(unifying_sweep_channel(&s, &p, 1006 + UNIFYING_PAIRING_TIMEOUT_MS) != held);
}

static void test_request_1_after_the_window_is_ignored(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	request_1(f, 0xe1, 0x05);
	CHECK_EQ(feed(&p, f, sizeof(f), UNIFYING_PAIRING_WINDOW_MS, reply, &refused), 0);
	CHECK(!refused);
	CHECK_EQ(p.state, UNIFYING_IDLE);
}

static void test_an_attempt_started_in_the_window_finishes_after_it(void)
{
	struct unifying_pairing p = {0};
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;
	uint32_t t = UNIFYING_PAIRING_WINDOW_MS - 10;

	request_1(f, 0xe1, 0x05);
	CHECK_EQ(feed(&p, f, sizeof(f), t, reply, &refused), UNIFYING_FRAME_MAX);
	request_2(f, 0xe1);
	CHECK_EQ(feed(&p, f, sizeof(f), t + 20, reply, &refused), UNIFYING_FRAME_MAX);
	request_3(f, 0x00);
	CHECK_EQ(feed(&p, f, sizeof(f), t + 30, reply, &refused), 10);
	CHECK_EQ(p.state, UNIFYING_PAIRED);
}

static void test_sweep_holds_once_the_window_closes(void)
{
	struct unifying_sweep s = {0};
	struct unifying_pairing p = {0};
	uint8_t held = unifying_sweep_channel(&s, &p, UNIFYING_PAIRING_WINDOW_MS - 5);

	CHECK_EQ(unifying_sweep_channel(&s, &p, UNIFYING_PAIRING_WINDOW_MS + 100), held);
	CHECK_EQ(unifying_sweep_channel(&s, &p, UNIFYING_PAIRING_WINDOW_MS + 5000), held);
}

static void test_sweep_continues_in_the_window_after_a_pairing(void)
{
	struct unifying_sweep s = {0};
	struct unifying_pairing p = {.state = UNIFYING_PAIRED};
	uint8_t first = unifying_sweep_channel(&s, &p, 1000);

	CHECK(unifying_sweep_channel(&s, &p, 1000 + UNIFYING_SWEEP_DWELL_MS) != first);
}

static void test_a_restored_device_is_paired_and_a_new_request_still_starts_an_attempt(void)
{
	struct unifying_pairing p = {0}, q = {0};
	struct unifying_device d;
	uint8_t f[UNIFYING_FRAME_MAX], reply[UNIFYING_FRAME_MAX];
	bool refused;

	pair_through_request_2(&p);
	request_3(f, 0x00);
	feed(&p, f, sizeof(f), 20, reply, &refused);
	unifying_device_of(&p, &d);
	unifying_pairing_restore(&q, &d);
	CHECK_EQ(q.state, UNIFYING_PAIRED);
	CHECK_MEM(q.key, p.key, UNIFYING_KEY_LEN);
	CHECK_MEM(q.device_wpid, p.device_wpid, 2);
	request_1(f, 0xe7, 0x05);
	CHECK_EQ(feed(&q, f, sizeof(f), 1000, reply, &refused), UNIFYING_FRAME_MAX);
	CHECK_EQ(q.state, UNIFYING_ANSWERED_1);
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
	RUN(test_sweep_continues_in_the_window_after_a_pairing);
	RUN(test_a_restored_device_is_paired_and_a_new_request_still_starts_an_attempt);
	return TEST_EXIT();
}
