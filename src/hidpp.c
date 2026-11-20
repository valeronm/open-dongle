#include <string.h>
#include "hidpp.h"

#define HIDPP_ROOT 0x00
#define HIDPP_ERROR 0xff
#define HIDPP_REPROG_CONTROLS 0x1b04
#define HIDPP_BATTERY_STATUS 0x1000
#define HIDPP_UNIFIED_BATTERY 0x1004
#define HIDPP_HIRES_WHEEL 0x2121
/* A request carries software ID 1 and an event ID 0. */
#define HIDPP_GET_FEATURE 0x01
#define HIDPP_SET_CID_REPORTING 0x31
#define HIDPP_GET_BATTERY 0x01
#define HIDPP_GET_UNIFIED_BATTERY 0x11
#define HIDPP_GET_WHEEL_CAPABILITY 0x01
#define HIDPP_SET_WHEEL_MODE 0x21
/* Bit 0 of the wheel mode would send the wheel over HID++ instead of the HID report. */
#define HIDPP_WHEEL_MODE_HIRES_BIT 0x02
#define HIDPP_BATTERY_EVENT 0x00
#define HIDPP_DIVERTED_EVENT 0x00
/* The divert bit and the bit that says it is valid; the persist bits stay clear. */
#define HIDPP_DIVERT_FLAGS 0x03

/* Logitech's control IDs mean the same key on every device; one a device lacks is refused. */
static const struct {
	uint16_t cid;
	uint16_t usage;
} diverted[] = {
	{0x00c7, HIDPP_BRIGHTNESS_DOWN},
	{0x00c8, HIDPP_BRIGHTNESS_UP},
};

#define DIVERTED_COUNT (sizeof(diverted) / sizeof(diverted[0]))

static size_t params_len(size_t len)
{
	size_t n = len > HIDPP_HEADER_LEN ? len - HIDPP_HEADER_LEN : 0;

	return n < HIDPP_PARAMS_MAX ? n : HIDPP_PARAMS_MAX;
}

void hidpp_msg_read(const uint8_t *bytes, size_t len, bool is_long, struct hidpp_msg *m)
{
	*m = (struct hidpp_msg){.is_long = is_long};
	if (len < HIDPP_HEADER_LEN) {
		return;
	}
	m->device = bytes[0];
	m->feature = bytes[1];
	m->function = bytes[2];
	memcpy(m->params, &bytes[HIDPP_HEADER_LEN], params_len(len));
}

void hidpp_msg_write(const struct hidpp_msg *m, uint8_t *bytes, size_t len)
{
	if (len < HIDPP_HEADER_LEN) {
		return;
	}
	bytes[0] = m->device;
	bytes[1] = m->feature;
	bytes[2] = m->function;
	memcpy(&bytes[HIDPP_HEADER_LEN], m->params, params_len(len));
}

static uint16_t diverted_usage(uint16_t cid)
{
	for (size_t i = 0; i < DIVERTED_COUNT; i++) {
		if (cid != 0 && diverted[i].cid == cid) {
			return diverted[i].usage;
		}
	}
	return 0;
}

static void hidpp_step(struct hidpp *x, enum hidpp_step step)
{
	x->step = step;
	x->tries = 0;
	x->awaiting = false;
}

/* A run takes the steps in their enum order and skips each one this device does not need. */
static bool wanted(const struct hidpp *x, enum hidpp_step step)
{
	switch (step) {
	case HIDPP_FIND_CONTROLS:
		return x->kind == DEVICE_KIND_KEYBOARD && !x->controls_known;
	case HIDPP_DIVERT:
		return x->controls != 0;
	case HIDPP_FIND_BATTERY:
		return !x->battery_known && !x->no_battery_status;
	case HIDPP_FIND_UNIFIED_BATTERY:
		return !x->battery_known && x->no_battery_status;
	case HIDPP_READ_BATTERY:
		return x->battery != 0;
	case HIDPP_FIND_WHEEL:
		return x->kind == DEVICE_KIND_MOUSE && !x->wheel_known;
	case HIDPP_READ_WHEEL:
		return x->wheel != 0 && x->wheel_multiplier == 0;
	case HIDPP_SET_WHEEL:
		return x->wheel != 0 && x->wheel_multiplier != 0 &&
		       x->wheel_mode != (x->hires_wanted ? HIDPP_WHEEL_HIRES : HIDPP_WHEEL_NOTCHES);
	default:
		return false;
	}
}

