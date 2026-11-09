#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>

/* Console output written before the host opens the port is dropped. */
static void wait_for_host(void)
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	for (int i = 0; i < 100 && !dtr; i++) {
		uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}
}

int main(void)
{
	uint32_t raw = NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
	uint8_t id[4];

	memcpy(id, &raw, sizeof(id));
	wait_for_host();
	for (;;) {
		printk("nuphy-dongle id %02x%02x%02x%02x up %u s\n", id[0], id[1], id[2], id[3],
		       k_uptime_get_32() / 1000);
		k_sleep(K_SECONDS(5));
	}
}
