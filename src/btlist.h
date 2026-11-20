#ifndef BTLIST_H
#define BTLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btpolicy.h"
#include "device.h"

#define BTLIST_NAME_LEN DIAG_NAME_MAX

struct btlist_entry {
	uint8_t addr[BTPOLICY_ADDR_LEN];
	/* Padded with zeros; not terminated when it fills the field. */
	char name[BTLIST_NAME_LEN];
	bool no_passkey;
	/* An enum device_kind; zero, a keyboard, for an entry added by btlist_reconcile. */
	uint8_t kind;
};

/* The stored devices, the most recently connected first. */
struct btlist {
	struct btlist_entry entries[BTPOLICY_STORED_MAX];
	uint8_t count;
};

/* Each of these returns true when it changed the list. */

/* Drops the entries no bond backs and adds a nameless entry, last, for each bond without one. */
bool btlist_reconcile(struct btlist *l, const uint8_t addrs[][BTPOLICY_ADDR_LEN], size_t n);
/* Moves the device to the front and records its name. */
bool btlist_connected(struct btlist *l, const uint8_t *addr, const char *name);
/* Moves the device to the front and records its kind, its passkey mark and, unless empty, its
 * name. */
bool btlist_paired(struct btlist *l, const uint8_t *addr, enum device_kind kind,
		   bool no_passkey, const char *name);
bool btlist_remove(struct btlist *l, const uint8_t *addr);

/* NULL for an address not in the list. */
const struct btlist_entry *btlist_entry(const struct btlist *l, const uint8_t *addr);
bool btlist_has(const struct btlist *l, const uint8_t *addr);
bool btlist_has_kind(const struct btlist *l, enum device_kind kind);
/* The most recently connected entry of the kind, or NULL when none is listed. */
const struct btlist_entry *btlist_latest(const struct btlist *l, enum device_kind kind);
/* DEVICE_NO_KIND for an address not in the list. */
enum device_kind btlist_kind(const struct btlist *l, const uint8_t *addr);

/* secure tells whether the entry paired with LE Secure Connections. */
uint8_t btlist_pairing(const struct btlist_entry *e, bool secure);

/* connected[s] is the address of the device ready in slot s, or NULL; secure tells whether the
 * device at an address paired with LE Secure Connections. Returns the number of devices written. */
size_t btlist_devices(const struct btlist *l, const uint8_t *const connected[DEVICE_SLOTS],
		      bool (*secure)(const uint8_t addr[BTPOLICY_ADDR_LEN]),
		      struct diag_device out[BTPOLICY_STORED_MAX]);

#endif
