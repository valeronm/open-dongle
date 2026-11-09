#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/usbd.h>
#include <hal/nrf_power.h>
#include "usb.h"

#define USB_VID 0x2fe3
#define USB_PID 0x4e44
/* The Adafruit nRF52 bootloader stays in UF2 and serial DFU mode when it finds this value. */
#define BOOTLOADER_DFU_MAGIC 0x57
#define EXTRA_ID_CONSUMER 1
#define EXTRA_ID_SYSTEM 2
#define EXTRA_REPORT_LEN 3
#define ALL_IFACES_READY BIT_MASK(REPORT_IFACES)

USBD_DEVICE_DEFINE(dongle_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), USB_VID, USB_PID);
USBD_DESC_LANG_DEFINE(dongle_lang);
USBD_DESC_MANUFACTURER_DEFINE(dongle_mfr, "nuphy-dongle");
USBD_DESC_PRODUCT_DEFINE(dongle_product, "nuphy-dongle");
USBD_DESC_SERIAL_NUMBER_DEFINE(dongle_sn);
USBD_DESC_CONFIG_DEFINE(dongle_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(dongle_config, USB_SCD_REMOTE_WAKEUP, 50, &dongle_cfg_desc);

static const uint8_t keyboard_desc[] = HID_KEYBOARD_REPORT_DESC();

/* Same usage ranges as QMK's extra-keys reports, which the keyboard sends unchanged. */
static const uint8_t extra_desc[] = {
	HID_USAGE_PAGE(0x0c), /* Consumer */
	HID_USAGE(0x01),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
	HID_REPORT_ID(EXTRA_ID_CONSUMER),
	HID_LOGICAL_MIN8(0x01),
	HID_LOGICAL_MAX16(0xa0, 0x02),
	HID_USAGE_MIN8(0x01),
	HID_USAGE_MAX16(0xa0, 0x02),
	HID_REPORT_SIZE(16),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x00),
	HID_END_COLLECTION,
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(0x80), /* System Control */
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
	HID_REPORT_ID(EXTRA_ID_SYSTEM),
	HID_LOGICAL_MIN8(0x01),
	HID_LOGICAL_MAX16(0xb7, 0x00),
	HID_USAGE_MIN8(0x01),
	HID_USAGE_MAX16(0xb7, 0x00),
	HID_REPORT_SIZE(16),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x00),
	HID_END_COLLECTION,
};

static const struct device *const keyboard_dev = DEVICE_DT_GET(DT_NODELABEL(hid_keyboard));
static const struct device *const extra_dev = DEVICE_DT_GET(DT_NODELABEL(hid_extra));
UDC_STATIC_BUF_DEFINE(keyboard_buf, PROTO_KEYBOARD_LEN);
UDC_STATIC_BUF_DEFINE(extra_buf, EXTRA_REPORT_LEN);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(hid_keyboard), in_report_size) == PROTO_KEYBOARD_LEN);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(hid_extra), in_report_size) == EXTRA_REPORT_LEN);

static const struct usb_callbacks *callbacks;
static atomic_t ready_mask;
static uint32_t idle_duration;

bool usb_live(void)
{
	return atomic_get(&ready_mask) == ALL_IFACES_READY && !usbd_is_suspended(&dongle_usbd);
}

bool usb_suspended(void)
{
	return usbd_is_suspended(&dongle_usbd);
}

static void mark_ready(enum report_iface iface, bool ready)
{
	if (ready) {
		atomic_or(&ready_mask, BIT(iface));
	} else {
		atomic_and(&ready_mask, ~BIT(iface));
	}
	callbacks->on_live();
}

static void keyboard_ready(const struct device *dev, const bool ready)
{
	mark_ready(REPORT_IFACE_KEYBOARD, ready);
}

static void extra_ready(const struct device *dev, const bool ready)
{
	mark_ready(REPORT_IFACE_EXTRA, ready);
}

static int no_get_report(const struct device *dev, const uint8_t type, const uint8_t id,
			 const uint16_t len, uint8_t *const buf)
{
	return -ENOTSUP;
}

