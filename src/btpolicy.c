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
		return !p->retry_pending || (int32_t)(now_ms - p->retry_at_ms) >= 0;
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

void btpolicy_paired(struct btpolicy *p)
{
	p->peer_stored = true;
}

void btpolicy_ready(struct btpolicy *p)
{
	p->state = BTPOLICY_READY;
}

bool btpolicy_is_ready(const struct btpolicy *p)
{
	return p->state == BTPOLICY_READY;
}

void btpolicy_disconnected(struct btpolicy *p, uint32_t now_ms)
{
	p->retry_pending = p->state != BTPOLICY_READY && p->peer_stored;
	p->retry_at_ms = now_ms + BTPOLICY_RETRY_MS;
	p->state = BTPOLICY_IDLE;
}

enum btpolicy_security btpolicy_security_failed(struct btpolicy *p, bool key_missing)
{
	if (p->peer_stored) {
		return key_missing ? BTPOLICY_BOND_LOST : BTPOLICY_SECURITY_FAILED;
	}
	if (refused(p, p->peer)) {
		return BTPOLICY_REFUSED_BEFORE;
	}
	if (p->refused_count < BTPOLICY_STORED_MAX) {
		memcpy(p->refused[p->refused_count++], p->peer, BTPOLICY_ADDR_LEN);
	}
	return BTPOLICY_PAIRING_REFUSED;
}

void btpolicy_window(struct btpolicy *p, bool open)
{
	if (p->window_open && !open) {
		p->refused_count = 0;
	}
	p->window_open = open;
}

bool btpolicy_scanning(const struct btpolicy *p)
{
	return p->state == BTPOLICY_IDLE;
}
