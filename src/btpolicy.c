#include <string.h>

#include "btpolicy.h"

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

bool btpolicy_interesting(const struct btpolicy_advert *a)
{
	return a->connectable && (a->stored || a->hid_service);
}

bool btpolicy_wants(const struct btpolicy *p, const struct btpolicy_advert *a, bool window_open,
		    uint8_t stored_count, uint32_t now_ms)
{
	if (p->state != BTPOLICY_IDLE || !btpolicy_interesting(a)) {
		return false;
	}
	if (a->stored) {
		return !window_open &&
		       (!p->retry_pending || (int32_t)(now_ms - p->retry_at_ms) >= 0);
	}
	return window_open && !a->directed && stored_count < BTPOLICY_STORED_MAX &&
	       !refused(p, a->addr);
}

void btpolicy_connecting(struct btpolicy *p, const struct btpolicy_advert *a)
{
	p->state = BTPOLICY_CONNECTING;
	memcpy(p->peer, a->addr, BTPOLICY_ADDR_LEN);
	p->peer_stored = a->stored;
}

void btpolicy_connected(struct btpolicy *p)
{
	p->state = BTPOLICY_CONNECTED;
}

const uint8_t *btpolicy_peer(const struct btpolicy *p)
{
	return p->state == BTPOLICY_IDLE ? NULL : p->peer;
}

static void refuse_peer(struct btpolicy *p)
{
	if (!refused(p, p->peer) && p->refused_count < BTPOLICY_STORED_MAX) {
		memcpy(p->refused[p->refused_count++], p->peer, BTPOLICY_ADDR_LEN);
	}
}

enum btpolicy_pairing btpolicy_paired(struct btpolicy *p, bool passkey_used)
{
	if (p->window_open && p->passkey_demanded && !passkey_used) {
		refuse_peer(p);
		return BTPOLICY_PAIRING_FORGOTTEN;
	}
	p->peer_stored = true;
	return BTPOLICY_PAIRING_KEPT;
}

void btpolicy_ready(struct btpolicy *p)
{
	p->state = BTPOLICY_READY;
}

bool btpolicy_is_ready(const struct btpolicy *p)
{
	return p->state == BTPOLICY_READY;
}

void btpolicy_disconnected(struct btpolicy *p, uint32_t now_ms, bool never_joined)
{
	bool failed = p->state != BTPOLICY_READY && p->peer_stored;
	bool quick = failed && never_joined && p->quick_retries < BTPOLICY_QUICK_RETRIES;

	p->quick_retries = quick ? p->quick_retries + 1 : 0;
	p->retry_pending = failed && !quick;
	p->retry_at_ms = now_ms + BTPOLICY_RETRY_MS;
	p->state = BTPOLICY_IDLE;
}

enum btpolicy_security btpolicy_security_failed(struct btpolicy *p, enum btpolicy_failure why)
{
	if (p->peer_stored) {
		return why == BTPOLICY_FAILURE_KEY_MISSING ? BTPOLICY_BOND_LOST
							   : BTPOLICY_SECURITY_FAILED;
	}
	if (why != BTPOLICY_FAILURE_REFUSAL) {
		return BTPOLICY_SECURITY_FAILED;
	}
	if (refused(p, p->peer)) {
		return BTPOLICY_REFUSED_BEFORE;
	}
	refuse_peer(p);
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

bool btpolicy_scanning(const struct btpolicy *p)
{
	return p->state == BTPOLICY_IDLE;
}
