#ifndef PAIRWIN_H
#define PAIRWIN_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"

#define PAIRWIN_MS 30000

enum pairwin_cause {
	PAIRWIN_REQUEST,
	PAIRWIN_BOOT,
};

struct pairwin {
	bool open;
	bool until_paired;
	enum device_kind holder;
	enum pairwin_cause cause;
	uint32_t opened_ms;
};

/* Opens pairing for PAIRWIN_MS, or until pairwin_close when nothing is paired yet. */
void pairwin_open(struct pairwin *w, enum pairwin_cause cause, bool paired, uint32_t now_ms);
bool pairwin_is_open(const struct pairwin *w, uint32_t now_ms);
void pairwin_close(struct pairwin *w);
/* Keeps an open window open past its PAIRWIN_MS until pairwin_close, or the holder's
 * pairwin_end_attempt; returns false and does nothing for a closed window. */
bool pairwin_hold(struct pairwin *w, enum device_kind holder, uint32_t now_ms);
/* DEVICE_NO_KIND for a window nothing holds open. */
enum device_kind pairwin_holder(const struct pairwin *w);
/* Closes the window if this holder holds it, and returns whether it did. */
bool pairwin_end_attempt(struct pairwin *w, enum device_kind holder);
/* 0 for a closed window and for one that stays open until paired or held. */
uint32_t pairwin_left_ms(const struct pairwin *w, uint32_t now_ms);
/* Returns true once when a window that was open has passed its PAIRWIN_MS, and closes it. */
bool pairwin_ran_out(struct pairwin *w, uint32_t now_ms);
/*
 * The paired keyboard connecting closes a window opened at boot; one opened on request stays open,
 * since it is there to pair another keyboard. A window waiting for a first pairing stays open too.
 */
void pairwin_link_up(struct pairwin *w);

#endif
