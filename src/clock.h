#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>
#include <zephyr/kernel.h>

/* Wraps about every 71 minutes. */
static inline uint32_t clock_now_us(void)
{
	return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

#endif
