#ifndef DEVICE_H
#define DEVICE_H

enum device_kind {
	DEVICE_KEYBOARD,
	DEVICE_MOUSE,
	DEVICE_KINDS,
	/* A device of neither kind, or one whose kind is not known yet. */
	DEVICE_NO_KIND = DEVICE_KINDS,
};

#endif
