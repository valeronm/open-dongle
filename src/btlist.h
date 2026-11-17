#ifndef BTLIST_H
#define BTLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btpolicy.h"

#define BTLIST_NAME_LEN 24

struct btlist_entry {
	uint8_t addr[BTPOLICY_ADDR_LEN];
	/* Padded with zeros; not terminated when it fills the field. */
	char name[BTLIST_NAME_LEN];
	bool no_passkey;
};

/* The stored keyboards, the most recently connected first. */
struct btlist {
	struct btlist_entry entries[BTPOLICY_STORED_MAX];
	uint8_t count;
};

/* Each of these returns true when it changed the list. */

/* Drops the entries no bond backs and adds a nameless entry, last, for each bond without one. */
bool btlist_reconcile(struct btlist *l, const uint8_t addrs[][BTPOLICY_ADDR_LEN], size_t n);
/* Moves the keyboard to the front and records its name. */
bool btlist_connected(struct btlist *l, const uint8_t *addr, const char *name);
bool btlist_paired(struct btlist *l, const uint8_t *addr, bool no_passkey);
bool btlist_remove(struct btlist *l, const uint8_t *addr);

bool btlist_has(const struct btlist *l, const uint8_t *addr);

#endif
