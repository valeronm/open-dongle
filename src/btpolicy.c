#include <string.h>

#include "btpolicy.h"

enum btpolicy_kind btpolicy_kind_of(uint16_t appearance)
{
	switch (appearance) {
	case BTPOLICY_APPEARANCE_KEYBOARD:
		return BTPOLICY_KEYBOARD;
	case BTPOLICY_APPEARANCE_MOUSE:
		return BTPOLICY_MOUSE;
	default:
		return BTPOLICY_NO_KIND;
	}
}

void btpolicy_init(struct btpolicy *p)
{
	*p = (struct btpolicy){0};
}

static bool refused(const struct btpolicy *p, const uint8_t *addr)
{
	for (uint8_t i = 0; i < p->refused_count; i++) {
		if (memcmp(p->refused[i], addr, BTPOLICY_ADDR_LEN) == 0) {
			return true;
		}
	}
	return false;
}

static bool setting_up(const struct btpolicy *p)
{
	for (int k = 0; k < BTPOLICY_KINDS; k++) {
		if (p->slots[k].state == BTPOLICY_CONNECTING ||
		    p->slots[k].state == BTPOLICY_CONNECTED) {
			return true;
		}
	}
	return false;
}

bool btpolicy_interesting(const struct btpolicy_advert *a)
{
	return a->connectable && (a->stored || (a->hid_service && a->kind != BTPOLICY_NO_KIND));
}

enum btpolicy_want btpolicy_wants(const struct btpolicy *p, const struct btpolicy_advert *a,
				  bool window_open, uint8_t stored_count, uint32_t now_ms)
{
	const struct btpolicy_slot *s;

	if (!btpolicy_interesting(a) || a->kind == BTPOLICY_NO_KIND || setting_up(p)) {
		return BTPOLICY_IGNORE;
	}
	s = &p->slots[a->kind];
	if (s->dropping) {
		return BTPOLICY_IGNORE;
	}
	if (a->stored) {
		bool waited = !s->retry_pending || (int32_t)(now_ms - s->retry_at_ms) >= 0;

		return s->state == BTPOLICY_IDLE && !window_open && waited ? BTPOLICY_CONNECT
									    : BTPOLICY_IGNORE;
	}
	if (!window_open || a->directed || stored_count >= BTPOLICY_STORED_MAX ||
	    refused(p, a->addr)) {
		return BTPOLICY_IGNORE;
	}
	return s->state == BTPOLICY_IDLE ? BTPOLICY_CONNECT : BTPOLICY_REPLACE;
}

void btpolicy_connecting(struct btpolicy *p, const struct btpolicy_advert *a)
{
	struct btpolicy_slot *s = &p->slots[a->kind];

	s->state = BTPOLICY_CONNECTING;
	memcpy(s->peer, a->addr, BTPOLICY_ADDR_LEN);
	s->peer_stored = a->stored;
}

void btpolicy_dropping(struct btpolicy *p, enum btpolicy_kind kind)
{
	p->slots[kind].dropping = true;
}

void btpolicy_connected(struct btpolicy *p, enum btpolicy_kind kind)
{
	p->slots[kind].state = BTPOLICY_CONNECTED;
}

const uint8_t *btpolicy_peer(const struct btpolicy *p, enum btpolicy_kind kind)
{
	return p->slots[kind].state == BTPOLICY_IDLE ? NULL : p->slots[kind].peer;
}

static void refuse_peer(struct btpolicy *p, const uint8_t *peer)
{
	if (!refused(p, peer) && p->refused_count < BTPOLICY_STORED_MAX) {
		memcpy(p->refused[p->refused_count++], peer, BTPOLICY_ADDR_LEN);
	}
}

enum btpolicy_pairing btpolicy_paired(struct btpolicy *p, enum btpolicy_kind kind,
				      bool passkey_used)
{
	struct btpolicy_slot *s = &p->slots[kind];

	if (kind == BTPOLICY_KEYBOARD && p->window_open && p->passkey_demanded && !passkey_used) {
		refuse_peer(p, s->peer);
		return BTPOLICY_PAIRING_FORGOTTEN;
	}
	s->peer_stored = true;
	return BTPOLICY_PAIRING_KEPT;
}

void btpolicy_ready(struct btpolicy *p, enum btpolicy_kind kind)
{
	p->slots[kind].state = BTPOLICY_READY;
}

bool btpolicy_is_ready(const struct btpolicy *p, enum btpolicy_kind kind)
{
	return p->slots[kind].state == BTPOLICY_READY;
}

void btpolicy_disconnected(struct btpolicy *p, enum btpolicy_kind kind, uint32_t now_ms,
			   bool never_joined)
{
	struct btpolicy_slot *s = &p->slots[kind];
	bool failed = s->state != BTPOLICY_READY && s->peer_stored;
	bool quick = failed && never_joined && s->quick_retries < BTPOLICY_QUICK_RETRIES;

	s->quick_retries = quick ? s->quick_retries + 1 : 0;
	s->retry_pending = failed && !quick;
	s->retry_at_ms = now_ms + BTPOLICY_RETRY_MS;
	s->state = BTPOLICY_IDLE;
	s->dropping = false;
}

enum btpolicy_security btpolicy_security_failed(struct btpolicy *p, enum btpolicy_kind kind,
						enum btpolicy_failure why)
{
	struct btpolicy_slot *s = &p->slots[kind];

	if (s->peer_stored) {
		return why == BTPOLICY_FAILURE_KEY_MISSING ? BTPOLICY_BOND_LOST
							   : BTPOLICY_SECURITY_FAILED;
	}
	if (why != BTPOLICY_FAILURE_REFUSAL) {
		return BTPOLICY_SECURITY_FAILED;
	}
	if (refused(p, s->peer)) {
		return BTPOLICY_REFUSED_BEFORE;
	}
	refuse_peer(p, s->peer);
	return BTPOLICY_PAIRING_REFUSED;
}

void btpolicy_window(struct btpolicy *p, bool open, bool passkey_demanded)
{
	if (p->window_open && !open) {
		p->refused_count = 0;
	}
	p->window_open = open;
	p->passkey_demanded = open && passkey_demanded;
}

enum btpolicy_scan btpolicy_scan(const struct btpolicy *p)
{
	bool slot_free = false;

	if (setting_up(p)) {
		return BTPOLICY_SCAN_OFF;
	}
	if (p->window_open) {
		return BTPOLICY_SCAN_ACTIVE;
	}
	for (int k = 0; k < BTPOLICY_KINDS; k++) {
		slot_free |= p->slots[k].state == BTPOLICY_IDLE;
	}
	return slot_free ? BTPOLICY_SCAN_PASSIVE : BTPOLICY_SCAN_OFF;
}