static void hidpp_advance(struct hidpp *x)
{
	enum hidpp_step next = x->step + 1;

	while (next < HIDPP_STEPS && !wanted(x, next)) {
		next++;
	}
	x->control_index = 0;
	hidpp_step(x, next < HIDPP_STEPS ? next : HIDPP_IDLE);
}

static void divert_end(struct hidpp *x, enum history_divert outcome, uint16_t cid)
{
	x->event = (struct hidpp_event){
		.pending = true,
		.detail = {.tag = DIAG_DETAIL_KEYS_DIVERTED,
			   .KEYS_DIVERTED = {.outcome = outcome,
					     .diverted = x->diverted,
					     .refused = x->refused,
					     .control = cid}}};
}

static void wheel_end(struct hidpp *x, enum history_wheel outcome)
{
	x->event = (struct hidpp_event){
		.pending = true,
		.detail = {.tag = DIAG_DETAIL_WHEEL_SET,
			   .WHEEL_SET = {.outcome = outcome,
					 .steps = outcome == HISTORY_WHEEL_HIRES ? x->wheel_multiplier
										 : 0}}};
}

void hidpp_init(struct hidpp *x, enum device_kind kind)
{
	*x = (struct hidpp){.kind = kind};
}

void hidpp_start(struct hidpp *x)
{
	if (x->step == HIDPP_FIND_CONTROLS || (x->step == HIDPP_DIVERT && x->control_index == 0)) {
		return;
	}
	x->event = (struct hidpp_event){0};
	x->diverted = 0;
	x->refused = 0;
	x->wheel_mode = HIDPP_WHEEL_UNKNOWN;
	x->step = HIDPP_IDLE;
	if (x->controls_known && x->controls == 0) {
		divert_end(x, HISTORY_DIVERT_NO_FEATURE, 0);
	}
	if (x->wheel_known && x->wheel == 0) {
		wheel_end(x, HISTORY_WHEEL_NO_FEATURE);
	}
	hidpp_advance(x);
}

static void hidpp_next(struct hidpp *x)
{
	if (x->step == HIDPP_DIVERT && (size_t)x->control_index + 1 < DIVERTED_COUNT) {
		x->control_index++;
		hidpp_step(x, HIDPP_DIVERT);
		return;
	}
	if (x->step == HIDPP_DIVERT) {
		divert_end(x, HISTORY_DIVERT_FINISHED, 0);
	} else if (x->step == HIDPP_FIND_CONTROLS && x->controls == 0) {
		divert_end(x, HISTORY_DIVERT_NO_FEATURE, 0);
	}
	hidpp_advance(x);
}

static void hidpp_give_up(struct hidpp *x)
{
	if (x->step == HIDPP_FIND_CONTROLS) {
		divert_end(x, HISTORY_DIVERT_UNANSWERED, 0);
	} else if (x->step == HIDPP_DIVERT) {
		divert_end(x, HISTORY_DIVERT_UNANSWERED, diverted[x->control_index].cid);
	} else if (x->step == HIDPP_FIND_WHEEL || x->step == HIDPP_READ_WHEEL ||
		   x->step == HIDPP_SET_WHEEL) {
		wheel_end(x, HISTORY_WHEEL_UNANSWERED);
	}
	hidpp_advance(x);
}

static void get_feature(struct hidpp_msg *out, uint16_t feature)
{
	out->feature = HIDPP_ROOT;
	out->function = HIDPP_GET_FEATURE;
	out->params[0] = (uint8_t)(feature >> 8);
	out->params[1] = (uint8_t)feature;
}

