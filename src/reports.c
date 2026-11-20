#include <string.h>
#include "reports.h"

#define MODIFIERS_BYTE (REPORT_MODIFIER_FIRST / 8)
#define BOOT_FIRST_SLOT 2

void reports_init(struct reports *r)
{
	memset(r, 0, sizeof(*r));
	for (int kind = 0; kind < REPORT_KINDS; kind++) {
		r->current[kind].kind = kind;
	}
	r->boot.kind = REPORT_USB_BOOT;
	r->nkro.kind = REPORT_USB_NKRO;
}

static enum report_iface reports_iface(enum report_usb_kind kind)
{
	switch (kind) {
	case REPORT_USB_BOOT:
		return REPORT_IFACE_KEYBOARD;
	case REPORT_USB_MOUSE:
		return REPORT_IFACE_MOUSE;
	default:
		return REPORT_IFACE_EXTRA;
	}
}

int reports_clamp(int value, int min, int max)
{
	return value < min ? min : value > max ? max : value;
}

struct report reports_consumer(uint16_t changed, uint16_t other)
{
	return (struct report){.kind = REPORT_CONSUMER, .usage = changed != 0 ? changed : other};
}

static bool has_key(const uint8_t keys[REPORT_KEYS_LEN], unsigned usage)
{
	return keys[usage / 8] & (1u << (usage % 8));
}

bool reports_key_held(const struct report *r, uint8_t usage)
{
	return has_key(r->keys, usage);
}

static void hold(uint8_t keys[REPORT_KEYS_LEN], unsigned usage)
{
	keys[usage / 8] |= 1u << (usage % 8);
}

struct report reports_held_keys(uint8_t modifiers, const uint8_t slots[REPORT_KEY_SLOTS])
{
	struct report r = {.kind = REPORT_KEYBOARD};

	r.keys[MODIFIERS_BYTE] = modifiers;
	for (int i = 0; i < REPORT_KEY_SLOTS; i++) {
		if (slots[i] >= REPORT_KEY_FIRST) {
			hold(r.keys, slots[i]);
		}
	}
	return r;
}

bool reports_keys_empty(const struct report *r)
{
	for (size_t i = 0; i < sizeof(r->keys); i++) {
		if (r->keys[i] != 0) {
			return false;
		}
	}
	return true;
}

static struct report_usb to_usb(const struct report *r)
{
	switch (r->kind) {
	case REPORT_CONSUMER:
		return (struct report_usb){.kind = REPORT_USB_CONSUMER, .usage = r->usage};
	case REPORT_SYSTEM:
		return (struct report_usb){.kind = REPORT_USB_SYSTEM, .usage = r->usage};
	default:
		return (struct report_usb){.kind = REPORT_USB_MOUSE, .mouse = r->mouse};
	}
}

static bool has_motion(const struct report *r)
{
	const struct report_mouse *m = &r->mouse;

	return r->kind == REPORT_MOUSE && (m->x || m->y || m->wheel || m->pan);
}

/* The sender takes a report off the queue before sending it, so the last queued report is unsent
 * and a mouse report with the same buttons can still add its motion to it. */
static bool merged_into_last(struct report_queue *q, const struct report_usb *next)
{
	struct report_mouse *last;

	if (next->kind != REPORT_USB_MOUSE || q->count == 0) {
		return false;
	}
	last = &q->items[(q->head + q->count - 1) % REPORTS_QUEUE_LEN].report.mouse;
	if (last->buttons != next->mouse.buttons) {
		return false;
	}
	last->x = (int16_t)reports_clamp(last->x + next->mouse.x, INT16_MIN, INT16_MAX);
	last->y = (int16_t)reports_clamp(last->y + next->mouse.y, INT16_MIN, INT16_MAX);
	last->wheel = (int8_t)reports_clamp(last->wheel + next->mouse.wheel, INT8_MIN, INT8_MAX);
	last->pan = (int8_t)reports_clamp(last->pan + next->mouse.pan, INT8_MIN, INT8_MAX);
	return true;
}

