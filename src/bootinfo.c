#include <string.h>
#include "bootinfo.h"

#define SD_INFO_SIZE 0x00
#define SD_MAGIC 0x04
#define SD_FWID 0x0c
#define SD_ID 0x10
#define SD_VERSION 0x14
#define SD_MAGIC_NUMBER 0x51b1e5dbu

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

void bootinfo_read(struct bootinfo *out, uint32_t reset_cause, uint32_t version_register,
		   const uint8_t sd_info[BOOTINFO_SD_INFO_LEN])
{
	memset(out, 0, sizeof(*out));
	out->reset_cause = reset_cause;
	/* The bootloader packs major, minor and patch into the low three bytes. */
	if ((version_register >> 24) == 0) {
		out->bootloader[0] = version_register >> 16;
		out->bootloader[1] = version_register >> 8;
		out->bootloader[2] = version_register;
	}
	if (le32(&sd_info[SD_MAGIC]) != SD_MAGIC_NUMBER) {
		return;
	}
	out->softdevice_fwid = sd_info[SD_FWID] | sd_info[SD_FWID + 1] << 8;
	/* A field lies inside the structure only where its size byte reaches past the field's
	 * offset. */
	if (sd_info[SD_INFO_SIZE] > SD_ID) {
		out->softdevice_id = le32(&sd_info[SD_ID]);
	}
	if (sd_info[SD_INFO_SIZE] > SD_VERSION) {
		out->softdevice_version = le32(&sd_info[SD_VERSION]);
	}
}
