#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include "diag.h"
#include "usb.h"

#define DIAG_PART_SLOTS 16

struct diag_part {
	uint8_t report[DIAG_REPORT_LEN];
	uint8_t after;
};

BUILD_ASSERT(DIAG_PART_SLOTS * DIAG_CHUNK >= DIAG_PAYLOAD_MAX);

K_MSGQ_DEFINE(diag_requests, sizeof(struct diag_request), 4, 4);
K_MSGQ_DEFINE(diag_parts, sizeof(struct diag_part), DIAG_PART_SLOTS, 4);

void diag_on_request(const uint8_t *buf, size_t len)
{
	struct diag_request req;

	if (diag_parse_request(buf, len, &req)) {
		k_msgq_put(&diag_requests, &req, K_NO_WAIT);
	}
}

bool diag_take_request(struct diag_request *out)
{
	return k_msgq_get(&diag_requests, out, K_NO_WAIT) == 0;
}

void diag_reply(const struct diag_request *req, const uint8_t *payload, size_t len,
		enum diag_after after)
{
	size_t count = diag_part_count(len);

	if (k_msgq_num_free_get(&diag_parts) < count) {
		return;
	}
	for (size_t i = 0; i < count; i++) {
		struct diag_part p = {.after = i + 1 == count ? after : DIAG_AFTER_NONE};

		diag_part(p.report, req, payload, len, i);
		k_msgq_put(&diag_parts, &p, K_NO_WAIT);
	}
}

void diag_reply_error(const struct diag_request *req, enum diag_error code)
{
	struct diag_part p = {.after = DIAG_AFTER_NONE};

	diag_error_report(p.report, req, code);
	k_msgq_put(&diag_parts, &p, K_NO_WAIT);
}

static void diag_thread(void *a, void *b, void *c)
{
	for (;;) {
		struct diag_part p;

		k_msgq_get(&diag_parts, &p, K_FOREVER);
		usb_diag_send(p.report);
		if (p.after == DIAG_AFTER_REBOOT) {
			sys_reboot(SYS_REBOOT_COLD);
		} else if (p.after == DIAG_AFTER_BOOTLOADER) {
			usb_reboot_to_bootloader();
		}
	}
}

K_THREAD_DEFINE(diag_tid, 1024, diag_thread, NULL, NULL, NULL, 7, 0, 0);
