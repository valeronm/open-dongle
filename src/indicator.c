#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include "indicator.h"

#define STEP_MS 50

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static atomic_t mode = ATOMIC_INIT(IND_IDLE);

void indicator_set(enum indicator_mode m)
{
	atomic_set(&mode, m);
}

static bool lit(enum indicator_mode m, uint32_t step)
{
	switch (m) {
	case IND_IDLE:
		return step % 20 == 0;
	case IND_PAIRING:
		return step % 4 < 2;
	case IND_CONNECTED:
		return true;
	case IND_SLEEP:
		return false;
	case IND_ERROR:
		return step % 40 < 6 && step % 2 == 0;
	}
	return false;
}

static void indicator_thread(void *a, void *b, void *c)
{
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	for (uint32_t step = 0;; step++) {
		gpio_pin_set_dt(&led, lit(atomic_get(&mode), step));
		k_msleep(STEP_MS);
	}
}

K_THREAD_DEFINE(indicator_tid, 512, indicator_thread, NULL, NULL, NULL, 10, 0, 0);
