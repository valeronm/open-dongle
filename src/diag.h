#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "diagproto.h"

enum diag_after {
	DIAG_AFTER_NONE,
	DIAG_AFTER_REBOOT,
	DIAG_AFTER_BOOTLOADER,
};

/* A request arriving while the queue is full is dropped. */
void diag_on_request(const uint8_t *buf, size_t len);
bool diag_take_request(struct diag_request *out);

/*
 * Never blocks: a reply that does not fit in the free part slots is dropped whole. after runs once
 * the host has read the last part.
 */
void diag_reply(const struct diag_request *req, const uint8_t *payload, size_t len,
		enum diag_after after);
void diag_reply_error(const struct diag_request *req, enum diag_error code, uint8_t detail);

#endif
