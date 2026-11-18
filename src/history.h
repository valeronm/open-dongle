#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device.h"
#include "noise.h"

#define HISTORY_MINUTES 1440
#define HISTORY_EVENTS 512
#define HISTORY_MINUTE_MS 60000

/* Totals must not decrease except across history_clear; minutes store their differences. */
struct history_totals {
	uint32_t packets[DEVICE_KINDS];
	uint32_t invalid;
	uint32_t dropped;
	uint32_t channel_changes;
	uint32_t link_losses;
	uint32_t usb_retries;
	uint32_t gaps;
};

/* All 0 for a minute without a history_rssi reading from the device. */
struct history_rssi {
	int8_t min, avg, max;
};

/* Counts saturate. */
struct history_minute {
	uint16_t packets[DEVICE_KINDS];
	uint8_t invalid, dropped, channel_changes, link_losses, usb_retries, gaps;
	struct history_rssi rssi[DEVICE_KINDS];
	struct noise_record noise;
};

struct history_rssi_sum {
	int32_t sum;
	uint32_t count;
	int8_t min;
	int8_t max;
};

enum history_event_type {
	HISTORY_BOOT = 1, HISTORY_PAIRED, HISTORY_LINK_LOST, HISTORY_SURVEY,
	HISTORY_NOISE, HISTORY_USB_SUSPEND, HISTORY_USB_RESUME,
	HISTORY_CLEAR, HISTORY_LINK_UP, HISTORY_LINK_MOVED, HISTORY_LINK_GAP,
	HISTORY_UNIFYING_PAIRED, HISTORY_PAIRING_OPENED, HISTORY_PAIRING_CLOSED,
	HISTORY_PAIRING_TIMEOUT, HISTORY_RECONNECT, HISTORY_ATTACK,
	HISTORY_BT_CONNECTED, HISTORY_BT_DISCONNECTED, HISTORY_BT_PAIRED,
	HISTORY_BT_REFUSED, HISTORY_BT_BOND_LOST
};

#define HISTORY_ADDR_LEN 7

/* Byte a of HISTORY_BT_REFUSED. */
enum history_bt_refused {
	HISTORY_REFUSED_BY_KEYBOARD,
	HISTORY_REFUSED_NO_PASSKEY,
};

/* Byte c of HISTORY_LINK_UP. */
enum history_link_up {
	HISTORY_UP_FIRST,
	HISTORY_UP_AGAIN,
	HISTORY_UP_WOKE,
};

/* Byte c of HISTORY_LINK_MOVED; a noise move's value holds the home share in its high 16 bits and
 * the new channel's in its low 16, in hundredths of a percent. */
enum history_link_moved {
	HISTORY_MOVED_TO_HOME,
	HISTORY_MOVED_FOR_NOISE,
	HISTORY_MOVED_ON_REQUEST,
};

struct history_event {
	uint64_t time_ms;
	uint8_t type, a, b, c;
	uint32_t value;
	struct noise_record noise;
};

/* Minute n covers uptime [n * HISTORY_MINUTE_MS, (n + 1) * HISTORY_MINUTE_MS). */
struct history {
	struct history_minute minutes[HISTORY_MINUTES];
	uint32_t minute_next;
	uint32_t minute_oldest;
	struct history_totals at_minute_start;
	struct history_rssi_sum rssi[DEVICE_KINDS];
	struct history_event events[HISTORY_EVENTS];
	uint32_t events_total;
	uint32_t events_oldest;
	struct noise_tally noise;
	struct noise_tally since_packet;
};

void history_init(struct history *h, uint64_t now_ms, const struct history_totals *t);

/* Drops the held minutes and events; their indexes are not reset. */
void history_clear(struct history *h, uint64_t now_ms, const struct history_totals *t);

/* Closes every minute that ended before now_ms. */
void history_tick(struct history *h, uint64_t now_ms, const struct history_totals *t);

/* A gap's event is logged before the keyboard packet that ended the gap comes here. */
void history_rssi(struct history *h, enum device_kind device, int8_t dbm);

/* The device's weakest reading over the current and the previous minute; false when neither had
 * one. */
bool history_weakest(const struct history *h, enum device_kind device, int8_t *out);

/* A gap or loss event records the readings since the last history_rssi in place of e's noise. */
void history_event(struct history *h, const struct history_event *e);

/* Readings off the home channel count toward a gap or loss but not toward the minute. */
void history_noise(struct history *h, const struct noise_tally *t, bool home);

/* addr is the address type, then the six address bytes, least significant first; the event leaves
 * out the type and holds the bytes in a, b and value, most significant first. c is 1 for a
 * HISTORY_BT_PAIRED with a passkey. */
struct history_event history_keyboard_event(uint8_t type, const uint8_t addr[HISTORY_ADDR_LEN],
					    uint8_t c);

/* Copies up to max held records from index from, or from the oldest held if that is later. */
size_t history_minutes(const struct history *h, uint32_t from, uint32_t *first,
		       struct history_minute *out, size_t max);
size_t history_events(const struct history *h, uint32_t from, uint32_t *first,
		      struct history_event *out, size_t max);

#endif
