#include "protocol.h"

static bool known(uint32_t v)
{
	return v == PROTOCOL_NUPHY || v == PROTOCOL_UNIFYING || v == PROTOCOL_BLUETOOTH;
}

enum protocol protocol_from_stored(bool loaded, uint8_t value)
{
	return loaded && known(value) ? (enum protocol)value : PROTOCOL_NUPHY;
}

bool protocol_from_arg(uint32_t arg, enum protocol *out)
{
	if (!known(arg)) {
		return false;
	}
	*out = (enum protocol)arg;
	return true;
}

bool protocol_has_esb(enum protocol p)
{
	return p != PROTOCOL_BLUETOOTH;
}
