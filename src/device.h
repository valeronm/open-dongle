#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

enum device_kind {
	DEVICE_KEYBOARD,
	DEVICE_MOUSE,
	DEVICE_KINDS,
	/* A device of neither kind, or one whose kind is not known yet. */
	DEVICE_NO_KIND = DEVICE_KINDS,
};

/* The most slots any protocol has; a slot is a numbered place for one device. */
#define DEVICE_SLOTS 2
#define DEVICE_NO_SLOT UINT8_MAX

#define DEVICE_NO_BATTERY UINT8_MAX

enum device_charging {
	DEVICE_DISCHARGING,
	DEVICE_CHARGING,
	/* On power with the battery full. */
	DEVICE_CHARGED,
	DEVICE_BATTERY_FAULT,
	DEVICE_CHARGE_UNKNOWN = UINT8_MAX,
};

/* A device can report its charging while its level is unknown. */
struct device_battery {
	/* Percent, or DEVICE_NO_BATTERY while unknown. */
	uint8_t level;
	enum device_charging charging;
};

#define DEVICE_BATTERY_UNKNOWN                                                                    \
	((struct device_battery){.level = DEVICE_NO_BATTERY, .charging = DEVICE_CHARGE_UNKNOWN})

static inline const char *device_kind_name(enum device_kind kind)
{
	return kind == DEVICE_MOUSE ? "mouse" : "keyboard";
}

#endif