static void push(struct reports *r, const struct report_usb *item, struct report_stamp stamp)
{
	struct report_queue *q = &r->queues[reports_iface(item->kind)];

	if (merged_into_last(q, item)) {
		return;
	}
	if (q->count == REPORTS_QUEUE_LEN) {
		/* The host skips the oldest intermediate state. */
		q->head = (q->head + 1) % REPORTS_QUEUE_LEN;
		q->count--;
		r->merges++;
	}
	q->items[(q->head + q->count) % REPORTS_QUEUE_LEN] =
		(struct report_entry){.report = *item, .stamp = stamp};
	q->count++;
}

static bool same_state(const struct report *a, const struct report *b)
{
	if (a->kind == REPORT_KEYBOARD) {
		return memcmp(a->keys, b->keys, sizeof(a->keys)) == 0;
	}
	if (a->kind == REPORT_MOUSE) {
		return a->mouse.buttons == b->mouse.buttons;
	}
	return a->usage == b->usage;
}

static unsigned queue(struct reports *r, const struct report_usb *item, struct report_stamp stamp)
{
	if (r->live) {
		push(r, item, stamp);
	}
	return 1u << reports_iface(item->kind);
}

static void place_boot(const uint8_t was[REPORT_KEYS_LEN], const uint8_t keys[REPORT_KEYS_LEN],
		       const uint8_t old_boot[REPORT_KEYBOARD_LEN], uint8_t boot[REPORT_KEYBOARD_LEN])
{
	int slot = BOOT_FIRST_SLOT;

	memset(boot, 0, REPORT_KEYBOARD_LEN);
	boot[0] = keys[MODIFIERS_BYTE];
	for (int i = BOOT_FIRST_SLOT; i < REPORT_KEYBOARD_LEN; i++) {
		if (old_boot[i] != 0 && has_key(keys, old_boot[i])) {
			boot[i] = old_boot[i];
		}
	}
	for (unsigned byte = 0; byte < REPORT_KEYS_LEN; byte++) {
		unsigned fresh = byte == MODIFIERS_BYTE ? 0 : keys[byte] & ~was[byte] & 0xffu;

		for (unsigned usage = byte * 8; fresh != 0; usage++, fresh >>= 1) {
			if (!(fresh & 1)) {
				continue;
			}
			while (slot < REPORT_KEYBOARD_LEN && boot[slot] != 0) {
				slot++;
			}
			if (slot == REPORT_KEYBOARD_LEN) {
				return;
			}
			boot[slot] = (uint8_t)usage;
		}
	}
}

static void nkro_of(const uint8_t keys[REPORT_KEYS_LEN], const uint8_t boot[REPORT_KEYBOARD_LEN],
		    uint8_t nkro[REPORT_NKRO_LEN])
{
	memcpy(nkro, keys, REPORT_NKRO_LEN);
	for (int i = BOOT_FIRST_SLOT; i < REPORT_KEYBOARD_LEN; i++) {
		if (boot[i] < REPORT_NKRO_KEYS) {
			nkro[boot[i] / 8] &= (uint8_t)~(1u << (boot[i] % 8));
		}
	}
}

static unsigned update_keys(struct reports *r, const struct report *next, struct report_stamp stamp)
{
	struct report *current = &r->current[REPORT_KEYBOARD];
	uint8_t boot[REPORT_KEYBOARD_LEN];
	uint8_t nkro[REPORT_NKRO_LEN];
	unsigned changed = 0;

	place_boot(current->keys, next->keys, r->boot.boot, boot);
	nkro_of(next->keys, boot, nkro);
	*current = *next;
	if (memcmp(boot, r->boot.boot, sizeof(boot)) != 0) {
		memcpy(r->boot.boot, boot, sizeof(boot));
		changed |= queue(r, &r->boot, stamp);
	}
	if (memcmp(nkro, r->nkro.nkro, sizeof(nkro)) != 0) {
		memcpy(r->nkro.nkro, nkro, sizeof(nkro));
		changed |= queue(r, &r->nkro, stamp);
	}
	return changed;
}

