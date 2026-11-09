#include <zephyr/settings/settings.h>
#include "store.h"

#define CHANNELS_KEY "dongle/channels"

int store_init(void)
{
	return settings_subsys_init();
}

bool store_load_channels(uint8_t out[3])
{
	return settings_load_one(CHANNELS_KEY, out, 3) == 3;
}

int store_save_channels(const uint8_t channels[3])
{
	return settings_save_one(CHANNELS_KEY, channels, 3);
}
