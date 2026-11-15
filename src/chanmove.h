#ifndef CHANMOVE_H
#define CHANMOVE_H

#define CHANMOVE_NO_HOME (-1)

enum chanmove_result {
	CHANMOVE_DONE,
	/* The keyboard never looks for the dongle on that channel. */
	CHANMOVE_NOT_A_CHANNEL,
	/* No keyboard paired, pairing open, in NuPhy the host suspended or, in Unifying, a pairing
	 * handshake running. */
	CHANMOVE_NOT_MOVABLE,
};

#endif
