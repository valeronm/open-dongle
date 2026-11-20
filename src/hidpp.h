#ifndef HIDPP_H
#define HIDPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device.h"
#include "history.h"

#define HIDPP_TRIES 3
#define HIDPP_PARAMS_MAX 16
#define HIDPP_SHORT_REPORT_ID 0x10
#define HIDPP_LONG_REPORT_ID 0x11
/* The bytes after the report ID: device, feature and function, then the parameters. */
#define HIDPP_HEADER_LEN 3
#define HIDPP_LONG_LEN (HIDPP_HEADER_LEN + HIDPP_PARAMS_MAX)
#define HIDPP_BRIGHTNESS_DOWN 0x70
#define HIDPP_BRIGHTNESS_UP 0x6f

/* A HID++ 2.0 message without its report ID; a short one carries the first parameters only. */
struct hidpp_msg {
	uint8_t device;
	uint8_t feature;
	/* The function in the high nibble, the software ID in the low one. */
	uint8_t function;
	uint8_t params[HIDPP_PARAMS_MAX];
	bool is_long;
};

enum hidpp_step {
	HIDPP_IDLE,
	HIDPP_FIND_CONTROLS,
	HIDPP_DIVERT,
	HIDPP_FIND_BATTERY,
	HIDPP_FIND_UNIFIED_BATTERY,
	HIDPP_READ_BATTERY,
	HIDPP_FIND_WHEEL,
	HIDPP_READ_WHEEL,
	HIDPP_SET_WHEEL,
	HIDPP_STEPS,
};

enum hidpp_wheel_mode {
	HIDPP_WHEEL_UNKNOWN,
	HIDPP_WHEEL_NOTCHES,
	HIDPP_WHEEL_HIRES,
};

/* A run settles one feature: a keyboard's divert or a mouse's wheel. */
struct hidpp_event {
	bool pending;
	enum history_event_type type;
	uint8_t a, b, c;
	uint32_t value;
};

/*
 * The HID++ run at each connect. A keyboard's controls in hidpp.c's table are diverted over
 * feature 0x1B04 for the device's session only, since a persistent divert would also silence them
 * on the device's other hosts. The battery is read once, because a device can send its connect
 * battery event before the run can know the feature's index. A mouse's wheel is set to the
 * resolution the host wants.
 */
struct hidpp {
	enum device_kind kind;
	enum hidpp_step step;
	/* The index of feature 0x1B04; 0 until the device names it. */
	uint8_t controls;
	uint8_t control_index;
	/* The index of feature 0x1000, or of 0x1004 on a device without it; 0 until the device names
	 * one. */
	uint8_t battery;
	/* A feature index is fixed by the device's firmware, so it is kept until another device
	 * pairs. */
	bool controls_known;
	bool battery_known;
	/* The device answered that it has no feature 0x1000. */
	bool no_battery_status;
	/* The index of feature 0x2121; 0 until the mouse names it. */
	uint8_t wheel;
	bool wheel_known;
	/* High-resolution steps per notch; 0 until the mouse reports it. */
	uint8_t wheel_multiplier;
	bool hires_wanted;
	/* The mode the last wheel request asked for. */
	bool hires_sent;
	/* The mode the mouse last confirmed; a mouse can come back from a disconnect in either. */
	enum hidpp_wheel_mode wheel_mode;
	/* Requests the device took without answering. */
	uint8_t tries;
	bool awaiting;
	uint64_t taken_ms;
	/* The divert's controls so far this run. */
	uint8_t diverted;
	uint8_t refused;
	struct hidpp_event event;
};

/* Reads a message from the len bytes after its report ID. */
void hidpp_msg_read(const uint8_t *bytes, size_t len, bool is_long, struct hidpp_msg *m);
/* Writes the message into the len bytes after its report ID, which the caller zeroed. */
void hidpp_msg_write(const struct hidpp_msg *m, uint8_t *bytes, size_t len);

/* Clears the run for a newly paired device: a keyboard has its controls diverted, a mouse its
 * wheel set to the resolution the host wants. */
void hidpp_init(struct hidpp *x, enum device_kind kind);
/* Starts the run at a connect; a run still on its first request is kept, since one connect can
 * call this several times. */
void hidpp_start(struct hidpp *x);
/* Writes the request due now and returns true; a request unanswered within retry_ms is sent again,
 * up to HIDPP_TRIES times. */
bool hidpp_request(struct hidpp *x, uint8_t device, uint64_t now_ms, uint32_t retry_ms,
		   struct hidpp_msg *out);
/* Records that the device took the request hidpp_request wrote. */
void hidpp_taken(struct hidpp *x, uint64_t now_ms);
/* Takes a message from the device, updating the battery it reports. Returns true for a
 * diverted-controls event, with the usage of the first held control in the table, or 0. */
bool hidpp_rx(struct hidpp *x, struct device_battery *b, const struct hidpp_msg *m,
	      uint16_t *usage);
/* Sets the wheel resolution the host wants; the run sets the mouse's wheel to it. */
void hidpp_want_hires(struct hidpp *x, bool on);
/* The mouse's wheel steps per notch: its multiplier once it confirms high resolution, else 1. */
uint8_t hidpp_wheel_steps(const struct hidpp *x);
/* Copies out the event the run settled, once. */
bool hidpp_take_event(struct hidpp *x, struct hidpp_event *out);

#endif
