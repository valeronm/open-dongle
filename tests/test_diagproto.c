#include <stdio.h>
#include "fixture.h"
#include "test.h"
#include "btlist.h"
#include "chanpick.h"
#include "chanwatch.h"
#include "diagproto.h"
#include "history.h"
#include "link.h"
#include "protocol.h"
#include "unifying.h"

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

static void test_request_carries_the_bytes_after_arg(void)
{
	const uint8_t buf[DIAG_REPORT_LEN] = {DIAG_FORGET, 9, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
	const uint8_t none[DIAG_REQUEST_DATA_LEN] = {0};
	struct diag_request req;

	CHECK(diag_parse_request(buf, sizeof(buf), &req));
	CHECK_MEM(req.data, &buf[DIAG_REQUEST_LEN], DIAG_REQUEST_DATA_LEN);
	CHECK(diag_parse_request(buf, DIAG_REQUEST_LEN + 2, &req));
	CHECK_MEM(req.data, &buf[DIAG_REQUEST_LEN], 2);
	CHECK_MEM(&req.data[2], none, DIAG_REQUEST_DATA_LEN - 2);
}

static void test_a_request_fixture_carries_the_slot_after_arg(void)
{
	uint8_t buf[DIAG_REPORT_LEN] = {0};
	struct diag_request req;

	CHECK_EQ(load_fixture("request.hex", buf, sizeof(buf)), DIAG_REQUEST_LEN + 1);
	CHECK(diag_parse_request(buf, sizeof(buf), &req));
	CHECK_EQ(req.command, DIAG_HISTORY);
	CHECK_EQ(req.seq, 9);
	CHECK_EQ(req.arg, 1440);
	CHECK_EQ(req.data[0], 1);
}

static void check_status_fixture(const char *name, const struct diag_status *s)
{
	uint8_t want[DIAG_PAYLOAD_MAX];
	uint8_t got[DIAG_PAYLOAD_MAX];
	size_t want_len = load_fixture(name, want, sizeof(want));
	size_t got_len = diag_encode_status(got, s);

	CHECK_EQ(got_len, want_len);
	CHECK_MEM(got, want, want_len);
}

static void test_status_matches_fixture(void)
{
	const struct diag_status s = {
		.firmware_version = {1, 2, 3},
		.boot = {.reset_cause = 1,
			 .bootloader = {0, 6, 0},
			 .softdevice_fwid = 0x00b6,
			 .softdevice_id = 140,
			 .softdevice_version = 6001001},
		.uptime_ms = 0x0102030405ull,
		.link_state = 4,
		.channel = 26,
		.own = {26, 30, 34},
		.forced = {62, 52, 44},
		.keyboard_id = {0x70, 0x15, 0xa3, 0x29},
		.usb_live = true,
		.invalid = 1,
		.dropped = 2,
		.usb_retries = 5,
		.merges = 6,
		.surveys = 7,
		.minute_next = 1500,
		.minute_oldest = 60,
		.events_total = 600,
		.events_oldest = 88,
		.protocol = PROTOCOL_NUPHY,
		.pairing_window = DIAG_WINDOW_OPEN,
		.keyboard_serial = {0xb8, 0xbc, 0xd5, 0xb3},
		.keyboard_type = 1,
		.slot_count = 1,
		.slots = {{.kind = DEVICE_KEYBOARD, .stored = true, .ready = true}},
		.slot = 0,
		.last_rssi = -71,
		.last_rx_age_ms = 12,
		.packets = 1000,
		.gaps = 8,
		.link_losses = 4,
		.latency_average_us = 850,
		.latency_max_us = 4200,
		.name = "MX Keys",
		.battery = {.level = 85, .charging = DEVICE_CHARGING},
	};

	check_status_fixture("status.hex", &s);
}

static void test_each_charging_state_has_its_wire_code(void)
{
	static const struct {
		enum device_charging charging;
		uint8_t code;
	} codes[] = {{DEVICE_DISCHARGING, DIAG_DISCHARGING},
		     {DEVICE_CHARGING, DIAG_CHARGING},
		     {DEVICE_CHARGED, DIAG_CHARGED},
		     {DEVICE_BATTERY_FAULT, DIAG_BATTERY_FAULT},
		     {DEVICE_CHARGE_UNKNOWN, DIAG_CHARGE_UNKNOWN}};
	uint8_t out[DIAG_PAYLOAD_MAX];

	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		struct diag_status s = {.battery = {.level = 40, .charging = codes[i].charging}};
		size_t len = diag_encode_status(out, &s);

		CHECK_EQ(out[len - 2], 40);
		CHECK_EQ(out[len - 1], codes[i].code);
	}
	CHECK_EQ(DIAG_DISCHARGING, 0);
	CHECK_EQ(DIAG_CHARGING, 1);
	CHECK_EQ(DIAG_CHARGED, 2);
	CHECK_EQ(DIAG_BATTERY_FAULT, 3);
}

