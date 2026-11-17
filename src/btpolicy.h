#ifndef BTPOLICY_H
#define BTPOLICY_H

#include <stdbool.h>
#include <stdint.h>

#define BTPOLICY_STORED_MAX 4
#define BTPOLICY_RETRY_MS 1000
/* The address type, then the six address bytes. */
#define BTPOLICY_ADDR_LEN 7

struct btpolicy_advert {
	uint8_t addr[BTPOLICY_ADDR_LEN];
	bool connectable;
	bool directed;
	bool hid_service;
	bool stored;
};

enum btpolicy_state {
	BTPOLICY_IDLE,
	BTPOLICY_CONNECTING,
	BTPOLICY_CONNECTED,
	/* The keyboard's report map is read and its reports are forwarded. */
	BTPOLICY_READY,
};

enum btpolicy_failure {
	/* The keyboard or the dongle turned the pairing down. */
	BTPOLICY_FAILURE_REFUSAL,
	/* The keyboard no longer holds its side of the bond. */
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
	/* The same keyboard refused earlier in this window. */
	BTPOLICY_REFUSED_BEFORE,
	BTPOLICY_BOND_LOST,
	BTPOLICY_SECURITY_FAILED,
};

struct btpolicy {
	enum btpolicy_state state;
	uint8_t peer[BTPOLICY_ADDR_LEN];
	bool peer_stored;
	bool window_open;
	bool passkey_demanded;
	bool retry_pending;
	uint32_t retry_at_ms;
	/* Keyboards that refused to pair in the open window. */
	uint8_t refused[BTPOLICY_STORED_MAX][BTPOLICY_ADDR_LEN];
	uint8_t refused_count;
};

void btpolicy_init(struct btpolicy *p);
/* False for an advert no rule can want, whatever the state. */
bool btpolicy_interesting(const struct btpolicy_advert *a);
/* A stored keyboard is not connected to while a pairing window is open. */
bool btpolicy_wants(const struct btpolicy *p, const struct btpolicy_advert *a, bool window_open,
		    uint8_t stored_count, uint32_t now_ms);
void btpolicy_connecting(struct btpolicy *p, const struct btpolicy_advert *a);
void btpolicy_connected(struct btpolicy *p);
/* A keyboard that paired without the passkey its window demands is to be forgotten; it is not
 * wanted again until the window closes. */
enum btpolicy_pairing btpolicy_paired(struct btpolicy *p, bool passkey_used);
void btpolicy_ready(struct btpolicy *p);
bool btpolicy_is_ready(const struct btpolicy *p);
/* A stored keyboard whose connection ended before it was ready is left alone for
 * BTPOLICY_RETRY_MS. */
void btpolicy_disconnected(struct btpolicy *p, uint32_t now_ms);
/* A keyboard that refuses to pair is not wanted again until the window closes. */
enum btpolicy_security btpolicy_security_failed(struct btpolicy *p, enum btpolicy_failure why);
/* Told the pairing window's state every tick; the refusals are forgotten when it closes. */
void btpolicy_window(struct btpolicy *p, bool open, bool passkey_demanded);
bool btpolicy_scanning(const struct btpolicy *p);

#endif
