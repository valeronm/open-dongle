#ifndef PAIRWIN_H
#define PAIRWIN_H

#include <stdbool.h>
#include <stdint.h>

#define PAIRWIN_MS 30000

enum pairwin_cause {
	PAIRWIN_REQUEST,
	PAIRWIN_BOOT,
};

struct pairwin {
	bool open;
	bool until_paired;
	enum pairwin_cause cause;
	uint32_t opened_ms;
};

/* Opens pairing for PAIRWIN_MS, or until pairwin_paired when nothing is paired yet. */
void pairwin_open(struct pairwin *w, enum pairwin_cause cause, bool paired, uint32_t now_ms);
bool pairwin_is_open(const struct pairwin *w, uint32_t now_ms);
void pairwin_paired(struct pairwin *w);
/* Returns true once when a window that was open has passed its PAIRWIN_MS, and closes it. */
bool pairwin_ran_out(struct pairwin *w, uint32_t now_ms);
/*
 * The paired keyboard connecting closes a window opened at boot; one opened on request stays open,
 * since it is there to pair another keyboard. A window waiting for a first pairing stays open too.
 */
void pairwin_link_up(struct pairwin *w);

#endif