static void test_a_mouse_slot_status_matches_fixture(void)
{
	const struct diag_status s = {
		.firmware_version = {1, 2, 3},
		.boot = {.reset_cause = 1},
		.uptime_ms = 0x0102030405ull,
		.link_state = 1,
		.usb_live = true,
		.dropped = 2,
		.usb_retries = 5,
		.merges = 6,
		.minute_next = 1500,
		.minute_oldest = 60,
		.events_total = 600,
		.events_oldest = 88,
		.protocol = PROTOCOL_BLUETOOTH,
		.pairing_window = DIAG_WINDOW_PASSKEY_AWAITED,
		.passkey = 123456,
		.passkey_name = "MX Keys",
		.slot_count = 2,
		.slots = {{.kind = DEVICE_KEYBOARD, .stored = true},
			  {.kind = DEVICE_MOUSE, .stored = true, .ready = true}},
		.slot = 1,
		.last_rssi = -60,
		.last_rx_age_ms = 250,
		.packets = 2000,
		.latency_average_us = UINT32_MAX,
		.pairing = DIAG_PAIRING_KNOWN | DIAG_PAIRING_SECURE,
		.name = "MX Master 3S",
		.battery = DEVICE_BATTERY_UNKNOWN,
	};

	check_status_fixture("status-slot1.hex", &s);
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

	diag_error_report(part, &req, DIAG_ERR_UNKNOWN_COMMAND, 0);
	CHECK_EQ(part[0], 0x42 | DIAG_ERROR_FLAG);
	CHECK_EQ(part[1], 5);
	CHECK_EQ(part[3], 1);
	CHECK_EQ(part[DIAG_HEADER_LEN], DIAG_ERR_UNKNOWN_COMMAND);
}

static void test_a_wrong_protocol_error_carries_the_active_protocol(void)
{
	const struct diag_request req = {.command = DIAG_PAIR, .seq = 2};
	uint8_t part[DIAG_REPORT_LEN];

	diag_error_report(part, &req, DIAG_ERR_WRONG_PROTOCOL, PROTOCOL_NUPHY);
	CHECK_EQ(part[0], DIAG_PAIR | DIAG_ERROR_FLAG);
	CHECK_EQ(part[DIAG_HEADER_LEN], DIAG_ERR_WRONG_PROTOCOL);
	CHECK_EQ(part[DIAG_HEADER_LEN + 1], PROTOCOL_NUPHY);
}

static void test_minutes_match_fixture(void)
{
	const struct history_minute m[] = {
		{.packets = {7, 300},
		 .usb_retries = 1,
		 .gaps = {9, 3},
		 .link_losses = {9, 0},
		 .rssi = {{.min = -1, .avg = -1, .max = -1}, {.min = -80, .avg = -70, .max = -60}},
		 .noise = {.peak = -41, .busy = 102, .strong = 5}},
		{.link_losses = {0, 1}},
	};
	uint8_t want[DIAG_PAYLOAD_MAX];
	uint8_t got[DIAG_PAYLOAD_MAX];
	size_t want_len = load_fixture("minutes.hex", want, sizeof(want));
	size_t got_len = diag_encode_minutes(got, 1440, m, 2, 1);

	CHECK_EQ(got_len, want_len);
	CHECK_MEM(got, want, want_len);
}

static void test_events_match_fixture(void)
{
	const struct history_event e[] = {
		{.time_ms = 5000, .type = HISTORY_LINK_LOST, .slot = 0, .a = (uint8_t)-71,
		 .value = 1200, .noise = {.peak = -50, .busy = 2500, .strong = 300}},
		{.time_ms = 7000, .type = HISTORY_SURVEY, .slot = DEVICE_NO_SLOT, .a = 26, .b = 30,
		 .c = 34},
	};
	uint8_t want[DIAG_PAYLOAD_MAX];
	uint8_t got[DIAG_PAYLOAD_MAX];
	size_t want_len = load_fixture("events.hex", want, sizeof(want));
	size_t got_len = diag_encode_events(got, 88, e, 2);

	CHECK_EQ(got_len, want_len);
	CHECK_MEM(got, want, want_len);
}

