#include "pairwin.h"

void pairwin_open(struct pairwin *w, enum pairwin_cause cause, bool paired, uint32_t now_ms)
{
	w->open = true;
	w->until_paired = !paired;
	w->holder = DEVICE_NO_KIND;
	w->cause = cause;
	w->opened_ms = now_ms;
}

bool pairwin_is_open(const struct pairwin *w, uint32_t now_ms)
{
	return w->open &&
	       (w->until_paired || w->holder != DEVICE_NO_KIND || now_ms - w->opened_ms < PAIRWIN_MS);
}

uint32_t pairwin_left_ms(const struct pairwin *w, uint32_t now_ms)
{
	if (w->until_paired || w->holder != DEVICE_NO_KIND || !pairwin_is_open(w, now_ms)) {
		return 0;
	}
	return PAIRWIN_MS - (now_ms - w->opened_ms);
}

void pairwin_close(struct pairwin *w)
{
	w->open = false;
}

bool pairwin_hold(struct pairwin *w, enum device_kind holder, uint32_t now_ms)
{
	if (!pairwin_is_open(w, now_ms)) {
		return false;
	}
	w->holder = holder;
	return true;
}

enum device_kind pairwin_holder(const struct pairwin *w)
{
	return w->open ? w->holder : DEVICE_NO_KIND;
}

bool pairwin_end_attempt(struct pairwin *w, enum device_kind holder)
{
	if (holder == DEVICE_NO_KIND || pairwin_holder(w) != holder) {
		return false;
	}
	pairwin_close(w);
	return true;
}

void pairwin_link_up(struct pairwin *w)
{
	if (w->cause == PAIRWIN_BOOT && !w->until_paired) {
		w->open = false;
	}
}

bool pairwin_ran_out(struct pairwin *w, uint32_t now_ms)
{
	if (!w->open || pairwin_is_open(w, now_ms)) {
		return false;
	}
	w->open = false;
	return true;
}
