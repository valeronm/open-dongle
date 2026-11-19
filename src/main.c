#include <hal/nrf_timer.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include "bootinfo.h"
#include "core.h"
#include "store.h"

/* The SoftDevice sits above the 4 kB MBR and keeps its information structure 0x2000 into itself. */
#define SD_INFO_ADDRESS 0x3000

static uint32_t bootloader_version_register;

/* TIMER2's CC[0] holds the bootloader's version only until a driver or radio stack reuses the
 * timer, and PRE_KERNEL_1 runs before any of them start. */
static int capture_bootloader_version(void)
{
	bootloader_version_register = nrf_timer_cc_get(NRF_TIMER2, NRF_TIMER_CC_CHANNEL0);
	return 0;
}

SYS_INIT(capture_bootloader_version, PRE_KERNEL_1, 0);

int main(void)
{
	uint32_t reset_cause = 0;
	uint8_t stored = 0;
	bool loaded = false;
	enum protocol active;
	struct bootinfo boot;
	int store_err;

	hwinfo_get_reset_cause(&reset_cause);
	hwinfo_clear_reset_cause();
	bootinfo_read(&boot, reset_cause, bootloader_version_register,
		      (const uint8_t *)SD_INFO_ADDRESS);
	store_err = store_init();
	if (store_err) {
		printk("store init failed: %d\n", store_err);
	} else {
		loaded = store_load_protocol(&stored);
	}
	active = protocol_from_stored(loaded, stored);
	core_start(active, &boot);
	if (active == PROTOCOL_BLUETOOTH) {
		bluetooth_run(store_err);
	}
	if (active == PROTOCOL_UNIFYING) {
		unifying_run(store_err);
	}
	nuphy_run(store_err);
	return 0;
}
