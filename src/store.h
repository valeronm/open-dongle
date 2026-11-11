#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stdint.h>

int store_init(void);
bool store_load_channels(uint8_t out[3]);
int store_save_channels(const uint8_t channels[3]);
bool store_load_forced(uint8_t out[3]);
/* NULL deletes them. */
int store_save_forced(const uint8_t *channels);

#endif
