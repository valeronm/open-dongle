#include "protocol.h"

enum protocol protocol_from_stored(bool loaded, uint8_t value)
{
	return loaded && diag_protocol_known(value) ? (enum protocol)value : PROTOCOL_NUPHY;
}

bool protocol_from_arg(uint32_t arg, enum protocol *out)
{
	if (!diag_protocol_known(arg)) {
		return false;
	}
	*out = (enum protocol)arg;
	return true;
}
