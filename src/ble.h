#ifndef BLE_H
#define BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_REPORT_MAX 64
#define BLE_MAP_MAX 1024
#define BLE_NAME_MAX 32
#define BLE_ADDRESS_MAX 30

enum ble_event_type {
	BLE_CONNECTED,
	BLE_DISCONNECTED,
	BLE_FAILED,
	BLE_MAP_READ,
	BLE_REPORT,
};

enum ble_stage {
	BLE_STAGE_SCAN,
	BLE_STAGE_CONNECT,
	BLE_STAGE_SECURITY,
	BLE_STAGE_DISCOVERY,
	BLE_STAGE_HID_SETUP,
	BLE_STAGE_MAP,
	BLE_STAGE_SUBSCRIBE,
};

struct ble_event {
	enum ble_event_type type;
	union {
		/* BLE_CONNECTED: the keyboard's address as text. */
		char address[BLE_ADDRESS_MAX];
		/* BLE_DISCONNECTED: the stack's reason code. */
		uint8_t reason;
		/* BLE_FAILED */
		struct {
			enum ble_stage stage;
			int error;
		} failed;
		/* BLE_REPORT: the bytes exclude the report ID. */
		struct {
			uint8_t id;
			uint8_t len;
			uint8_t data[BLE_REPORT_MAX];
		} report;
	};
};

/* Starts the Bluetooth stack and scans for HID keyboards; a second call fails with -EALREADY
 * once the first has run. */
int ble_start(void);
bool ble_take_event(struct ble_event *ev);
/* Valid from BLE_MAP_READ until the next BLE_CONNECTED. */
size_t ble_report_map(const uint8_t **map);
/* The connected keyboard's name, empty if it gave none; valid as the report map is. */
const char *ble_name(void);
const char *ble_stage_name(enum ble_stage stage);

#endif