unsigned reports_update(struct reports *r, const struct report *next, struct report_stamp stamp)
{
	struct report *current = &r->current[next->kind];
	struct report_usb out;

	if (next->kind == REPORT_KEYBOARD) {
		return update_keys(r, next, stamp);
	}
	if (same_state(current, next) && !has_motion(next)) {
		return 0;
	}
	*current = *next;
	if (next->kind == REPORT_MOUSE) {
		/* A mouse's held state is its buttons; its motion is sent once. */
		current->mouse = (struct report_mouse){.buttons = next->mouse.buttons};
	}
	out = to_usb(next);
	return queue(r, &out, stamp);
}

void reports_release_all(struct reports *r)
{
	for (int kind = 0; kind < REPORT_KINDS; kind++) {
		struct report released = {.kind = kind};

		reports_update(r, &released, REPORT_UNSTAMPED);
	}
}

bool reports_any_held(const struct reports *r)
{
	for (int kind = 0; kind < REPORT_KINDS; kind++) {
		struct report released = {.kind = kind};

		if (!same_state(&r->current[kind], &released)) {
			return true;
		}
	}
	return false;
}

void reports_set_live(struct reports *r, bool live)
{
	if (live == r->live) {
		return;
	}
	r->live = live;
	for (int iface = 0; iface < REPORT_IFACES; iface++) {
		r->queues[iface].head = 0;
		r->queues[iface].count = 0;
	}
	if (live) {
		push(r, &r->boot, REPORT_UNSTAMPED);
		for (int kind = 0; kind < REPORT_KINDS; kind++) {
			struct report_usb out;

			if (kind == REPORT_KEYBOARD) {
				continue;
			}
			out = to_usb(&r->current[kind]);
			push(r, &out, REPORT_UNSTAMPED);
		}
		push(r, &r->nkro, REPORT_UNSTAMPED);
	}
}

bool reports_peek(const struct reports *r, enum report_iface iface, struct report_entry *out)
{
	const struct report_queue *q = &r->queues[iface];

	if (q->count == 0) {
		return false;
	}
	*out = q->items[q->head];
	return true;
}

void reports_pop(struct reports *r, enum report_iface iface)
{
	struct report_queue *q = &r->queues[iface];

	if (q->count > 0) {
		q->head = (q->head + 1) % REPORTS_QUEUE_LEN;
		q->count--;
	}
}

void reports_delivered(struct reports *r, const struct report_entry *sent, uint32_t now_us)
{
	struct report_latency *l;
	uint32_t delay_us;

	if (sent->stamp.slot >= DEVICE_SLOTS) {
		return;
	}
	l = &r->latency[sent->stamp.slot];
	delay_us = now_us - sent->stamp.received_us;
	l->count++;
	l->sum_us += delay_us;
	if (delay_us > l->max_us) {
		l->max_us = delay_us;
	}
}

uint32_t reports_latency_average_us(const struct report_latency *l)
{
	return l->count == 0 ? UINT32_MAX : (uint32_t)(l->sum_us / l->count);
}

size_t reports_encode_mouse(const struct report_mouse *m, bool boot,
			    uint8_t out[REPORT_MOUSE_LEN])
{
	out[0] = (uint8_t)m->buttons;
	if (boot) {
		out[1] = (uint8_t)reports_clamp(m->x, -127, 127);
		out[2] = (uint8_t)reports_clamp(m->y, -127, 127);
		return REPORT_MOUSE_BOOT_LEN;
	}
	out[1] = (uint8_t)(m->buttons >> 8);
	out[2] = (uint8_t)m->x;
	out[3] = (uint8_t)((uint16_t)m->x >> 8);
	out[4] = (uint8_t)m->y;
	out[5] = (uint8_t)((uint16_t)m->y >> 8);
	out[6] = (uint8_t)m->wheel;
	out[7] = (uint8_t)m->pan;
	return REPORT_MOUSE_LEN;
}

int reports_scale_wheel(int *carry, int value, int from_steps, int to_steps)
{
	int total = *carry + value * to_steps;
	int out = total / from_steps;

	*carry = total - out * from_steps;
	return out;
}
