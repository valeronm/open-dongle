#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

/* The SoftDevice information structure's fields up to and including its version. */
#define BOOTINFO_SD_INFO_LEN 0x18

struct bootinfo {
	/* The RESET_* bits of Zephyr's hwinfo. */
	uint32_t reset_cause;
	/* All zero when no bootloader left its version. */
	uint8_t bootloader[3];
	/* 0 when no SoftDevice is in flash. */
	uint16_t softdevice_fwid;
	/* 0 when the SoftDevice's information structure is too short to hold it. */
	uint32_t softdevice_id;
	/* major * 1000000 + minor * 1000 + bugfix, or 0 like softdevice_id. */
	uint32_t softdevice_version;
};

/* version_register is TIMER2's CC[0] as the Adafruit bootloader leaves it; sd_info is the flash at
 * the SoftDevice's information structure. */
void bootinfo_read(struct bootinfo *out, uint32_t reset_cause, uint32_t version_register,
		   const uint8_t sd_info[BOOTINFO_SD_INFO_LEN]);

#endif
