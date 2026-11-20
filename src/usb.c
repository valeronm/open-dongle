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
#define EXTRA_ID_KEYS 3
#define EXTRA_USAGE_REPORT_LEN 3
#define EXTRA_REPORT_LEN (1 + REPORT_NKRO_LEN)
#define ALL_IFACES_READY BIT_MASK(REPORT_IFACES)

USBD_DEVICE_DEFINE(dongle_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), USB_VID, USB_PID);
USBD_DESC_LANG_DEFINE(dongle_lang);
USBD_DESC_MANUFACTURER_DEFINE(dongle_mfr, "OpenHardware");
USBD_DESC_PRODUCT_DEFINE(dongle_product, CONFIG_BT_DEVICE_NAME);
USBD_DESC_SERIAL_NUMBER_DEFINE(dongle_sn);
USBD_DESC_CONFIG_DEFINE(dongle_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(dongle_config, USB_SCD_REMOTE_WAKEUP, 50, &dongle_cfg_desc);

/* The Zephyr macro's key array stops at logical and usage maximum 101, which hosts enforce. */
static const uint8_t keyboard_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_KEYBOARD),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP_KEYPAD),
	HID_USAGE_MIN8(0xe0),
	HID_USAGE_MAX8(0xe7),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX8(1),
	HID_REPORT_SIZE(1),
	HID_REPORT_COUNT(8),
	HID_INPUT(0x02),
	HID_REPORT_SIZE(8),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x03),
	HID_REPORT_SIZE(1),
	HID_REPORT_COUNT(5),
	HID_USAGE_PAGE(HID_USAGE_GEN_LEDS),
	HID_USAGE_MIN8(1),
	HID_USAGE_MAX8(5),
	HID_OUTPUT(0x02),
	HID_REPORT_SIZE(3),
	HID_REPORT_COUNT(1),
	HID_OUTPUT(0x03),
	HID_REPORT_SIZE(8),
	HID_REPORT_COUNT(6),
	HID_LOGICAL_MIN8(0),
	/* A one-byte 0xff is -1 as a logical maximum. */
	HID_LOGICAL_MAX16(0xff, 0x00),
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP_KEYPAD),
	HID_USAGE_MIN8(0),
	HID_USAGE_MAX8(0xff),
	HID_INPUT(0x00),
	HID_END_COLLECTION,
};

/* The keyboard sends QMK's extra-keys usage codes unchanged. */
static const uint8_t extra_desc[] = {
	HID_USAGE_PAGE(0x0c), /* Consumer */
	HID_USAGE(0x01),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
	HID_REPORT_ID(EXTRA_ID_CONSUMER),
	HID_LOGICAL_MIN8(0x01),
	HID_LOGICAL_MAX16(REPORT_CONSUMER_MAX & 0xff, REPORT_CONSUMER_MAX >> 8),
	HID_USAGE_MIN8(0x01),
	HID_USAGE_MAX16(REPORT_CONSUMER_MAX & 0xff, REPORT_CONSUMER_MAX >> 8),
	HID_REPORT_SIZE(16),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x00),
	HID_END_COLLECTION,
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(0x80), /* System Control */
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
	HID_REPORT_ID(EXTRA_ID_SYSTEM),
	/*
	 * Generic Desktop usages outside power, sleep and wake include joystick and D-pad ones, which
	 * make a host treat the interface as a joystick.
	 */
	HID_LOGICAL_MIN16(REPORT_SYSTEM_MIN, 0x00), /* One-byte 0x81 and 0x83 would be negative. */
	HID_LOGICAL_MAX16(REPORT_SYSTEM_MAX, 0x00),
	HID_USAGE_MIN8(REPORT_SYSTEM_MIN),
	HID_USAGE_MAX8(REPORT_SYSTEM_MAX),
	HID_REPORT_SIZE(16),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x00),
	HID_END_COLLECTION,
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_KEYBOARD),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
	HID_REPORT_ID(EXTRA_ID_KEYS),
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP_KEYPAD),
	HID_USAGE_MIN8(0),
	HID_USAGE_MAX8(REPORT_NKRO_KEYS - 1),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX8(1),
	HID_REPORT_SIZE(1),
	HID_REPORT_COUNT(REPORT_NKRO_KEYS),
	HID_INPUT(0x02),
	HID_END_COLLECTION,
};

