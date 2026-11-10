#include <stdio.h>
#include "test.h"
#include "diagproto.h"

static size_t load_fixture(const char *name, uint8_t *out, size_t max)
{
	char path[512];
	unsigned int byte;
	size_t n = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", FIXTURES_DIR, name);
	f = fopen(path, "r");
	if (f == NULL) {
		printf("cannot open %s\n", path);
		test_failures++;
		return 0;
	}
	while (n < max && fscanf(f, "%2x", &byte) == 1) {
		out[n++] = byte;
	}
	fclose(f);
	return n;
}

static void test_request_carries_command_seq_and_arg(void)
{
	const uint8_t buf[DIAG_REPORT_LEN] = {DIAG_HISTORY, 9, 0x78, 0x56, 0x34, 0x12};
	struct diag_request req;

	CHECK(diag_parse_request(buf, sizeof(buf), &req));
	CHECK_EQ(req.command, DIAG_HISTORY);
	CHECK_EQ(req.seq, 9);
	CHECK_EQ(req.arg, 0x12345678);
	CHECK(!diag_parse_request(buf, DIAG_REQUEST_LEN - 1, &req));
}

static void test_status_matches_fixture(void)
{
	const struct diag_status s = {
		.uptime_ms = 0x0102030405ull,
		.reset_cause = 1,
		.link_state = 4,
		.channel = 26,
		.own = {26, 30, 34},
		.keyboard_id = {0x70, 0x15, 0xa3, 0x29},
		.last_rssi = -71,
		.last_rx_age_ms = 12,
		.usb_live = true,
		.packets = 1000,
		.invalid = 1,
		.dropped = 2,
		.channel_changes = 3,
		.link_losses = 4,
		.usb_retries = 5,
		.merges = 6,
		.surveys = 7,
		.minute_next = 1500,
		.minute_oldest = 60,
		.events_total = 600,
		.events_oldest = 88,
	};
	uint8_t want[DIAG_PAYLOAD_MAX];
	uint8_t got[DIAG_PAYLOAD_MAX];
	size_t want_len = load_fixture("status.hex", want, sizeof(want));
	size_t got_len = diag_encode_status(got, &s);

	CHECK_EQ(got_len, want_len);
	CHECK_MEM(got, want, want_len);
}

static void test_a_76_byte_payload_goes_out_in_two_parts(void)
{
	const struct diag_request req = {.command = DIAG_STATUS, .seq = 7};
	uint8_t payload[76];
	uint8_t part[DIAG_REPORT_LEN];

	for (size_t i = 0; i < sizeof(payload); i++) {
		payload[i] = i + 1;
	}
	CHECK_EQ(diag_part_count(sizeof(payload)), 2);
	diag_part(part, &req, payload, sizeof(payload), 1);
	CHECK_EQ(part[0], DIAG_STATUS);
	CHECK_EQ(part[1], 7);
	CHECK_EQ(part[2], 1);
	CHECK_EQ(part[3], 2);
	CHECK_EQ(part[DIAG_HEADER_LEN], 61);
	CHECK_EQ(part[DIAG_HEADER_LEN + 15], 76);
	CHECK_EQ(part[DIAG_HEADER_LEN + 16], 0);
}

static void test_an_empty_payload_is_one_part(void)
{
	const struct diag_request req = {.command = DIAG_CLEAR, .seq = 3};
	uint8_t part[DIAG_REPORT_LEN];

	CHECK_EQ(diag_part_count(0), 1);
	diag_part(part, &req, NULL, 0, 0);
	CHECK_EQ(part[0], DIAG_CLEAR);
	CHECK_EQ(part[2], 0);
	CHECK_EQ(part[3], 1);
}

static void test_error_sets_the_flag_and_code(void)
{
	const struct diag_request req = {.command = 0x42, .seq = 5};
	uint8_t part[DIAG_REPORT_LEN];

	diag_error_report(part, &req, DIAG_ERR_UNKNOWN_COMMAND);
	CHECK_EQ(part[0], 0x42 | DIAG_ERROR_FLAG);
	CHECK_EQ(part[1], 5);
	CHECK_EQ(part[3], 1);
	CHECK_EQ(part[DIAG_HEADER_LEN], DIAG_ERR_UNKNOWN_COMMAND);
}

int main(void)
{
	RUN(test_request_carries_command_seq_and_arg);
	RUN(test_status_matches_fixture);
	RUN(test_a_76_byte_payload_goes_out_in_two_parts);
	RUN(test_an_empty_payload_is_one_part);
	RUN(test_error_sets_the_flag_and_code);
	return TEST_EXIT();
}
