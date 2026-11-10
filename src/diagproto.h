#ifndef DIAGPROTO_H
#define DIAGPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "history.h"

#define DIAG_VERSION 1
#define DIAG_REPORT_LEN 64
#define DIAG_HEADER_LEN 4
#define DIAG_CHUNK (DIAG_REPORT_LEN - DIAG_HEADER_LEN)
#define DIAG_REQUEST_LEN 6
#define DIAG_ERROR_FLAG 0x80
#define DIAG_PAYLOAD_MAX 640
#define DIAG_MINUTES_PER_PAGE 60
#define DIAG_EVENTS_PER_PAGE 32

enum diag_command {
	DIAG_STATUS = 1,
	DIAG_HISTORY,
	DIAG_EVENTS,
	DIAG_NOISE,
	DIAG_REBOOT,
	DIAG_BOOTLOADER,
	DIAG_FORGET,
	DIAG_CLEAR,
};

enum diag_error {
	DIAG_ERR_UNKNOWN_COMMAND = 1,
	DIAG_ERR_RADIO,
	DIAG_ERR_STORE,
};

struct diag_request {
	uint8_t command;
	uint8_t seq;
	uint32_t arg;
};

struct diag_status {
	uint64_t uptime_ms;
	uint32_t reset_cause;
	uint8_t link_state;
	uint8_t channel;
	uint8_t own[3];
	uint8_t keyboard_id[4];
	int8_t last_rssi;
	uint32_t last_rx_age_ms;
	bool usb_live;
	bool usb_suspended;
	uint32_t packets;
	uint32_t invalid;
	uint32_t dropped;
	uint32_t channel_changes;
	uint32_t link_losses;
	uint32_t usb_retries;
	uint32_t merges;
	uint32_t surveys;
	uint32_t minute_next;
	uint32_t minute_oldest;
	uint32_t events_total;
	uint32_t events_oldest;
};

/* Returns false for a report shorter than DIAG_REQUEST_LEN. */
bool diag_parse_request(const uint8_t *buf, size_t len, struct diag_request *out);
size_t diag_part_count(size_t payload_len);
void diag_part(uint8_t out[DIAG_REPORT_LEN], const struct diag_request *req,
	       const uint8_t *payload, size_t len, size_t part);
void diag_error_report(uint8_t out[DIAG_REPORT_LEN], const struct diag_request *req,
		       enum diag_error code);
size_t diag_encode_status(uint8_t *out, const struct diag_status *s);
size_t diag_encode_minutes(uint8_t *out, uint32_t first, const struct history_minute *m,
			  size_t n);
size_t diag_encode_events(uint8_t *out, uint32_t first, const struct history_event *e,
			 size_t n);

#endif
