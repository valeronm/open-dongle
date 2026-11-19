#include "test.h"
#include "bootinfo.h"

/* The info block of S140 6.1.1 as the Adafruit release image carries it. */
static const uint8_t s140_6_1_1[BOOTINFO_SD_INFO_LEN] = {
	0x2c, 0xff, 0xff, 0xff, 0xdb, 0xe5, 0xb1, 0x51, 0x00, 0x60, 0x02, 0x00,
	0xb6, 0x00, 0xff, 0xff, 0x8c, 0x00, 0x00, 0x00, 0x69, 0x91, 0x5b, 0x00,
};

static const uint8_t erased[BOOTINFO_SD_INFO_LEN] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void test_the_register_gives_the_bootloader_version(void)
{
	struct bootinfo b;
	const uint8_t v0_6_0[3] = {0, 6, 0};
	const uint8_t v0_11_0[3] = {0, 11, 0};

	bootinfo_read(&b, 0, 0x000600, erased);
	CHECK_MEM(b.bootloader, v0_6_0, 3);
	bootinfo_read(&b, 0, 0x000b00, erased);
	CHECK_MEM(b.bootloader, v0_11_0, 3);
}

static void test_a_register_no_bootloader_wrote_reads_as_unknown(void)
{
	struct bootinfo b;
	const uint8_t unknown[3] = {0};

	bootinfo_read(&b, 0, 0, erased);
	CHECK_MEM(b.bootloader, unknown, 3);
	bootinfo_read(&b, 0, 0x01000600, erased);
	CHECK_MEM(b.bootloader, unknown, 3);
}

static void test_the_reset_cause_is_kept_as_given(void)
{
	struct bootinfo b;

	bootinfo_read(&b, 0x11, 0x000600, s140_6_1_1);
	CHECK_EQ(b.reset_cause, 0x11);
}

static void test_the_info_block_gives_the_softdevice(void)
{
	struct bootinfo b;

	bootinfo_read(&b, 0, 0x000600, s140_6_1_1);
	CHECK_EQ(b.softdevice_fwid, 0x00b6);
	CHECK_EQ(b.softdevice_id, 140);
	CHECK_EQ(b.softdevice_version, 6001001);
}

static void test_flash_without_the_magic_has_no_softdevice(void)
{
	struct bootinfo b;
	uint8_t info[BOOTINFO_SD_INFO_LEN];

	bootinfo_read(&b, 0, 0x000600, erased);
	CHECK_EQ(b.softdevice_fwid, 0);
	CHECK_EQ(b.softdevice_id, 0);
	CHECK_EQ(b.softdevice_version, 0);
	memcpy(info, s140_6_1_1, sizeof(info));
	info[4] = 0;
	bootinfo_read(&b, 0, 0x000600, info);
	CHECK_EQ(b.softdevice_fwid, 0);
	CHECK_EQ(b.softdevice_id, 0);
	CHECK_EQ(b.softdevice_version, 0);
}

static void test_a_short_info_block_gives_only_the_fwid(void)
{
	struct bootinfo b;
	uint8_t info[BOOTINFO_SD_INFO_LEN];

	memcpy(info, s140_6_1_1, sizeof(info));
	info[0] = 0x10;
	bootinfo_read(&b, 0, 0x000600, info);
	CHECK_EQ(b.softdevice_fwid, 0x00b6);
	CHECK_EQ(b.softdevice_id, 0);
	CHECK_EQ(b.softdevice_version, 0);
	info[0] = 0x14;
	bootinfo_read(&b, 0, 0x000600, info);
	CHECK_EQ(b.softdevice_id, 140);
	CHECK_EQ(b.softdevice_version, 0);
}

int main(void)
{
	RUN(test_the_register_gives_the_bootloader_version);
	RUN(test_a_register_no_bootloader_wrote_reads_as_unknown);
	RUN(test_the_reset_cause_is_kept_as_given);
	RUN(test_the_info_block_gives_the_softdevice);
	RUN(test_flash_without_the_magic_has_no_softdevice);
	RUN(test_a_short_info_block_gives_only_the_fwid);
	return TEST_EXIT();
}
