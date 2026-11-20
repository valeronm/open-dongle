#ifndef BLE_H
#define BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "btpolicy.h"
#include "noise.h"

#define BLE_REPORT_MAX 64
#define BLE_MAP_MAX 1024
#define BLE_NAME_MAX 32
#define BLE_ADDRESS_MAX 30
#define BLE_REASON_NOT_ESTABLISHED 0x3e

enum ble_event_type {
	BLE_ADVERT,
	BLE_CONNECTED,
	BLE_DISCONNECTED,
	BLE_FAILED,
	BLE_PAIRED,
	BLE_MAP_READ,
	BLE_REPORT,
	BLE_BATTERY,
};

enum ble_stage {
	BLE_STAGE_CONNECT,
	BLE_STAGE_SECURITY,
	BLE_STAGE_DISCOVERY,
	BLE_STAGE_HID_SETUP,
	BLE_STAGE_MAP,
	BLE_STAGE_SUBSCRIBE,
};

struct ble_event {
	enum ble_event_type type;
	/* The slot the event is about; unset for BLE_ADVERT. */
	enum device_kind kind;
	union {
		/* BLE_ADVERT: from a stored device or one offering the HID service. */
		struct btpolicy_advert advert;
		/* BLE_CONNECTED: the device's address as text. */
		char address[BLE_ADDRESS_MAX];
		/* BLE_DISCONNECTED: the stack's reason code, BLE_REASON_NOT_ESTABLISHED when the
		 * device never joined the connection. */
		uint8_t reason;
		/* BLE_FAILED */
		struct {
			enum ble_stage stage;
			int error;
			enum btpolicy_failure why;
		} failed;
		/* BLE_PAIRED: the device was typed the offered passkey. */
		bool passkey_used;
		/* BLE_REPORT: the bytes exclude the report ID. */
		struct {
			uint32_t received_us;
			uint8_t id;
			uint8_t len;
			uint8_t data[BLE_REPORT_MAX];
		} report;
		/* BLE_BATTERY: the level is unknown once it cannot be read. */
		struct device_battery battery;
	};
};

/* Starts the Bluetooth stack; a second call fails with -EALREADY once the first has run. */
int ble_start(void);
/* Does nothing when scanning is already in the mode asked for; an active scan asks every
 * advertiser for its scan response. */
int ble_scan(enum btpolicy_scan mode);
/* Every call that returns 0 ends in one BLE_DISCONNECTED for the slot, after a BLE_FAILED when a
 * stage failed. */
int ble_connect(enum device_kind kind, const uint8_t addr[BTPOLICY_ADDR_LEN]);
/* While on, a keyboard able to take a passkey is asked for this six-digit one as it pairs. */
int ble_offer_passkey(bool on, uint32_t value);
uint8_t ble_stored_count(void);
/* Copies up to cap bonded addresses and returns how many bonds there are. */
uint8_t ble_stored(uint8_t addrs[][BTPOLICY_ADDR_LEN], uint8_t cap);
/* Deletes the device's bond, disconnecting it first if it is connected. */
int ble_forget(const uint8_t addr[BTPOLICY_ADDR_LEN]);
bool ble_take_event(struct ble_event *ev, k_timeout_t wait);
/* Copies the report map read at the slot's connection and returns its length, 0 before its
 * BLE_MAP_READ. */
size_t ble_copy_map(enum device_kind kind, uint8_t *out, size_t cap);
/* The name of the slot's device, empty if it gave none. */
void ble_copy_name(enum device_kind kind, char out[BLE_NAME_MAX + 1]);
void ble_disconnect(enum device_kind kind);
/* Writes the slot's output report with this ID, data excluding the ID; fails until the device's
 * reports are discovered and while an earlier write to that report is still being sent. */
int ble_write_report(enum device_kind kind, uint8_t report_id, const uint8_t *data, uint8_t len);
/* Reads the slot's signal from the controller; fails with -ENOTCONN without a device and -ENODATA
 * when the controller has no reading yet. */
int ble_rssi(enum device_kind kind, int8_t *dbm);
/* Whether the bond with the device was made by LE Secure Connections, not legacy pairing; false
 * for an address without a bond. */
bool ble_bond_secure(const uint8_t addr[BTPOLICY_ADDR_LEN]);
const char *ble_stage_name(enum ble_stage stage);
/* Takes the energy read on the data channels since the last call; it always counts toward the
 * minute, since the links hop over every data channel. */
bool ble_take_noise(struct noise_tally *t);

#endif
