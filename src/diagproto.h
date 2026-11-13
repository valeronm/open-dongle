#ifndef DIAGPROTO_H
#define DIAGPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chanpick.h"
#include "history.h"
#include "protocol.h"

/* Bumped only when a layout that a release has shipped changes. */
#define DIAG_VERSION 2
#define DIAG_REPORT_LEN 64
#define DIAG_HEADER_LEN 4
#define DIAG_CHUNK (DIAG_REPORT_LEN - DIAG_HEADER_LEN)
#define DIAG_REQUEST_LEN 6
#define DIAG_ERROR_FLAG 0x80
#define DIAG_PAYLOAD_MAX 704
#define DIAG_MINUTES_PER_PAGE 40
#define DIAG_EVENTS_PER_PAGE 32

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
	/* arg 0 reads; 1 or 2 sets. The reply is the running protocol, then 1 if the dongle
	 * reboots to apply a changed protocol. */
	DIAG_PROTOCOL,
};

enum diag_error {
	DIAG_ERR_UNKNOWN_COMMAND = 1,
	DIAG_ERR_RADIO,
	DIAG_ERR_BAD_CHANNELS,
	DIAG_ERR_STORE,
	DIAG_ERR_WRONG_PROTOCOL,
};

struct diag_request {
	uint8_t command;
	uint8_t seq;
	uint32_t arg;
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
/* Returns 0 for a command every protocol answers. */
uint8_t diag_command_protocol(uint8_t command);
size_t diag_encode_status(uint8_t *out, const struct diag_status *s);
size_t diag_encode_minutes(uint8_t *out, uint32_t first, const struct history_minute *m,
			  size_t n);
size_t diag_encode_events(uint8_t *out, uint32_t first, const struct history_event *e,
			 size_t n);
size_t diag_encode_noise(uint8_t *out, const struct chan_survey *s);

#endif
