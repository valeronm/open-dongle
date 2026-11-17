#ifndef BLE_H
#define BLE_H

#include <stdint.h>

/* Starts the Bluetooth stack and a passive scan; a second call fails with -EALREADY once the
 * first has run. */
int ble_start(void);
uint32_t ble_adverts_heard(void);

#endif