static void set_cid_reporting(struct hidpp_msg *out, uint8_t feature, uint16_t cid)
{
	out->feature = feature;
	out->function = HIDPP_SET_CID_REPORTING;
	out->params[0] = (uint8_t)(cid >> 8);
	out->params[1] = (uint8_t)cid;
	out->params[2] = HIDPP_DIVERT_FLAGS;
	out->is_long = true;
}

static uint8_t battery_function(const struct hidpp *x)
{
	return x->no_battery_status ? HIDPP_GET_UNIFIED_BATTERY : HIDPP_GET_BATTERY;
}

bool hidpp_request(struct hidpp *x, uint8_t device, uint64_t now_ms, uint32_t retry_ms,
		   struct hidpp_msg *out)
{
	if (x->step == HIDPP_IDLE || (x->awaiting && now_ms - x->taken_ms < retry_ms)) {
		return false;
	}
	if (x->tries >= HIDPP_TRIES) {
		hidpp_give_up(x);
		return false;
	}
	*out = (struct hidpp_msg){.device = device};
	switch (x->step) {
	case HIDPP_IDLE:
	case HIDPP_STEPS:
		break;
	case HIDPP_FIND_CONTROLS:
		get_feature(out, HIDPP_REPROG_CONTROLS);
		return true;
	case HIDPP_DIVERT:
		set_cid_reporting(out, x->controls, diverted[x->control_index].cid);
		return true;
	case HIDPP_FIND_BATTERY:
		get_feature(out, HIDPP_BATTERY_STATUS);
		return true;
	case HIDPP_FIND_UNIFIED_BATTERY:
		get_feature(out, HIDPP_UNIFIED_BATTERY);
		return true;
	case HIDPP_READ_BATTERY:
		out->feature = x->battery;
		out->function = battery_function(x);
		return true;
	case HIDPP_FIND_WHEEL:
		get_feature(out, HIDPP_HIRES_WHEEL);
		return true;
	case HIDPP_READ_WHEEL:
		out->feature = x->wheel;
		out->function = HIDPP_GET_WHEEL_CAPABILITY;
		return true;
	case HIDPP_SET_WHEEL:
		x->hires_sent = x->hires_wanted;
		out->feature = x->wheel;
		out->function = HIDPP_SET_WHEEL_MODE;
		out->params[0] = x->hires_sent ? HIDPP_WHEEL_MODE_HIRES_BIT : 0;
		return true;
	}
	return false;
}

/* A level of 0 is unknown, in either battery feature. */
static void read_battery(struct device_battery *b, const struct hidpp_msg *m)
{
	uint8_t status = m->params[2];

	b->level = m->params[0] != 0 ? m->params[0] : DEVICE_NO_BATTERY;
	if (status == 0) {
		b->charging = DEVICE_CHARGING_DISCHARGING;
	} else if (status == 3) {
		b->charging = DEVICE_CHARGING_CHARGED;
	} else if (status == 1 || status == 2 || status == 4) {
		b->charging = DEVICE_CHARGING_CHARGING;
	} else if (status <= 7) {
		b->charging = DEVICE_CHARGING_BATTERY_FAULT;
	} else {
		b->charging = DEVICE_CHARGING_UNKNOWN;
	}
}

void hidpp_taken(struct hidpp *x, uint64_t now_ms)
{
	if (x->step != HIDPP_IDLE) {
		x->tries++;
		x->awaiting = true;
		x->taken_ms = now_ms;
	}
}