static void test_a_full_page_fits_the_payload_buffer(void)
{
	static const struct history_minute m[DIAG_MINUTES_PER_PAGE];
	static const struct history_event e[DIAG_EVENTS_PER_PAGE];
	uint8_t out[2 * DIAG_PAYLOAD_MAX];

	CHECK(diag_encode_minutes(out, 0, m, DIAG_MINUTES_PER_PAGE, 0) <= DIAG_PAYLOAD_MAX);
	CHECK(diag_encode_events(out, 0, e, DIAG_EVENTS_PER_PAGE) <= DIAG_PAYLOAD_MAX);
}

static void test_noise_matches_fixture(void)
{
	static struct chan_survey s;
	uint8_t want[DIAG_PAYLOAD_MAX];
	uint8_t got[DIAG_PAYLOAD_MAX];
	size_t want_len = load_fixture("noise.hex", want, sizeof(want));
	size_t got_len;

	for (int i = 0; i < CHAN_COUNT; i++) {
		for (int k = 0; k < CHAN_SAMPLES; k++) {
			s.dbm[i][k] = i == 0 ? -100 : i == CHAN_COUNT - 1 ? -30 : k == 0 ? -50 : -90;
		}
	}
	got_len = diag_encode_noise(got, &s);
	CHECK_EQ(got_len, want_len);
	CHECK_MEM(got, want, want_len);
}

static void test_watch_matches_fixture(void)
{
	static struct chanwatch w;
	const uint8_t channels[] = {2, 41, 62};
	uint8_t want[DIAG_PAYLOAD_MAX];
	uint8_t got[DIAG_PAYLOAD_MAX];
	size_t want_len = load_fixture("watch.hex", want, sizeof(want));
	size_t got_len;

	chanwatch_set_channels(&w, channels, sizeof(channels), 0);
	w.weight[0][0] = 60.0f;
	w.weight[1][0] = 1000.0f;
	w.weight[1][1] = 200.0f;
	w.weight[1][2] = 30.0f;
	w.weight[1][3] = 5.0f;
	w.weight[1][5] = 1.0f;
	w.weight[2][0] = 10.4f;
	w.weight[2][1] = 20.0f;
	w.hold_until_ms = 70000;
	got_len = diag_encode_watch(got, &w, 41, 2, false, 40000);
	CHECK_EQ(got_len, want_len);
	CHECK_MEM(got, want, want_len);
}

static void test_a_paused_watch_past_its_hold_reports_none_left(void)
{
	static struct chanwatch w;
	uint8_t got[DIAG_PAYLOAD_MAX];

	w.hold_until_ms = 1000;
	CHECK_EQ(diag_encode_watch(got, &w, 0, 0, true, 5000), 8);
	CHECK_EQ(got[0], 0);
	CHECK_EQ(got[1], 0);
	CHECK_EQ(got[2], 1);
	CHECK_EQ(got[3] | got[4] | got[5] | got[6], 0);
	CHECK_EQ(got[7], 0);
}

static void test_a_watch_without_a_threshold_counts_nothing(void)
{
	static struct chanwatch w;
	const uint8_t channels[] = {41};
	uint8_t got[DIAG_PAYLOAD_MAX];

	chanwatch_set_channels(&w, channels, sizeof(channels), 0);
	w.weight[0][5] = 100.0f;
	diag_encode_watch(got, &w, 41, 0, false, 0);
	CHECK_EQ(got[10] | got[11], 0);
}

static void test_a_full_watch_fits_the_payload_buffer(void)
{
	static struct chanwatch w;
	uint8_t channels[CHANWATCH_MAX] = {0};
	uint8_t got[DIAG_PAYLOAD_MAX];

	for (int i = 0; i < CHANWATCH_MAX; i++) {
		channels[i] = 2 + i;
	}
	chanwatch_set_channels(&w, channels, CHANWATCH_MAX, 0);
	CHECK(diag_encode_watch(got, &w, 2, 1, false, 0) <= DIAG_PAYLOAD_MAX);
}

