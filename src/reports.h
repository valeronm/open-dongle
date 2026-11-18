#ifndef REPORTS_H
#define REPORTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REPORTS_QUEUE_LEN 64
#define REPORT_KEYBOARD_LEN 8
#define REPORT_MOUSE_LEN 8
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
		uint8_t keyboard[REPORT_KEYBOARD_LEN];
		uint16_t usage;
		struct report_mouse mouse;
	};
};

enum report_iface {
	REPORT_IFACE_KEYBOARD,
	REPORT_IFACE_EXTRA,
	REPORT_IFACE_MOUSE,
	REPORT_IFACES,
};

struct report_queue {
	struct report items[REPORTS_QUEUE_LEN];
	uint8_t head;
	uint8_t count;
};

struct reports {
	struct report_queue queues[REPORT_IFACES];
	struct report current[REPORT_KINDS];
	bool live;
	uint32_t merges;
};

void reports_init(struct reports *r);
enum report_iface reports_iface(enum report_kind kind);
int reports_clamp(int value, int min, int max);

/* Returns true when next changed its kind's state or carries mouse motion. */
bool reports_update(struct reports *r, const struct report *next);
void reports_release_all(struct reports *r);
/* Returns true while the host sees any key, modifier or media usage down. */
bool reports_any_held(const struct reports *r);

/*
 * While not live (USB suspended or unconfigured) only the current state is kept; going live
 * queues the current state of every report kind so the host resynchronizes.
 */
void reports_set_live(struct reports *r, bool live);
bool reports_peek(const struct reports *r, enum report_iface iface, struct report *out);
void reports_pop(struct reports *r, enum report_iface iface);
/* Returns REPORT_MOUSE_BOOT_LEN in boot protocol, REPORT_MOUSE_LEN otherwise. */
size_t reports_encode_mouse(const struct report_mouse *m, bool boot,
			    uint8_t out[REPORT_MOUSE_LEN]);

#endif
