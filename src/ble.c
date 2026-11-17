#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include "ble.h"

static atomic_t adverts;

static void on_advert(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		      struct net_buf_simple *data)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(rssi);
	ARG_UNUSED(type);
	ARG_UNUSED(data);
	atomic_inc(&adverts);
}

int ble_start(void)
{
	int err = bt_enable(NULL);

	if (!err) {
		err = settings_load();
	}
	if (!err) {
		err = bt_le_scan_start(BT_LE_SCAN_PARAM(BT_LE_SCAN_TYPE_PASSIVE, BT_LE_SCAN_OPT_NONE,
							BT_GAP_SCAN_FAST_INTERVAL,
							BT_GAP_SCAN_FAST_WINDOW),
				       on_advert);
	}
	return err;
}

uint32_t ble_adverts_heard(void)
{
	return (uint32_t)atomic_get(&adverts);
}
