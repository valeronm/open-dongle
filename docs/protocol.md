# Keyboard radio protocol

The keyboard and the dongle talk over Nordic Enhanced ShockBurst (ESB), the proprietary 2.4 GHz
protocol of the nRF52 radio. The keyboard's radio module is the transmitter (PTX) and the dongle is
the receiver (PRX). Everything the dongle sends back rides as an ACK payload: it is attached to the
hardware ACK of the keyboard's next packet on that pipe.

## Radio settings

The ESB configuration is in `src/radio.c`. What the code does not show:

- Pipe 0 carries pairing. After pairing, the keyboard sends everything else on pipe 1.
- The keyboard transmits at +4 dBm, and at -20 dBm while pairing, so pairing only works up close.

## Messages

Frame layouts, type codes and the checksum are in `src/protocol.h` and `src/protocol.c`. What the
code does not show:

- Keyboard, media and system reports carry the current key state, not key events.
- The keyboard gives every transmission a new sequence number, including a resend of the same
  report.
- A hello means the keyboard woke up or reconnected.
- A pair request carries the ID of the dongle the keyboard was last paired with.
- The keyboard passes a USB suspend message on to QMK.
- Name chunks, mouse reports and the NKRO bitmap are valid frames this firmware ignores.

## Pairing

1. The keyboard enters pairing (Fn+4 held for 3 s) and sends `0x80` on pipe 0, hopping over the
   pairing channels 2, 50 and 80.
2. The dongle answers with `0x32`. The keyboard stores the dongle ID and the three channels.
3. The keyboard sends `0x81`. The dongle checks both IDs.
4. The dongle queues a `0x30` status on pipe 1. Once the keyboard receives it, pairing is complete.

## Channels and delivery

Both sides use the same 13-entry channel table: the pairing channels 2, 50 and 80, then the three
channels from the pair reply, then the seven fixed channels 16, 44, 72, 22, 48, 58 and 78 built into
the keyboard. ESB channel n is 2400 + n MHz.

The keyboard resends a report until it is ACKed. After a failed send it moves to the next of its ten
non-pairing channels and resends there. It stays on a channel that works.

The radio hardware ACKs a packet once its CRC passes, before the firmware checks the `0x5A` marker
and the checksum. A packet that passes the CRC but fails the checksum has already been ACKed, so the
keyboard does not resend it.

The keyboard treats the link as lost after about 1 s without a `0x30` status. The dongle therefore
has to keep sending status frames while connected.

QMK sends every report three times to the radio module and repeats the current state about every
50 ms, so the dongle sees many duplicate frames.
