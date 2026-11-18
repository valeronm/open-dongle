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

static inline const char *device_kind_name(enum device_kind kind)
{
	return kind == DEVICE_MOUSE ? "mouse" : "keyboard";
}

#endif