static void test_noise_limits_match_fixture(void)
{
	const int8_t got[] = {NOISE_BUSY_DBM, NOISE_STRONG_DBM};
	uint8_t want[sizeof(got) + 1];
	size_t want_len = load_fixture("noise-limits.hex", want, sizeof(want));

	CHECK_EQ(want_len, sizeof(got));
	CHECK_MEM(got, want, sizeof(got));
}

static void test_bucket_floors_match_fixture(void)
{
	int8_t got[NOISE_BUCKETS - 1];
	uint8_t want[NOISE_BUCKETS];
	size_t want_len = load_fixture("bucket-floors.hex", want, sizeof(want));

	for (int b = 1; b < NOISE_BUCKETS; b++) {
		int dbm = INT8_MIN;

		while (noise_bucket((int8_t)dbm) < b) {
			dbm++;
		}
		got[b - 1] = (int8_t)dbm;
	}
	CHECK_EQ(want_len, sizeof(got));
	CHECK_MEM(got, want, sizeof(got));
}

static void test_command_numbers_match_fixture(void)
{
	const uint8_t got[] = {DIAG_STATUS, DIAG_HISTORY, DIAG_EVENTS, DIAG_NOISE,
			       DIAG_REBOOT, DIAG_BOOTLOADER, DIAG_CLEAR, DIAG_FORCE_CHANNELS,
			       DIAG_PAIR, DIAG_PROTOCOL, DIAG_MOVE, DIAG_WATCH,
			       DIAG_DEVICES, DIAG_FORGET};
	uint8_t want[sizeof(got) + 1];
	size_t want_len = load_fixture("commands.hex", want, sizeof(want));

	CHECK_EQ(want_len, sizeof(got));
	CHECK_MEM(got, want, sizeof(got));
}

static void test_event_type_numbers_match_fixture(void)
{
	const uint8_t got[] = {HISTORY_BOOT, HISTORY_PAIRED, HISTORY_LINK_LOST,
			       HISTORY_SURVEY, HISTORY_NOISE, HISTORY_USB_SUSPEND,
			       HISTORY_USB_RESUME, HISTORY_CLEAR, HISTORY_LINK_UP,
			       HISTORY_LINK_MOVED, HISTORY_LINK_GAP,
			       HISTORY_UNIFYING_PAIRED, HISTORY_PAIRING_OPENED,
			       HISTORY_PAIRING_CLOSED, HISTORY_PAIRING_TIMEOUT,
			       HISTORY_RECONNECT, HISTORY_ATTACK, HISTORY_BT_CONNECTED,
			       HISTORY_BT_DISCONNECTED, HISTORY_BT_PAIRED,
			       HISTORY_BT_REFUSED, HISTORY_BT_BOND_LOST, HISTORY_BT_FORGOTTEN,
			       HISTORY_KEYS_DIVERTED, HISTORY_WHEEL_SET,
			       HISTORY_BT_PASSKEY_AWAITED};
	uint8_t want[sizeof(got) + 1];
	size_t want_len = load_fixture("event-types.hex", want, sizeof(want));

	CHECK_EQ(want_len, sizeof(got));
	CHECK_MEM(got, want, sizeof(got));
}

static void test_divert_and_wheel_outcomes_match_fixtures(void)
{
	const uint8_t divert[] = {HISTORY_DIVERT_FINISHED, HISTORY_DIVERT_NO_FEATURE,
				  HISTORY_DIVERT_UNANSWERED};
	const uint8_t wheel[] = {HISTORY_WHEEL_HIRES, HISTORY_WHEEL_NOTCHES, HISTORY_WHEEL_NO_FEATURE,
				 HISTORY_WHEEL_UNANSWERED, HISTORY_WHEEL_REFUSED};
	uint8_t want[sizeof(wheel) + 1];
	size_t want_len = load_fixture("divert-outcomes.hex", want, sizeof(want));

	CHECK_EQ(want_len, sizeof(divert));
	CHECK_MEM(divert, want, sizeof(divert));
	want_len = load_fixture("wheel-outcomes.hex", want, sizeof(want));
	CHECK_EQ(want_len, sizeof(wheel));
	CHECK_MEM(wheel, want, sizeof(wheel));
}

