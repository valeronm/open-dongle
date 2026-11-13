#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/printk.h>
#include "core.h"
#include "store.h"

int main(void)
{
	uint32_t reset_cause = 0;
	uint8_t stored = 0;
	bool loaded = false;
	enum protocol active;
	int store_err;

	hwinfo_get_reset_cause(&reset_cause);
	hwinfo_clear_reset_cause();
	store_err = store_init();
	if (store_err) {
		printk("store init failed: %d\n", store_err);
	} else {
		loaded = store_load_protocol(&stored);
	}
	active = protocol_from_stored(loaded, stored);
	core_start(active, reset_cause);
	if (active == PROTOCOL_UNIFYING) {
		unifying_run(store_err);
	}
	nuphy_run(store_err);
	return 0;
}
