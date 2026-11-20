#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

#include "diagwire.h"

#define DEVICE_KINDS (DEVICE_KIND_MOUSE + 1)
/* A device of neither kind, or one whose kind is not known yet. */
#define DEVICE_NO_KIND DEVICE_KINDS

#define DEVICE_BATTERY_UNKNOWN                                                                    \
	((struct device_battery){.level = DEVICE_NO_BATTERY, .charging = DEVICE_CHARGING_UNKNOWN})

static inline const char *device_kind_name(enum device_kind kind)
{
	return kind == DEVICE_KIND_MOUSE ? "mouse" : "keyboard";
}

#endif
