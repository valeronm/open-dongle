#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include "chanpick.h"
#include "store.h"

#define CHANNELS_KEY "dongle/channels"

int store_init(void)
{
	return settings_subsys_init();
}

bool store_load_channels(uint8_t out[3])
{
	if (settings_load_one(CHANNELS_KEY, out, 3) != 3) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		if (out[i] < CHAN_FIRST || out[i] > CHAN_LAST) {
			printk("stored channels out of range\n");
			return false;
		}
	}
	return true;
}

int store_save_channels(const uint8_t channels[3])
{
	return settings_save_one(CHANNELS_KEY, channels, 3);
}
