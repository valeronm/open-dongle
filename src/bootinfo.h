#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

#include "diagwire.h"

/* The SoftDevice information structure's fields up to and including its version. */
#define BOOTINFO_SD_INFO_LEN 0x18

/* version_register is TIMER2's CC[0] as the Adafruit bootloader leaves it; sd_info is the flash at
 * the SoftDevice's information structure. */
void bootinfo_read(struct bootinfo *out, uint32_t reset_cause, uint32_t version_register,
		   const uint8_t sd_info[BOOTINFO_SD_INFO_LEN]);

#endif
