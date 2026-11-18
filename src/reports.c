#include <string.h>
#include "reports.h"

void reports_init(struct reports *r)
{
	memset(r, 0, sizeof(*r));
	for (int kind = 0; kind < REPORT_KINDS; kind++) {
		r->current[kind].kind = kind;
	}
}

enum report_iface reports_iface(enum report_kind kind)
{
	switch (kind) {
	case REPORT_KEYBOARD:
		return REPORT_IFACE_KEYBOARD;
	case REPORT_MOUSE:
		return REPORT_IFACE_MOUSE;
	default:
		return REPORT_IFACE_EXTRA;
	}
}

int reports_clamp(int value, int min, int max)
{
	return value < min ? min : value > max ? max : value;
}

static bool has_motion(const struct report *r)
{
	const struct report_mouse *m = &r->mouse;

	return r->kind == REPORT_MOUSE && (m->x || m->y || m->wheel || m->pan);
}

/* The sender takes a report off the queue before sending it, so the last queued report is unsent
 * and a mouse report with the same buttons can still add its motion to it. */
static bool merged_into_last(struct report_queue *q, const struct report *next)
{
	struct report_mouse *last;

	if (next->kind != REPORT_MOUSE || q->count == 0) {
		return false;
	}
	last = &q->items[(q->head + q->count - 1) % REPORTS_QUEUE_LEN].mouse;
	if (last->buttons != next->mouse.buttons) {
		return false;
	}
	last->x = (int16_t)reports_clamp(last->x + next->mouse.x, INT16_MIN, INT16_MAX);
	last->y = (int16_t)reports_clamp(last->y + next->mouse.y, INT16_MIN, INT16_MAX);
	last->wheel = (int8_t)reports_clamp(last->wheel + next->mouse.wheel, INT8_MIN, INT8_MAX);
	last->pan = (int8_t)reports_clamp(last->pan + next->mouse.pan, INT8_MIN, INT8_MAX);
	return true;
}

static void push(struct reports *r, const struct report *item)
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
	q->items[(q->head + q->count) % REPORTS_QUEUE_LEN] = *item;
	q->count++;
}

static bool same_state(const struct report *a, const struct report *b)
{
	if (a->kind == REPORT_KEYBOARD) {
		return memcmp(a->keyboard, b->keyboard, sizeof(a->keyboard)) == 0;
	}
	if (a->kind == REPORT_MOUSE) {
		return a->mouse.buttons == b->mouse.buttons;
	}
	return a->usage == b->usage;
}

bool reports_update(struct reports *r, const struct report *next)
{
	struct report *current = &r->current[next->kind];

	if (same_state(current, next) && !has_motion(next)) {
		return false;
	}
	*current = *next;
	if (next->kind == REPORT_MOUSE) {
		/* A mouse's held state is its buttons; its motion is sent once. */
		current->mouse = (struct report_mouse){.buttons = next->mouse.buttons};
	}
	if (r->live) {
		push(r, next);
	}
	return true;
}

void reports_release_all(struct reports *r)
{
	for (int kind = 0; kind < REPORT_KINDS; kind++) {
		struct report released = {.kind = kind};

		reports_update(r, &released);
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
		for (int kind = 0; kind < REPORT_KINDS; kind++) {
			push(r, &r->current[kind]);
		}
	}
}

bool reports_peek(const struct reports *r, enum report_iface iface, struct report *out)
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