static bool hidpp_reply(struct hidpp *x, struct device_battery *b, const struct hidpp_msg *m)
{
	bool error = m->feature == HIDPP_ERROR;
	uint8_t feature = error ? m->function : m->feature;
	uint8_t function = error ? m->params[0] : m->function;

	bool find = feature == HIDPP_ROOT && function == HIDPP_GET_FEATURE;

	if (x->step == HIDPP_FIND_CONTROLS && find) {
		x->controls = error ? 0 : m->params[0];
		x->controls_known = true;
	} else if (x->step == HIDPP_FIND_BATTERY && find) {
		x->battery = error ? 0 : m->params[0];
		x->no_battery_status = x->battery == 0;
		x->battery_known = !x->no_battery_status;
	} else if (x->step == HIDPP_FIND_UNIFIED_BATTERY && find) {
		x->battery = error ? 0 : m->params[0];
		x->battery_known = true;
	} else if (x->step == HIDPP_READ_BATTERY && feature == x->battery &&
		   function == battery_function(x)) {
		if (!error) {
			read_battery(b, m);
		}
	} else if (x->step == HIDPP_FIND_WHEEL && find) {
		x->wheel = error ? 0 : m->params[0];
		x->wheel_known = true;
		if (x->wheel == 0) {
			wheel_end(x, HISTORY_WHEEL_NO_FEATURE);
		}
	} else if (x->step == HIDPP_READ_WHEEL && feature == x->wheel &&
		   function == HIDPP_GET_WHEEL_CAPABILITY) {
		if (error || m->params[0] == 0) {
			x->wheel = 0;
			wheel_end(x, HISTORY_WHEEL_REFUSED);
		} else {
			x->wheel_multiplier = m->params[0];
		}
	} else if (x->step == HIDPP_SET_WHEEL && feature == x->wheel &&
		   function == HIDPP_SET_WHEEL_MODE) {
		if (error) {
			x->wheel_mode = HIDPP_WHEEL_UNKNOWN;
			wheel_end(x, HISTORY_WHEEL_REFUSED);
		} else {
			x->wheel_mode = x->hires_sent ? HIDPP_WHEEL_HIRES : HIDPP_WHEEL_NOTCHES;
			wheel_end(x, x->hires_sent ? HISTORY_WHEEL_HIRES : HISTORY_WHEEL_NOTCHES);
		}
	} else if (x->step == HIDPP_DIVERT && feature == x->controls &&
		   function == HIDPP_SET_CID_REPORTING &&
		   (error || (m->params[0] << 8 | m->params[1]) == diverted[x->control_index].cid)) {
		if (error) {
			x->refused++;
		} else {
			x->diverted++;
		}
	} else {
		return false;
	}
	hidpp_next(x);
	return true;
}

bool hidpp_rx(struct hidpp *x, struct device_battery *b, const struct hidpp_msg *m,
	      uint16_t *usage)
{
	if (hidpp_reply(x, b, m)) {
		return false;
	}
	if (x->battery != 0 && m->feature == x->battery && m->function == HIDPP_BATTERY_EVENT) {
		read_battery(b, m);
		return false;
	}
	if (x->controls == 0 || m->feature != x->controls ||
	    m->function != HIDPP_DIVERTED_EVENT || !m->is_long) {
		return false;
	}
	*usage = 0;
	/* The event lists up to four held controls. */
	for (size_t i = 0; i < 8 && *usage == 0; i += 2) {
		*usage = diverted_usage((uint16_t)(m->params[i] << 8 | m->params[i + 1]));
	}
	return true;
}

bool hidpp_take_event(struct hidpp *x, struct hidpp_event *out)
{
	if (!x->event.pending) {
		return false;
	}
	*out = x->event;
	x->event.pending = false;
	return true;
}

void hidpp_want_hires(struct hidpp *x, bool on)
{
	if (on == x->hires_wanted) {
		return;
	}
	x->hires_wanted = on;
	if (x->step == HIDPP_IDLE && wanted(x, HIDPP_SET_WHEEL)) {
		hidpp_step(x, HIDPP_SET_WHEEL);
	}
}

uint8_t hidpp_wheel_steps(const struct hidpp *x)
{
	return x->wheel_mode == HIDPP_WHEEL_HIRES ? x->wheel_multiplier : 1;
}
