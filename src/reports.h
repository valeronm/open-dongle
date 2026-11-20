#ifndef REPORTS_H
#define REPORTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device.h"

#define REPORTS_QUEUE_LEN 64
#define REPORT_KEYBOARD_LEN 8
/* One bit per keyboard-page usage, bit u % 8 of byte u / 8. */
#define REPORT_KEYS_LEN 32
/* Keyboard-page usages below this are no key and the error codes. */
#define REPORT_KEY_FIRST 0x04
#define REPORT_MODIFIER_FIRST 0xe0
#define REPORT_KEY_SLOTS 6
/* The NKRO report covers the keyboard-page usages below the modifiers. */
#define REPORT_NKRO_KEYS REPORT_MODIFIER_FIRST
#define REPORT_NKRO_LEN (REPORT_NKRO_KEYS / 8)
#define REPORT_MOUSE_LEN 8
/* The wheel steps per notch the USB mouse offers a host that turns on high resolution. */
#define REPORT_WHEEL_HIRES_STEPS 8
#define REPORT_MOUSE_BOOT_LEN 3
/* The usages the USB report descriptors declare. */
#define REPORT_CONSUMER_MAX 0x2a0
#define REPORT_SYSTEM_MIN 0x81
#define REPORT_SYSTEM_MAX 0x83

enum report_kind {
	REPORT_KEYBOARD,
	REPORT_CONSUMER,
	REPORT_SYSTEM,
	REPORT_MOUSE,
	REPORT_KINDS,
};

struct report_mouse {
	uint16_t buttons;
	int16_t x, y;
	int8_t wheel, pan;
};

struct report {
	enum report_kind kind;
	union {
		/* The held keys, modifiers included as 0xe0-0xe7. */
		uint8_t keys[REPORT_KEYS_LEN];
		uint16_t usage;
		struct report_mouse mouse;
	};
};

enum report_usb_kind {
	REPORT_USB_BOOT,
	REPORT_USB_NKRO,
	REPORT_USB_CONSUMER,
	REPORT_USB_SYSTEM,
	REPORT_USB_MOUSE,
};

struct report_usb {
	enum report_usb_kind kind;
	union {
		uint8_t boot[REPORT_KEYBOARD_LEN];
		uint8_t nkro[REPORT_NKRO_LEN];
		uint16_t usage;
		struct report_mouse mouse;
	};
};

struct report_stamp {
	/* DEVICE_NO_SLOT on a report the dongle makes up itself, such as a release or a resend of
	 * the state. */
	uint8_t slot;
	uint32_t received_us;
};

#define REPORT_UNSTAMPED ((struct report_stamp){.slot = DEVICE_NO_SLOT})

struct report_entry {
	struct report_usb report;
	struct report_stamp stamp;
};

/* The delay from each stamped report's receipt to its delivery to the host. */
struct report_latency {
	uint32_t count;
	uint32_t max_us;
	uint64_t sum_us;
};

enum report_iface {
	REPORT_IFACE_KEYBOARD,
	REPORT_IFACE_EXTRA,
	REPORT_IFACE_MOUSE,
	REPORT_IFACES,
};

struct report_queue {
	struct report_entry items[REPORTS_QUEUE_LEN];
	uint8_t head;
	uint8_t count;
};

struct reports {
	struct report_queue queues[REPORT_IFACES];
	struct report current[REPORT_KINDS];
	/* The last boot and NKRO reports; a held key keeps its place in them until released. */
	struct report_usb boot, nkro;
	bool live;
	uint32_t merges;
	struct report_latency latency[DEVICE_SLOTS];
};

void reports_init(struct reports *r);
int reports_clamp(int value, int min, int max);
/* The consumer report of two sources that each hold one usage: the usage that changed wins, and a
 * released one gives way to the other source's held usage. */
struct report reports_consumer(uint16_t changed, uint16_t other);
/* Converts wheel steps of from_steps per notch into to_steps per notch, keeping the part of a step
 * not yet sent in carry. */
int reports_scale_wheel(int *carry, int value, int from_steps, int to_steps);

/* A boot report's modifier byte and key slots as held keys; usages below REPORT_KEY_FIRST are
 * not keys. */
struct report reports_held_keys(uint8_t modifiers, const uint8_t slots[REPORT_KEY_SLOTS]);
bool reports_key_held(const struct report *r, uint8_t usage);
bool reports_keys_empty(const struct report *r);

/*
 * Returns one bit per enum report_iface whose report changed, mouse motion counting as a change,
 * whether or not it was queued. A keyboard report's held keys are split into the boot report and
 * the NKRO report: a held key keeps its place, a newly held key takes the first empty boot slot in
 * usage order or else the NKRO report, and a key 0xe0 or above with no free slot is dropped.
 */
unsigned reports_update(struct reports *r, const struct report *next, struct report_stamp stamp);
void reports_release_all(struct reports *r);
/* Returns true while the host sees any key, modifier or media usage down. */
bool reports_any_held(const struct reports *r);

/*
 * While not live (USB suspended or unconfigured) only the current state is kept; going live
 * queues the current state of every report kind so the host resynchronizes.
 */
void reports_set_live(struct reports *r, bool live);
bool reports_peek(const struct reports *r, enum report_iface iface, struct report_entry *out);
void reports_pop(struct reports *r, enum report_iface iface);
/* Counts a stamped entry's delay toward its slot's latency once the host has it. */
void reports_delivered(struct reports *r, const struct report_entry *sent, uint32_t now_us);
/* UINT32_MAX before any stamped report is delivered. */
uint32_t reports_latency_average_us(const struct report_latency *l);
/* Returns REPORT_MOUSE_BOOT_LEN in boot protocol, REPORT_MOUSE_LEN otherwise. */
size_t reports_encode_mouse(const struct report_mouse *m, bool boot,
			    uint8_t out[REPORT_MOUSE_LEN]);

#endif
