#ifndef DIAGPROTO_H
#define DIAGPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btlist.h"
#include "chanpick.h"
#include "chanwatch.h"
#include "history.h"
#include "protocol.h"

/* Bumped only when a layout that a release has shipped, or what a value in it means, changes. */
#define DIAG_VERSION 4
#define DIAG_REPORT_LEN 64
#define DIAG_HEADER_LEN 4
#define DIAG_CHUNK (DIAG_REPORT_LEN - DIAG_HEADER_LEN)
#define DIAG_REQUEST_LEN 6
#define DIAG_REQUEST_DATA_LEN 7
#define DIAG_ERROR_FLAG 0x80
#define DIAG_PAYLOAD_MAX 720
#define DIAG_MINUTES_PER_PAGE 40
#define DIAG_EVENTS_PER_PAGE 32
#define DIAG_NAME_MAX 24
/* Marks last_rssi as absent: no receiver reports a signal this low. */
#define DIAG_NO_RSSI INT8_MIN
#define DIAG_SLOT_STORED 0x01
#define DIAG_SLOT_READY 0x02
/* The other pairing bits are unset without it. */
#define DIAG_PAIRING_KNOWN 0x01
/* LE Secure Connections, not legacy pairing. */
#define DIAG_PAIRING_SECURE 0x02
#define DIAG_PAIRING_PASSKEY 0x04
/* In a DIAG_PAIR request's arg under Bluetooth: pair without asking the keyboard for a passkey. */
#define DIAG_PAIR_NO_PASSKEY 1
/* The reply under Bluetooth: 1 if a passkey is to be typed, then the passkey. */
#define DIAG_PAIR_REPLY_LEN 5
#define DIAG_DEVICE_LEN (BTPOLICY_ADDR_LEN + 3 + BTLIST_NAME_LEN)
#define DIAG_DEVICES_MAX (1 + BTPOLICY_STORED_MAX * DIAG_DEVICE_LEN)

enum diag_command {
	/* This and DIAG_HISTORY take the slot asked about as data's first byte. */
	DIAG_STATUS = 1,
	DIAG_HISTORY,
	DIAG_EVENTS,
	DIAG_NOISE,
	DIAG_REBOOT,
	DIAG_BOOTLOADER,
	DIAG_CLEAR,
	DIAG_FORCE_CHANNELS,
	DIAG_PAIR,
	/* arg 0 reads; a protocol's value sets. The reply is the running protocol, then 1 if the dongle
	 * reboots to apply a changed protocol. */
	DIAG_PROTOCOL,
	/* arg is the channel; the reply is the channel the link was on. */
	DIAG_MOVE,
	DIAG_WATCH,
	/* The stored devices; under Bluetooth the most recently connected first. */
	DIAG_DEVICES,
	/* data is the device's address as DIAG_DEVICES gives it. */
	DIAG_FORGET,
};

enum diag_error {
	DIAG_ERR_UNKNOWN_COMMAND = 1,
	DIAG_ERR_RADIO,
	DIAG_ERR_BAD_CHANNELS,
	DIAG_ERR_STORE,
	DIAG_ERR_WRONG_PROTOCOL,
	DIAG_ERR_NOT_MOVABLE,
	DIAG_ERR_KEY_HELD,
	DIAG_ERR_LIST_FULL,
	DIAG_ERR_NO_SUCH_DEVICE,
};

struct diag_request {
	uint8_t command;
	uint8_t seq;
	uint32_t arg;
	/* The bytes after arg; zeros where the report ends before them. */
	uint8_t data[DIAG_REQUEST_DATA_LEN];
};

struct diag_slot {
	/* An enum device_kind. */
	uint8_t kind;
	bool stored;
	bool ready;
};

struct diag_status {
	uint8_t firmware_version[3];
	uint64_t uptime_ms;
	uint32_t reset_cause;
	uint8_t link_state;
	uint8_t channel;
	uint8_t own[3];
	/* All zero while pairing surveys. */
	uint8_t forced[3];
	uint8_t keyboard_id[4];
	bool usb_live;
	bool usb_suspended;
	uint32_t invalid;
	uint32_t dropped;
	uint32_t channel_changes;
	uint32_t usb_retries;
	uint32_t merges;
	uint32_t surveys;
	uint32_t minute_next;
	uint32_t minute_oldest;
	uint32_t events_total;
	uint32_t events_oldest;
	uint8_t protocol;
	bool pairing_open;
	uint8_t keyboard_serial[4];
	uint8_t keyboard_type;
	uint8_t slot_count;
	struct diag_slot slots[DEVICE_SLOTS];
	/* The rest is the asked slot's. */
	uint8_t slot;
	int8_t last_rssi;
	uint32_t last_rx_age_ms;
	uint32_t packets;
	uint32_t gaps;
	uint32_t link_losses;
	/* UINT32_MAX before a report from the slot reached the host since the last clear; the
	 * maximum is 0 then. */
	uint32_t latency_average_us;
	uint32_t latency_max_us;
	uint8_t pairing;
	/* Padded with zeros. */
	uint8_t name[DIAG_NAME_MAX];
};

/* Returns false for a report shorter than DIAG_REQUEST_LEN. */
bool diag_parse_request(const uint8_t *buf, size_t len, struct diag_request *out);
/* Returns false for an arg of zero, which clears the forced channels. */
bool diag_channels_arg(uint32_t arg, uint8_t out[3]);
size_t diag_part_count(size_t payload_len);
void diag_part(uint8_t out[DIAG_REPORT_LEN], const struct diag_request *req,
	       const uint8_t *payload, size_t len, size_t part);
/* detail is the active protocol for DIAG_ERR_WRONG_PROTOCOL and 0 otherwise. */
void diag_error_report(uint8_t out[DIAG_REPORT_LEN], const struct diag_request *req,
		       enum diag_error code, uint8_t detail);
bool diag_command_answered(uint8_t command, enum protocol active);
size_t diag_encode_status(uint8_t *out, const struct diag_status *s);
/* Each minute carries the link-wide counters and the slot's own. */
size_t diag_encode_minutes(uint8_t *out, uint32_t first, const struct history_minute *m,
			  size_t n, uint8_t slot);
size_t diag_encode_events(uint8_t *out, uint32_t first, const struct history_event *e,
			 size_t n);
size_t diag_encode_noise(uint8_t *out, const struct chan_survey *s);
uint8_t diag_pairing(const struct btlist_entry *e, bool secure);
/* connected[s] is the address of the device ready in slot s, or NULL; secure tells whether the
 * device at an address paired with LE Secure Connections, and NULL leaves every device's pairing
 * unknown. */
size_t diag_encode_devices(uint8_t *out, const struct btlist *l,
			   const uint8_t *const connected[DEVICE_SLOTS],
			   bool (*secure)(const uint8_t addr[BTPOLICY_ADDR_LEN]));
/* No home is sent as 0, no threshold as CHANWATCH_NO_THRESHOLD. */
size_t diag_encode_watch(uint8_t *out, const struct chanwatch *w, uint8_t home,
			 uint8_t threshold_bucket, bool paused, uint64_t now_ms);

#endif
