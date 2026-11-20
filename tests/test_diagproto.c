#include <stdio.h>
#include "fixture.h"
#include "test.h"
#include "btlist.h"
#include "chanpick.h"
#include "chanwatch.h"
#include "diagwire.h"
#include "history.h"
#include "link.h"
#include "protocol.h"
#include "unifying.h"

static void check_status_fixture(const char *name, const struct diag_status *s)
{
	uint8_t want[DIAG_PAYLOAD_MAX];
	uint8_t got[DIAG_PAYLOAD_MAX];
	size_t want_len = load_fixture(name, want, sizeof(want));
	size_t got_len = diag_encode_status(got, sizeof(got), s);

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
		.slots = {{.kind = DEVICE_KIND_KEYBOARD, .stored = true, .ready = true}},
		.slot = 0,
		.last_rssi = -71,
		.last_rx_age_ms = 12,
		.packets = 1000,
		.gaps = 8,
		.link_losses = 4,
		.latency_average_us = 850,
		.latency_max_us = 4200,
		.name = "MX Keys",
		.battery = {.level = 85, .charging = DEVICE_CHARGING_CHARGING},
	};

	check_status_fixture("status.hex", &s);
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
		.slots = {{.kind = DEVICE_KIND_KEYBOARD, .stored = true},
			  {.kind = DEVICE_KIND_MOUSE, .stored = true, .ready = true}},
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

static size_t encode_watch(uint8_t *out, const struct chanwatch *w, uint8_t home,
			   uint8_t threshold, bool paused, uint64_t now_ms)
{
	const struct diag_watch_head head = {.home = home,
					     .threshold = threshold,
					     .paused = paused,
					     .hold_left_ms = chanwatch_hold_left(w, now_ms)};
	struct diag_watch_channel channels[CHANWATCH_MAX];
	size_t n = chanwatch_report(w, threshold, channels);

	return diag_encode_watch(out, DIAG_PAYLOAD_MAX, &head, channels, n);
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
	got_len = encode_watch(got, &w, 41, 2, false, 40000);
	CHECK_EQ(got_len, want_len);
	CHECK_MEM(got, want, want_len);
}

static void test_a_paused_watch_past_its_hold_reports_none_left(void)
{
	static struct chanwatch w;
	uint8_t got[DIAG_PAYLOAD_MAX];

	w.hold_until_ms = 1000;
	CHECK_EQ(encode_watch(got, &w, 0, 0, true, 5000), 8);
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
	encode_watch(got, &w, 41, 0, false, 0);
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
	CHECK(encode_watch(got, &w, 2, 1, false, 0) <= DIAG_PAYLOAD_MAX);
}

static const uint8_t *secure_addr;

static bool is_secure(const uint8_t addr[BTPOLICY_ADDR_LEN])
{
	return memcmp(addr, secure_addr, BTPOLICY_ADDR_LEN) == 0;
}

static size_t encode_devices(uint8_t *out, const struct btlist *l,
			     const uint8_t *const connected[DEVICE_SLOTS],
			     bool (*secure)(const uint8_t addr[BTPOLICY_ADDR_LEN]))
{
	struct diag_device devices[BTPOLICY_STORED_MAX];
	size_t n = btlist_devices(l, connected, secure, devices);

	return diag_encode_devices(out, DIAG_DEVICES_MAX, devices, n);
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
	list.entries[1].kind = DEVICE_KIND_MOUSE;
	list.entries[1].no_passkey = true;
	secure_addr = list.entries[1].addr;
	CHECK_EQ(encode_devices(got, &list,
				(const uint8_t *const[]){list.entries[0].addr, list.entries[1].addr},
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

	btlist_paired(&list, keyboard, DEVICE_KIND_KEYBOARD, false, "");
	btlist_paired(&list, mouse, DEVICE_KIND_MOUSE, true, "");
	secure_addr = mouse;
	CHECK_EQ(encode_devices(got, &list, (const uint8_t *const[]){keyboard, NULL}, is_secure),
		 1 + 2 * DIAG_DEVICE_LEN);
	CHECK_EQ(got[1 + BTPOLICY_ADDR_LEN + 2], DEVICE_NO_SLOT);
	CHECK_EQ(got[1 + DIAG_DEVICE_LEN + BTPOLICY_ADDR_LEN + 2], 0);
	CHECK_EQ(got[1 + BTPOLICY_ADDR_LEN + 1], DIAG_PAIRING_KNOWN | DIAG_PAIRING_SECURE);
	CHECK_EQ(got[1 + DIAG_DEVICE_LEN + BTPOLICY_ADDR_LEN + 1],
		 DIAG_PAIRING_KNOWN | DIAG_PAIRING_PASSKEY);
}

static void test_a_command_or_protocol_it_does_not_know_is_answered(void)
{
	CHECK(diag_command_answered(0, PROTOCOL_NUPHY));
	CHECK(diag_command_answered(200, PROTOCOL_BLUETOOTH));
	CHECK(diag_command_answered(DIAG_COMMAND_FORGET, 0));
	CHECK(!diag_command_answered(DIAG_COMMAND_FORGET, PROTOCOL_NUPHY));
}

int main(void)
{
	RUN(test_status_matches_fixture);
	RUN(test_a_mouse_slot_status_matches_fixture);
	RUN(test_watch_matches_fixture);
	RUN(test_a_paused_watch_past_its_hold_reports_none_left);
	RUN(test_a_watch_without_a_threshold_counts_nothing);
	RUN(test_a_full_watch_fits_the_payload_buffer);
	RUN(test_devices_match_fixture);
	RUN(test_a_device_ready_in_no_slot_is_sent_with_none);
	RUN(test_a_command_or_protocol_it_does_not_know_is_answered);
	return TEST_EXIT();
}
