#ifndef INDICATOR_H
#define INDICATOR_H

enum indicator_mode {
	IND_IDLE,
	IND_PAIRING,
	IND_CONNECTED,
	IND_SLEEP,
	IND_ERROR,
};

void indicator_set(enum indicator_mode mode);

#endif
