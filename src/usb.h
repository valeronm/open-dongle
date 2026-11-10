#ifndef USB_H
#define USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "diagproto.h"
#include "reports.h"

/* Callbacks run in the USB stack's thread or the system workqueue, never in an interrupt. */
struct usb_callbacks {
	void (*on_live)(void);
	void (*on_leds)(uint8_t leds);
	void (*on_diag_request)(const uint8_t *buf, size_t len);
};

int usb_start(const struct usb_callbacks *cb);

/* Returns when the host has read the report or a reset or disable has cancelled the transfer. */
int usb_send(const struct report *r);
bool usb_live(void);
bool usb_suspended(void);
int usb_wakeup(void);

/* Blocks until the host reads the report, or fails at once when the interface is not ready. */
int usb_diag_send(const uint8_t report[DIAG_REPORT_LEN]);
void usb_reboot_to_bootloader(void);

#endif
