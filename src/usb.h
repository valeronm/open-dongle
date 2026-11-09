#ifndef USB_H
#define USB_H

#include <stdbool.h>
#include <stdint.h>
#include "reports.h"

/* Callbacks run in the USB stack's thread. */
struct usb_callbacks {
	void (*on_live)(void);
	void (*on_leds)(uint8_t leds);
};

int usb_start(const struct usb_callbacks *cb);

/* Returns when the host has read the report or a reset or disable has cancelled the transfer. */
int usb_send(const struct report *r);
bool usb_live(void);
bool usb_suspended(void);
int usb_wakeup(void);

#endif