#define HID_PHYSICAL_MIN8(a) HID_ITEM(HID_ITEM_TAG_PHYSICAL_MIN, HID_ITEM_TYPE_GLOBAL, 1), a
#define HID_PHYSICAL_MAX8(a) HID_ITEM(HID_ITEM_TAG_PHYSICAL_MAX, HID_ITEM_TYPE_GLOBAL, 1), a
#define HID_USAGE_GEN_DESKTOP_RESOLUTION_MULTIPLIER 0x48

/* A host sets the multiplier to 1 to read the wheel in REPORT_WHEEL_HIRES_STEPS per notch; the
 * multiplier applies within its logical collection. */
static const uint8_t mouse_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_MOUSE),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_POINTER),
	HID_COLLECTION(HID_COLLECTION_PHYSICAL),
	HID_USAGE_PAGE(HID_USAGE_GEN_BUTTON),
	HID_USAGE_MIN8(1),
	HID_USAGE_MAX8(16),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX8(1),
	HID_REPORT_SIZE(1),
	HID_REPORT_COUNT(16),
	HID_INPUT(0x02),
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_X),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_Y),
	HID_LOGICAL_MIN16(0x01, 0x80),
	HID_LOGICAL_MAX16(0xff, 0x7f),
	HID_REPORT_SIZE(16),
	HID_REPORT_COUNT(2),
	HID_INPUT(0x06),
	HID_COLLECTION(HID_COLLECTION_LOGICAL),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_RESOLUTION_MULTIPLIER),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX8(1),
	HID_PHYSICAL_MIN8(1),
	HID_PHYSICAL_MAX8(REPORT_WHEEL_HIRES_STEPS),
	HID_REPORT_SIZE(2),
	HID_REPORT_COUNT(1),
	HID_FEATURE(0x02),
	HID_PHYSICAL_MIN8(0),
	HID_PHYSICAL_MAX8(0),
	HID_REPORT_SIZE(6),
	HID_FEATURE(0x03),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_WHEEL),
	HID_LOGICAL_MIN8(-127),
	HID_LOGICAL_MAX8(127),
	HID_REPORT_SIZE(8),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x06),
	HID_END_COLLECTION,
	HID_USAGE_PAGE(0x0c), /* Consumer */
	HID_USAGE16(0x0238), /* AC Pan */
	HID_INPUT(0x06),
	HID_END_COLLECTION,
	HID_END_COLLECTION,
};

static const uint8_t diag_desc[] = {
	HID_USAGE_PAGE16(0xff00),
	HID_USAGE(0x01),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX16(0xff, 0x00),
	HID_REPORT_SIZE(8),
	HID_REPORT_COUNT(DIAG_REPORT_LEN),
	HID_USAGE(0x02),
	HID_INPUT(0x02),
	HID_REPORT_COUNT(DIAG_REPORT_LEN),
	HID_USAGE(0x03),
	HID_OUTPUT(0x02),
	HID_END_COLLECTION,
};

static const struct device *const keyboard_dev = DEVICE_DT_GET(DT_NODELABEL(hid_keyboard));
static const struct device *const extra_dev = DEVICE_DT_GET(DT_NODELABEL(hid_extra));
static const struct device *const mouse_dev = DEVICE_DT_GET(DT_NODELABEL(hid_mouse));
static const struct device *const diag_dev = DEVICE_DT_GET(DT_NODELABEL(hid_diag));
UDC_STATIC_BUF_DEFINE(diag_buf, DIAG_REPORT_LEN);
UDC_STATIC_BUF_DEFINE(keyboard_buf, REPORT_KEYBOARD_LEN);
UDC_STATIC_BUF_DEFINE(extra_buf, EXTRA_REPORT_LEN);
UDC_STATIC_BUF_DEFINE(mouse_buf, REPORT_MOUSE_LEN);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(hid_keyboard), in_report_size) == REPORT_KEYBOARD_LEN);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(hid_extra), in_report_size) == EXTRA_REPORT_LEN);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(hid_mouse), in_report_size) == REPORT_MOUSE_LEN);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(hid_diag), in_report_size) == DIAG_REPORT_LEN);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(hid_diag), out_report_size) == DIAG_REPORT_LEN);

static const struct usb_callbacks *callbacks;
static atomic_t ready_mask;
static uint32_t idle_duration;
static atomic_t mouse_boot;
static atomic_t mouse_hires;

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

static void mouse_ready(const struct device *dev, const bool ready)
{
	/* A host starts each configuration in report protocol. */
	if (!ready) {
		atomic_set(&mouse_boot, false);
		atomic_set(&mouse_hires, false);
	}
	mark_ready(REPORT_IFACE_MOUSE, ready);
}