static void test_error_codes_match_fixture(void)
{
	const uint8_t got[] = {DIAG_ERR_UNKNOWN_COMMAND, DIAG_ERR_RADIO, DIAG_ERR_BAD_CHANNELS,
			       DIAG_ERR_STORE, DIAG_ERR_WRONG_PROTOCOL, DIAG_ERR_NOT_MOVABLE,
			       DIAG_ERR_KEY_HELD, DIAG_ERR_LIST_FULL, DIAG_ERR_NO_SUCH_DEVICE};
	uint8_t want[sizeof(got) + 1];
	size_t want_len = load_fixture("error-codes.hex", want, sizeof(want));

	CHECK_EQ(want_len, sizeof(got));
	CHECK_MEM(got, want, sizeof(got));
}

static void test_keyboard_channel_tables_match_fixtures(void)
{
	uint8_t want[UNIFYING_PAIRING_CHANNELS + 1];

	CHECK_EQ(load_fixture("nuphy-fixed-channels.hex", want, sizeof(want)), LINK_FIXED_CHANNELS);
	CHECK_MEM(link_fixed_channels, want, LINK_FIXED_CHANNELS);
	CHECK_EQ(load_fixture("unifying-channels.hex", want, sizeof(want)),
		 UNIFYING_PAIRING_CHANNELS);
	CHECK_MEM(unifying_pairing_channels, want, UNIFYING_PAIRING_CHANNELS);
}

static void test_force_channels_arg_carries_three_channels_or_none(void)
{
	uint8_t channels[3] = {0};

	CHECK(diag_channels_arg(0x002c343e, channels));
	CHECK_EQ(channels[0], 62);
	CHECK_EQ(channels[1], 52);
	CHECK_EQ(channels[2], 44);
	CHECK(!diag_channels_arg(0, channels));
}

static const uint8_t *secure_addr;

static bool is_secure(const uint8_t addr[BTPOLICY_ADDR_LEN])
{
	return memcmp(addr, secure_addr, BTPOLICY_ADDR_LEN) == 0;
}

static void test_devices_match_fixture(void)
{
	struct btlist list = {.count = 2};
	uint8_t got[DIAG_DEVICES_MAX];
	uint8_t want[DIAG_DEVICES_MAX];
	size_t want_len = load_fixture("devices.hex", want, sizeof(want));

	memcpy(list.entries[0].addr, (uint8_t[]){1, 0xd9, 0x9a, 0xa6, 0x49, 0xbb, 0xeb}, 7);
	memcpy(list.entries[0].name, "MX Keys", 7);
	memcpy(list.entries[1].addr, (uint8_t[]){1, 0x5a, 0x3c, 0x11, 0xe0, 0xf2, 0xc8}, 7);
	memcpy(list.entries[1].name, "MX Master 3S", 12);
	list.entries[1].kind = DEVICE_MOUSE;
	list.entries[1].no_passkey = true;
	secure_addr = list.entries[1].addr;
	CHECK_EQ(diag_encode_devices(got, &list,
				     (const uint8_t *const[]){list.entries[0].addr,
							      list.entries[1].addr},
				     is_secure),
		 want_len);
	CHECK_MEM(got, want, want_len);
}

static void test_a_device_ready_in_no_slot_is_sent_with_none(void)
{
	struct btlist list = {0};
	const uint8_t keyboard[BTPOLICY_ADDR_LEN] = {1, 1};
	const uint8_t mouse[BTPOLICY_ADDR_LEN] = {1, 2};
	uint8_t got[DIAG_DEVICES_MAX];

	btlist_paired(&list, keyboard, DEVICE_KEYBOARD, false, "");
	btlist_paired(&list, mouse, DEVICE_MOUSE, true, "");
	secure_addr = mouse;
	CHECK_EQ(diag_encode_devices(got, &list, (const uint8_t *const[]){keyboard, NULL}, is_secure),
		 1 + 2 * DIAG_DEVICE_LEN);
	CHECK_EQ(got[1 + BTPOLICY_ADDR_LEN + 2], DEVICE_NO_SLOT);
	CHECK_EQ(got[1 + DIAG_DEVICE_LEN + BTPOLICY_ADDR_LEN + 2], 0);
	CHECK_EQ(got[1 + BTPOLICY_ADDR_LEN + 1], DIAG_PAIRING_KNOWN | DIAG_PAIRING_SECURE);
	CHECK_EQ(got[1 + DIAG_DEVICE_LEN + BTPOLICY_ADDR_LEN + 1],
		 DIAG_PAIRING_KNOWN | DIAG_PAIRING_PASSKEY);
	diag_encode_devices(got, &list, (const uint8_t *const[]){keyboard, NULL}, NULL);
	CHECK_EQ(got[1 + BTPOLICY_ADDR_LEN + 1], 0);
	CHECK_EQ(got[1 + DIAG_DEVICE_LEN + BTPOLICY_ADDR_LEN + 1], 0);
}

