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

/* Bumped only when a layout that a release has shipped changes. */
#define DIAG_VERSION 2
#define DIAG_REPORT_LEN 64
#define DIAG_HEADER_LEN 4
#define DIAG_CHUNK (DIAG_REPORT_LEN - DIAG_HEADER_LEN)
#define DIAG_REQUEST_LEN 6
#define DIAG_REQUEST_DATA_LEN 7
#define DIAG_ERROR_FLAG 0x80
#define DIAG_PAYLOAD_MAX 704
#define DIAG_MINUTES_PER_PAGE 40
#define DIAG_EVENTS_PER_PAGE 32
#define DIAG_NAME_MAX 24
#define DIAG_KEYBOARD_CONNECTED 0x01
#define DIAG_KEYBOARD_NO_PASSKEY 0x02
/* In a DIAG_PAIR request's arg under Bluetooth: pair without asking the keyboard for a passkey. */
#define DIAG_PAIR_NO_PASSKEY 1
/* The reply under Bluetooth: 1 if a passkey is to be typed, then the passkey. */
#define DIAG_PAIR_REPLY_LEN 5
#define DIAG_KEYBOARD_LEN (BTPOLICY_ADDR_LEN + 1 + BTLIST_NAME_LEN)
#define DIAG_KEYBOARDS_MAX (1 + BTPOLICY_STORED_MAX * DIAG_KEYBOARD_LEN)

enum diag_command {
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
	/* The stored Bluetooth keyboards, the most recently connected first. */
	DIAG_KEYBOARDS,
	/* data is the keyboard's address as DIAG_KEYBOARDS gives it. */
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
	DIAG_ERR_NO_SUCH_KEYBOARD,
};

struct diag_request {
	uint8_t command;
	uint8_t seq;
	uint32_t arg;
	/* The bytes after arg; zeros where the report ends before them. */
	uint8_t data[DIAG_REQUEST_DATA_LEN];
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
	int8_t last_rssi;
	uint32_t last_rx_age_ms;
	bool usb_live;
	bool usb_suspended;
	struct history_totals totals;
	uint32_t merges;
	uint32_t surveys;
	uint32_t minute_next;
	uint32_t minute_oldest;
	uint32_t events_total;
	uint32_t events_oldest;
	uint8_t protocol;
	bool pairing_open;
	/* The name is padded with zeros. */
	uint8_t keyboard_serial[4];
	uint8_t keyboard_type;
	uint8_t keyboard_name[DIAG_NAME_MAX];
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
size_t diag_encode_minutes(uint8_t *out, uint32_t first, const struct history_minute *m,
			  size_t n);
size_t diag_encode_events(uint8_t *out, uint32_t first, const struct history_event *e,
			 size_t n);
size_t diag_encode_noise(uint8_t *out, const struct chan_survey *s);
/* connected is the connected keyboard's address, or NULL. */
size_t diag_encode_keyboards(uint8_t *out, const struct btlist *l, const uint8_t *connected);
/* No home is sent as 0, no threshold as CHANWATCH_NO_THRESHOLD. */
size_t diag_encode_watch(uint8_t *out, const struct chanwatch *w, uint8_t home,
			 uint8_t threshold_bucket, bool paused, uint64_t now_ms);

#endif
