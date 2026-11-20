#ifndef BTPOLICY_H
#define BTPOLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "device.h"

#define BTPOLICY_STORED_MAX 8
#define BTPOLICY_RETRY_MS 1000
#define BTPOLICY_QUICK_RETRIES 3
/* The address type, then the six address bytes. */
#define BTPOLICY_ADDR_LEN 7
#define BTPOLICY_NAME_MAX 32
#define BTPOLICY_APPEARANCE_KEYBOARD 0x03c1
#define BTPOLICY_APPEARANCE_MOUSE 0x03c2

struct btpolicy_advert {
	uint8_t addr[BTPOLICY_ADDR_LEN];
	bool connectable;
	bool directed;
	bool hid_service;
	bool stored;
	/* From the appearance for a new device, from the stored list for a stored one. */
	enum device_kind kind;
	/* Empty unless the advert or its scan response named the device. */
	char name[BTPOLICY_NAME_MAX + 1];
};

enum btpolicy_state {
	BTPOLICY_IDLE,
	BTPOLICY_CONNECTING,
	BTPOLICY_CONNECTED,
	/* The device's report map is read and its reports are forwarded. */
	BTPOLICY_READY,
};

enum btpolicy_want {
	BTPOLICY_IGNORE,
	BTPOLICY_CONNECT,
	/* The slot's device is to be dropped first; the advert is wanted once the slot is idle. */
	BTPOLICY_REPLACE,
};

enum btpolicy_scan {
	BTPOLICY_SCAN_OFF,
	BTPOLICY_SCAN_PASSIVE,
	BTPOLICY_SCAN_ACTIVE,
};

enum btpolicy_failure {
	/* The device or the dongle turned the pairing down. */
	BTPOLICY_FAILURE_REFUSAL,
	/* The device no longer holds its side of the bond. */
	BTPOLICY_FAILURE_KEY_MISSING,
	/* The link dropped before security was settled. */
	BTPOLICY_FAILURE_LINK,
};

enum btpolicy_pairing {
	BTPOLICY_PAIRING_KEPT,
	BTPOLICY_PAIRING_FORGOTTEN,
};

enum btpolicy_security {
	BTPOLICY_PAIRING_REFUSED,
	/* The same device refused earlier in this window. */
	BTPOLICY_REFUSED_BEFORE,
	BTPOLICY_BOND_LOST,
	BTPOLICY_SECURITY_FAILED,
};

struct btpolicy_slot {
	enum btpolicy_state state;
	uint8_t peer[BTPOLICY_ADDR_LEN];
	bool peer_stored;
	bool dropping;
	bool retry_pending;
	uint8_t quick_retries;
	uint32_t retry_at_ms;
};

struct btpolicy {
	struct btpolicy_slot slots[DEVICE_KINDS];
	bool window_open;
	bool passkey_demanded;
	/* Devices that refused to pair in the open window. */
	uint8_t refused[BTPOLICY_STORED_MAX][BTPOLICY_ADDR_LEN];
	uint8_t refused_count;
};

enum device_kind btpolicy_kind_of(uint16_t appearance);
void btpolicy_init(struct btpolicy *p);
/* False for an advert no rule can want, whatever the state. */
bool btpolicy_interesting(const struct btpolicy_advert *a);
/* A stored device is not connected to while a pairing window is open, and only one device is set
 * up at a time. */
enum btpolicy_want btpolicy_wants(const struct btpolicy *p, const struct btpolicy_advert *a,
				  bool window_open, uint8_t stored_count, uint32_t now_ms);
void btpolicy_connecting(struct btpolicy *p, const struct btpolicy_advert *a);
void btpolicy_dropping(struct btpolicy *p, enum device_kind kind);
void btpolicy_connected(struct btpolicy *p, enum device_kind kind);
/* The device being connected to or connected in the slot, or NULL. */
const uint8_t *btpolicy_peer(const struct btpolicy *p, enum device_kind kind);
/* A keyboard that paired without the passkey its window demands is to be forgotten; it is not
 * wanted again until the window closes. A mouse is always kept. */
enum btpolicy_pairing btpolicy_paired(struct btpolicy *p, enum device_kind kind,
				      bool passkey_used);
void btpolicy_ready(struct btpolicy *p, enum device_kind kind);
bool btpolicy_is_ready(const struct btpolicy *p, enum device_kind kind);
/* A stored device whose connection ended before it was ready leaves its slot alone for
 * BTPOLICY_RETRY_MS, except for the first BTPOLICY_QUICK_RETRIES in a row that never joined the
 * connection. */
void btpolicy_disconnected(struct btpolicy *p, enum device_kind kind, uint32_t now_ms,
			   bool never_joined);
/* A device that refuses to pair is not wanted again until the window closes. */
enum btpolicy_security btpolicy_security_failed(struct btpolicy *p, enum device_kind kind,
						enum btpolicy_failure why);
/* Told the pairing window's state every tick; the refusals are forgotten when it closes. */
void btpolicy_window(struct btpolicy *p, bool open, bool passkey_demanded);
/* Active while a pairing window is open, since a new device may give its appearance only in its
 * scan response. */
enum btpolicy_scan btpolicy_scan(const struct btpolicy *p);

#endif
