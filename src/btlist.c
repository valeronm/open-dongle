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

/* Puts the device's entry first, creating it if there is room; NULL when there is not. */
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

static bool set_name(struct btlist_entry *e, const char *name)
{
	char padded[BTLIST_NAME_LEN] = {0};

	strncpy(padded, name, sizeof(padded));
	if (memcmp(e->name, padded, sizeof(padded)) == 0) {
		return false;
	}
	memcpy(e->name, padded, sizeof(padded));
	return true;
}

bool btlist_connected(struct btlist *l, const uint8_t *addr, const char *name)
{
	bool changed = false;
	struct btlist_entry *e = to_front(l, addr, &changed);

	if (!e) {
		return false;
	}
	return set_name(e, name) || changed;
}

bool btlist_paired(struct btlist *l, const uint8_t *addr, enum device_kind kind,
		   bool no_passkey, const char *name)
{
	bool changed = false;
	struct btlist_entry *e = to_front(l, addr, &changed);

	if (!e) {
		return false;
	}
	if (e->no_passkey != no_passkey || e->kind != kind) {
		e->no_passkey = no_passkey;
		e->kind = kind;
		changed = true;
	}
	if (name[0] != '\0' && set_name(e, name)) {
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

const struct btlist_entry *btlist_entry(const struct btlist *l, const uint8_t *addr)
{
	uint8_t at = find(l, addr);

	return at == NOT_LISTED ? NULL : &l->entries[at];
}

bool btlist_has(const struct btlist *l, const uint8_t *addr)
{
	return btlist_entry(l, addr) != NULL;
}

bool btlist_has_kind(const struct btlist *l, enum device_kind kind)
{
	return btlist_latest(l, kind) != NULL;
}

const struct btlist_entry *btlist_latest(const struct btlist *l, enum device_kind kind)
{
	for (uint8_t i = 0; i < l->count; i++) {
		if (l->entries[i].kind == kind) {
			return &l->entries[i];
		}
	}
	return NULL;
}

enum device_kind btlist_kind(const struct btlist *l, const uint8_t *addr)
{
	const struct btlist_entry *e = btlist_entry(l, addr);

	return e ? e->kind : DEVICE_NO_KIND;
}

_Static_assert(BTPOLICY_ADDR_LEN == DIAG_ADDR_LEN, "a device list carries the whole address");
_Static_assert(BTPOLICY_STORED_MAX <= DIAG_STORED_MAX, "a device list carries every device");

uint8_t btlist_pairing(const struct btlist_entry *e, bool secure)
{
	return diag_pairing(secure, !e->no_passkey);
}

size_t btlist_devices(const struct btlist *l, const uint8_t *const connected[DEVICE_SLOTS],
		      bool (*secure)(const uint8_t addr[BTPOLICY_ADDR_LEN]),
		      struct diag_device out[BTPOLICY_STORED_MAX])
{
	for (uint8_t i = 0; i < l->count; i++) {
		const struct btlist_entry *e = &l->entries[i];

		out[i] = (struct diag_device){
			.kind = e->kind,
			.pairing = btlist_pairing(e, secure(e->addr)),
			.slot = DEVICE_NO_SLOT,
		};
		memcpy(out[i].addr, e->addr, sizeof(out[i].addr));
		memcpy(out[i].name, e->name, sizeof(out[i].name));
		for (uint8_t s = 0; s < DEVICE_SLOTS; s++) {
			if (connected[s] && memcmp(e->addr, connected[s], BTPOLICY_ADDR_LEN) == 0) {
				out[i].slot = s;
			}
		}
	}
	return l->count;
}
