#ifndef CORE_H
#define CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include "diagproto.h"
#include "indicator.h"
#include "protocol.h"
#include "radio.h"
#include "reports.h"

struct rx_item {
	uint8_t len;
	uint8_t pipe;
	int8_t rssi;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

extern const uint8_t core_firmware_version[3];

/* Starts USB, the report senders and the watchdog for the protocol this boot runs. */
void core_start(enum protocol active, uint32_t reset_cause);
uint32_t core_reset_cause(void);
void core_on_radio_rx(const uint8_t *data, size_t len, uint8_t pipe, int8_t rssi);
/* Blocks until the radio starts, retrying each second. */
void core_start_radio(const struct radio_config *rc);
bool core_take_rx(struct rx_item *out, k_timeout_t wait);
uint32_t core_rx_dropped(void);
/* Returns true when the report changed what the host sees; a change wakes a suspended host. */
bool core_submit(const struct report *r);
void core_release_keys(void);
uint32_t core_usb_retries(void);
uint32_t core_merges(void);
void core_clear_counters(void);
/* Returns true once after each host LED change; *leds always receives the current LEDs. */
bool core_take_leds(uint8_t *leds);
void core_watchdog_feed(void);
/* A suspended host shows IND_SLEEP whatever the mode. */
void core_show_state(enum indicator_mode mode);
/* Sets the fields every protocol reports and zeroes the rest. */
void core_fill_status(struct diag_status *st);
/* Answers protocol, reboot and bootloader requests, and requests for the other protocol's commands,
 * itself; returns true with the next request it does not answer. */
bool core_take_request(struct diag_request *out);

FUNC_NORETURN void nuphy_run(int store_err);
FUNC_NORETURN void unifying_run(int store_err);

#endif