static int keyboard_set_report(const struct device *dev, const uint8_t type, const uint8_t id,
			       const uint16_t len, const uint8_t *const buf)
{
	if (type != HID_REPORT_TYPE_OUTPUT || len < 1) {
		return -ENOTSUP;
	}
	callbacks->on_leds(buf[0]);
	return 0;
}

static void set_idle(const struct device *dev, const uint8_t id, const uint32_t duration)
{
	idle_duration = duration;
}

static uint32_t get_idle(const struct device *dev, const uint8_t id)
{
	return idle_duration;
}

static void set_protocol(const struct device *dev, const uint8_t proto)
{
}

static const struct hid_device_ops keyboard_ops = {
	.iface_ready = keyboard_ready,
	.get_report = no_get_report,
	.set_report = keyboard_set_report,
	.set_idle = set_idle,
	.get_idle = get_idle,
	.set_protocol = set_protocol,
};

static const struct hid_device_ops extra_ops = {
	.iface_ready = extra_ready,
	.get_report = no_get_report,
	.set_idle = set_idle,
	.get_idle = get_idle,
};

static void enter_bootloader_on_touch(const struct device *dev)
{
	uint32_t baud = 0;

	if (uart_line_ctrl_get(dev, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 && baud == 1200) {
		nrf_power_gpregret_set(NRF_POWER, 0, BOOTLOADER_DFU_MAGIC);
		sys_reboot(SYS_REBOOT_COLD);
	}
}

static void on_usbd_msg(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	switch (msg->type) {
	case USBD_MSG_RESET:
	case USBD_MSG_SUSPEND:
	case USBD_MSG_RESUME:
		callbacks->on_live();
		break;
	case USBD_MSG_CDC_ACM_LINE_CODING:
		enter_bootloader_on_touch(msg->dev);
		break;
	default:
		break;
	}
}

int usb_start(const struct usb_callbacks *cb)
{
	struct usbd_desc_node *const descs[] = {&dongle_lang, &dongle_mfr, &dongle_product,
						&dongle_sn};
	int err;

	callbacks = cb;
	err = hid_device_register(keyboard_dev, keyboard_desc, sizeof(keyboard_desc),
				  &keyboard_ops);
	if (err) {
		return err;
	}
	err = hid_device_register(extra_dev, extra_desc, sizeof(extra_desc), &extra_ops);
	if (err) {
		return err;
	}
	for (size_t i = 0; i < ARRAY_SIZE(descs); i++) {
		err = usbd_add_descriptor(&dongle_usbd, descs[i]);
		if (err) {
			return err;
		}
	}
	err = usbd_add_configuration(&dongle_usbd, USBD_SPEED_FS, &dongle_config);
	if (err) {
		return err;
	}
	err = usbd_register_all_classes(&dongle_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		return err;
	}
	/* The CDC ACM console uses an interface association, which needs this class code. */
	usbd_device_set_code_triple(&dongle_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02,
				    0x01);
	err = usbd_msg_register_cb(&dongle_usbd, on_usbd_msg);
	if (err) {
		return err;
	}
	err = usbd_init(&dongle_usbd);
	if (err) {
		return err;
	}
	/* A reset with the cable already in raises no VBUS event. */
	return usbd_enable(&dongle_usbd);
}

int usb_send(const struct report *r)
{
	if (r->kind == REPORT_KEYBOARD) {
		memcpy(keyboard_buf, r->keyboard, PROTO_KEYBOARD_LEN);
		return hid_device_submit_report(keyboard_dev, PROTO_KEYBOARD_LEN, keyboard_buf);
	}
	extra_buf[0] = r->kind == REPORT_CONSUMER ? EXTRA_ID_CONSUMER : EXTRA_ID_SYSTEM;
	extra_buf[1] = r->usage & 0xff;
	extra_buf[2] = r->usage >> 8;
	return hid_device_submit_report(extra_dev, EXTRA_REPORT_LEN, extra_buf);
}

int usb_wakeup(void)
{
	return usbd_wakeup_request(&dongle_usbd);
}
