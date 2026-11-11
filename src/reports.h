#ifndef REPORTS_H
#define REPORTS_H

#include <stdbool.h>
#include <stdint.h>
#include "nuphy.h"

#define REPORTS_QUEUE_LEN 64

enum report_kind {
	REPORT_KEYBOARD,
	REPORT_CONSUMER,
	REPORT_SYSTEM,
	REPORT_KINDS,
};

struct report {
	enum report_kind kind;
	union {
		uint8_t keyboard[NUPHY_KEYBOARD_LEN];
		uint16_t usage;
	};
};

enum report_iface {
	REPORT_IFACE_KEYBOARD,
	REPORT_IFACE_EXTRA,
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

/* Returns true when the message changed a report's state. */
bool reports_on_message(struct reports *r, const struct nuphy_msg *m);
void reports_release_all(struct reports *r);

/*
 * While not live (USB suspended or unconfigured) only the current state is kept; going live
 * queues the current state of every report kind so the host resynchronizes.
 */
void reports_set_live(struct reports *r, bool live);
bool reports_peek(const struct reports *r, enum report_iface iface, struct report *out);
void reports_pop(struct reports *r, enum report_iface iface);

#endif
