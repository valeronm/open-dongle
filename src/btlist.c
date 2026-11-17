#include <string.h>

#include "btlist.h"

#define NOT_LISTED UINT8_MAX

static uint8_t find(const struct btlist *l, const uint8_t *addr)
{
	for (uint8_t i = 0; i < l->count; i++) {
		if (memcmp(l->entries[i].addr, addr, BTPOLICY_ADDR_LEN) == 0) {
			return i;
		}
	}
	return NOT_LISTED;
}

static void remove_at(struct btlist *l, uint8_t at)
{
	memmove(&l->entries[at], &l->entries[at + 1],
		(l->count - at - 1) * sizeof(l->entries[0]));
	l->count--;
}

/* Puts the keyboard's entry first, creating it if there is room; NULL when there is not. */
static struct btlist_entry *to_front(struct btlist *l, const uint8_t *addr, bool *changed)
{
	struct btlist_entry e = {0};
	uint8_t at = find(l, addr);

	if (at == NOT_LISTED) {
		if (l->count == BTPOLICY_STORED_MAX) {
			return NULL;
		}
		memcpy(e.addr, addr, BTPOLICY_ADDR_LEN);
		at = l->count++;
		*changed = true;
	} else {
		e = l->entries[at];
		*changed = at != 0;
	}
	memmove(&l->entries[1], &l->entries[0], at * sizeof(l->entries[0]));
	l->entries[0] = e;
	return &l->entries[0];
}

bool btlist_reconcile(struct btlist *l, const uint8_t addrs[][BTPOLICY_ADDR_LEN], size_t n)
{
	bool changed = false;

	for (uint8_t i = l->count; i-- > 0;) {
		bool bonded = false;

		for (size_t j = 0; j < n; j++) {
			bonded |= memcmp(l->entries[i].addr, addrs[j], BTPOLICY_ADDR_LEN) == 0;
		}
		if (!bonded) {
			remove_at(l, i);
			changed = true;
		}
	}
	for (size_t j = 0; j < n && l->count < BTPOLICY_STORED_MAX; j++) {
		if (find(l, addrs[j]) == NOT_LISTED) {
			l->entries[l->count] = (struct btlist_entry){0};
			memcpy(l->entries[l->count++].addr, addrs[j], BTPOLICY_ADDR_LEN);
			changed = true;
		}
	}
	return changed;
}

bool btlist_connected(struct btlist *l, const uint8_t *addr, const char *name)
{
	char padded[BTLIST_NAME_LEN] = {0};
	bool changed = false;
	struct btlist_entry *e = to_front(l, addr, &changed);

	if (!e) {
		return false;
	}
	strncpy(padded, name, sizeof(padded));
	if (memcmp(e->name, padded, sizeof(padded)) != 0) {
		memcpy(e->name, padded, sizeof(padded));
		changed = true;
	}
	return changed;
}

bool btlist_paired(struct btlist *l, const uint8_t *addr, bool no_passkey)
{
	bool changed = false;
	struct btlist_entry *e = to_front(l, addr, &changed);

	if (!e) {
		return false;
	}
	if (e->no_passkey != no_passkey) {
		e->no_passkey = no_passkey;
		changed = true;
	}
	return changed;
}

bool btlist_remove(struct btlist *l, const uint8_t *addr)
{
	uint8_t at = find(l, addr);

	if (at == NOT_LISTED) {
		return false;
	}
	remove_at(l, at);
	return true;
}

bool btlist_has(const struct btlist *l, const uint8_t *addr)
{
	return find(l, addr) != NOT_LISTED;
}
