#include "pairwin.h"

void pairwin_open(struct pairwin *w, enum pairwin_cause cause, bool paired, uint32_t now_ms)
{
	w->open = true;
	w->until_paired = !paired;
	w->cause = cause;
	w->opened_ms = now_ms;
}

bool pairwin_is_open(const struct pairwin *w, uint32_t now_ms)
{
	return w->open && (w->until_paired || now_ms - w->opened_ms < PAIRWIN_MS);
}

void pairwin_paired(struct pairwin *w)
{
	w->open = false;
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
