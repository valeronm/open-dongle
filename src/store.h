#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int store_init(void);
/* True only when exactly len bytes were stored. */
bool store_load_nuphy(void *out, size_t len);
int store_save_nuphy(const void *in, size_t len);
int store_delete_nuphy(void);
bool store_load_forced(uint8_t out[3]);
/* NULL deletes them. */
int store_save_forced(const uint8_t *channels);
/* True only when exactly len bytes were stored. */
bool store_load_unifying(void *out, size_t len);
int store_save_unifying(const void *in, size_t len);
int store_delete_unifying(void);
/* True only when exactly len bytes were stored. */
bool store_load_bluetooth(void *out, size_t len);
int store_save_bluetooth(const void *in, size_t len);
bool store_load_protocol(uint8_t *out);
int store_save_protocol(uint8_t value);

#endif