uint8_t usb_mouse_wheel_steps(void)
{
	return atomic_get(&mouse_hires) ? REPORT_WHEEL_HIRES_STEPS : 1;
}

/* The feature report is the multiplier's index in its two low bits, without a report ID. */
static int mouse_get_report(const struct device *dev, const uint8_t type, const uint8_t id,
			    const uint16_t len, uint8_t *const buf)
{
	if (type != HID_REPORT_TYPE_FEATURE || len < 1) {
		return -ENOTSUP;
	}
	buf[0] = atomic_get(&mouse_hires) ? 1 : 0;
	return 1;
}

static int mouse_set_report(const struct device *dev, const uint8_t type, const uint8_t id,
			    const uint16_t len, const uint8_t *const buf)
{
	if (type != HID_REPORT_TYPE_FEATURE || len < 1) {
		return -ENOTSUP;
	}
	atomic_set(&mouse_hires, (buf[0] & 0x03) != 0);
	return 0;
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

static void mouse_set_protocol(const struct device *dev, const uint8_t proto)
{
	atomic_set(&mouse_boot, proto == HID_PROTOCOL_BOOT);
}

static const struct hid_device_ops mouse_ops = {
	.iface_ready = mouse_ready,
	.get_report = mouse_get_report,
	.set_report = mouse_set_report,
	.set_idle = set_idle,
	.get_idle = get_idle,
	.set_protocol = mouse_set_protocol,
};

static const struct hid_device_ops extra_ops = {
	.iface_ready = extra_ready,
	.get_report = no_get_report,
	.set_idle = set_idle,
	.get_idle = get_idle,
};

static int no_set_report(const struct device *dev, const uint8_t type, const uint8_t id,
			 const uint16_t len, const uint8_t *const buf)
{
	return -ENOTSUP;
}

static void diag_output(const struct device *dev, const uint16_t len, const uint8_t *const buf)
{
	callbacks->on_diag_request(buf, len);
}

static const struct hid_device_ops diag_ops = {
	.get_report = no_get_report,
	.set_report = no_set_report,
	.output_report = diag_output,
};

void usb_reboot_to_bootloader(void)
{
	nrf_power_gpregret_set(NRF_POWER, 0, BOOTLOADER_DFU_MAGIC);
	sys_reboot(SYS_REBOOT_COLD);
}

static void enter_bootloader_on_touch(const struct device *dev)
{
	uint32_t baud = 0;

	if (uart_line_ctrl_get(dev, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 && baud == 1200) {
		usb_reboot_to_bootloader();
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
	err = hid_device_register(mouse_dev, mouse_desc, sizeof(mouse_desc), &mouse_ops);
	if (err) {
		return err;
	}
	err = hid_device_register(diag_dev, diag_desc, sizeof(diag_desc), &diag_ops);
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

int usb_send(const struct report_usb *r)
{
	switch (r->kind) {
	case REPORT_USB_BOOT:
		memcpy(keyboard_buf, r->boot, REPORT_KEYBOARD_LEN);
		return hid_device_submit_report(keyboard_dev, REPORT_KEYBOARD_LEN, keyboard_buf);
	case REPORT_USB_MOUSE: {
		size_t len = reports_encode_mouse(&r->mouse, atomic_get(&mouse_boot), mouse_buf);

		return hid_device_submit_report(mouse_dev, len, mouse_buf);
	}
	case REPORT_USB_NKRO:
		extra_buf[0] = EXTRA_ID_KEYS;
		memcpy(&extra_buf[1], r->nkro, REPORT_NKRO_LEN);
		return hid_device_submit_report(extra_dev, EXTRA_REPORT_LEN, extra_buf);
	default:
		extra_buf[0] = r->kind == REPORT_USB_CONSUMER ? EXTRA_ID_CONSUMER : EXTRA_ID_SYSTEM;
		extra_buf[1] = r->usage & 0xff;
		extra_buf[2] = r->usage >> 8;
		return hid_device_submit_report(extra_dev, EXTRA_USAGE_REPORT_LEN, extra_buf);
	}
}

int usb_wakeup(void)
{
	return usbd_wakeup_request(&dongle_usbd);
}

int usb_diag_send(const uint8_t report[DIAG_REPORT_LEN])
{
	memcpy(diag_buf, report, DIAG_REPORT_LEN);
	return hid_device_submit_report(diag_dev, DIAG_REPORT_LEN, diag_buf);
}
