#include <string.h>
#include "reports.h"

void reports_init(struct reports *r)
{
	memset(r, 0, sizeof(*r));
	for (int kind = 0; kind < REPORT_KINDS; kind++) {
		r->current[kind].kind = kind;
	}
}

static enum report_iface iface_of(enum report_kind kind)
{
	return kind == REPORT_KEYBOARD ? REPORT_IFACE_KEYBOARD : REPORT_IFACE_EXTRA;
}

static void push(struct reports *r, enum report_kind kind)
{
	struct report_queue *q = &r->queues[iface_of(kind)];

	if (q->count == REPORTS_QUEUE_LEN) {
		/* The host skips the oldest intermediate state. */
		q->head = (q->head + 1) % REPORTS_QUEUE_LEN;
		q->count--;
		r->merges++;
	}
	q->items[(q->head + q->count) % REPORTS_QUEUE_LEN] = r->current[kind];
	q->count++;
}

static bool same_state(const struct report *a, const struct report *b)
{
	if (a->kind == REPORT_KEYBOARD) {
		return memcmp(a->keyboard, b->keyboard, sizeof(a->keyboard)) == 0;
	}
	return a->usage == b->usage;
}

static bool update(struct reports *r, const struct report *next)
{
	struct report *current = &r->current[next->kind];

	if (same_state(current, next)) {
		return false;
	}
	*current = *next;
	if (r->live) {
		push(r, next->kind);
	}
	return true;
}

bool reports_on_message(struct reports *r, const struct nuphy_msg *m)
{
	struct report next;

	switch (m->kind) {
	case NUPHY_KEYBOARD:
		next.kind = REPORT_KEYBOARD;
		memcpy(next.keyboard, m->keyboard, sizeof(next.keyboard));
		break;
	case NUPHY_CONSUMER:
		next = (struct report){.kind = REPORT_CONSUMER, .usage = m->usage};
		break;
	case NUPHY_SYSTEM:
		next = (struct report){.kind = REPORT_SYSTEM, .usage = m->usage};
		break;
	default:
		return false;
	}
	return update(r, &next);
}

void reports_release_all(struct reports *r)
{
	for (int kind = 0; kind < REPORT_KINDS; kind++) {
		struct report released = {.kind = kind};

		update(r, &released);
	}
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
			push(r, kind);
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