static void test_a_protocol_answers_only_its_commands(void)
{
	static const uint8_t everywhere[] = {
		DIAG_STATUS, DIAG_HISTORY, DIAG_EVENTS, DIAG_REBOOT, DIAG_BOOTLOADER,
		DIAG_CLEAR,  DIAG_PROTOCOL, DIAG_PAIR, DIAG_DEVICES, 0, 200,
	};
	static const uint8_t not_bluetooth[] = {DIAG_NOISE, DIAG_MOVE, DIAG_WATCH};

	for (size_t i = 0; i < sizeof(everywhere); i++) {
		CHECK(diag_command_answered(everywhere[i], PROTOCOL_NUPHY));
		CHECK(diag_command_answered(everywhere[i], PROTOCOL_UNIFYING));
		CHECK(diag_command_answered(everywhere[i], PROTOCOL_BLUETOOTH));
	}
	for (size_t i = 0; i < sizeof(not_bluetooth); i++) {
		CHECK(diag_command_answered(not_bluetooth[i], PROTOCOL_NUPHY));
		CHECK(diag_command_answered(not_bluetooth[i], PROTOCOL_UNIFYING));
		CHECK(!diag_command_answered(not_bluetooth[i], PROTOCOL_BLUETOOTH));
	}
	CHECK(diag_command_answered(DIAG_FORGET, PROTOCOL_BLUETOOTH));
	CHECK(!diag_command_answered(DIAG_FORGET, PROTOCOL_NUPHY));
	CHECK(!diag_command_answered(DIAG_FORGET, PROTOCOL_UNIFYING));
	CHECK(diag_command_answered(DIAG_FORCE_CHANNELS, PROTOCOL_NUPHY));
	CHECK(!diag_command_answered(DIAG_FORCE_CHANNELS, PROTOCOL_UNIFYING));
	CHECK(!diag_command_answered(DIAG_FORCE_CHANNELS, PROTOCOL_BLUETOOTH));
}

int main(void)
{
	RUN(test_keyboard_channel_tables_match_fixtures);
	RUN(test_request_carries_command_seq_and_arg);
	RUN(test_request_carries_the_bytes_after_arg);
	RUN(test_a_request_fixture_carries_the_slot_after_arg);
	RUN(test_status_matches_fixture);
	RUN(test_each_charging_state_has_its_wire_code);
	RUN(test_a_mouse_slot_status_matches_fixture);
	RUN(test_a_76_byte_payload_goes_out_in_two_parts);
	RUN(test_an_empty_payload_is_one_part);
	RUN(test_error_sets_the_flag_and_code);
	RUN(test_a_wrong_protocol_error_carries_the_active_protocol);
	RUN(test_minutes_match_fixture);
	RUN(test_events_match_fixture);
	RUN(test_noise_matches_fixture);
	RUN(test_watch_matches_fixture);
	RUN(test_a_paused_watch_past_its_hold_reports_none_left);
	RUN(test_a_watch_without_a_threshold_counts_nothing);
	RUN(test_a_full_watch_fits_the_payload_buffer);
	RUN(test_noise_limits_match_fixture);
	RUN(test_bucket_floors_match_fixture);
	RUN(test_command_numbers_match_fixture);
	RUN(test_event_type_numbers_match_fixture);
	RUN(test_divert_and_wheel_outcomes_match_fixtures);
	RUN(test_a_full_page_fits_the_payload_buffer);
	RUN(test_error_codes_match_fixture);
	RUN(test_force_channels_arg_carries_three_channels_or_none);
	RUN(test_devices_match_fixture);
	RUN(test_a_device_ready_in_no_slot_is_sent_with_none);
	RUN(test_a_protocol_answers_only_its_commands);
	return TEST_EXIT();
}
