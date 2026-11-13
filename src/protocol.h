#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

enum protocol {
	PROTOCOL_NUPHY = 1,
	PROTOCOL_UNIFYING = 2,
};

/* A dongle that never chose, or holds a value this firmware does not know, runs NuPhy. */
enum protocol protocol_from_stored(bool loaded, uint8_t value);

/* Returns false for an arg naming no protocol, 0 included. */
bool protocol_from_arg(uint32_t arg, enum protocol *out);

#endif
