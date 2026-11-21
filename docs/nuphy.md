# NuPhy 2.4G, QMK keyboards

This page compares SpareDongle with NuPhy's own nRF52 dongle, as decompiled from NuPhy's update
image, then describes the link on air. NuPhy publishes no documentation of either.

The Air60, Air75 and Air96 V2, the Gem80 tri-mode and the Halo75 and Halo96 V2 share one dongle
firmware, and the Kick75 QMK/VIA shares the Air75 V2's radio code, though its radio module's
firmware was not compared. Only the Air75 V2 was tested.

- [Features](#features)
- [Reliability](#reliability)
- [Security](#security)
- [On air](#on-air)

In the comparison tables, 🟢 marks a row where SpareDongle does better, ⚪ one where both do the same,
🔴 one where NuPhy's dongle does better, and no mark one where the other side was not checked.

## Features

| | NuPhy dongle | SpareDongle |  |
|---|---|---|---|
| **Devices** | | | |
| Keyboards connected at once | one, and a [second slot](#second-pairing-slot) no known keyboard uses | one | ⚪ |
| Keyboards stored | one, and a [second slot](#second-pairing-slot) no known keyboard uses | one | ⚪ |
| Mice | the keyboard's mouse keys | the same | ⚪ |
| Mouse wheel | in notches | the same | ⚪ |
| **Keys** | | | |
| Keyboard keys (6KRO) | yes | yes | ⚪ |
| More than six held keys | yes, with the Win/Mac switch on Win | the same | ⚪ |
| Media keys | yes | yes | ⚪ |
| Power and sleep keys | yes | yes | ⚪ |
| Vendor-specific keys | the keyboard sends none | the same | ⚪ |
| **Back to the keyboard** | | | |
| Lock LEDs | yes | yes | ⚪ |
| Wakes a suspended PC | on the keyboard's hello or any key | the same | ⚪ |
| **Pairing** | | | |
| Pairing opens | 30 s after plugging in | 30 s after plugging in, or on request | 🟢 |
| Forgets a device | no: a new pairing replaces the keyboard | yes, with the tool | 🟢 |
| **Use** | | | |
| Boot menus | yes | yes | ⚪ |
| Moves to another PC with the dongle | yes | yes | ⚪ |
| **Shown by the tool** | | | |
| Name | used as the dongle's USB name | yes | ⚪ |
| Battery | no: the link carries none | the same | ⚪ |
| Signal | no | yes | 🟢 |
| Signal history | no | per minute | 🟢 |
| Noise history | no | per minute, on the channel it listens on | 🟢 |
| Noise on the keyboard's channels | no | yes | 🟢 |
| Noise map | no | yes | 🟢 |
| Latency | no | yes | 🟢 |
| Events | no | connects, gaps, link losses, channel moves and pairings | 🟢 |
| **[Logitech HID++](unifying.md#hid)** | | | |
| HID++ used by the dongle | no: the keyboard speaks none | the same | ⚪ |
| HID++ passed to Logitech Options and Solaar | no: the keyboard speaks none | the same | ⚪ |
| **NuPhy only** | | | |
| Tells the keyboard the PC suspended | yes | yes | ⚪ |

Both close pairing once the paired keyboard connects. With no keyboard paired, SpareDongle keeps
it open until one pairs.

The official dongle has a [second pairing slot](#second-pairing-slot) whose keyboard's keys it
merges with the first one's, but no known NuPhy keyboard pairs into it, so SpareDongle does not
implement it.

## Reliability

| | NuPhy dongle | SpareDongle |  |
|---|---|---|---|
| Valid reports dropped | some | none | 🟢 |
| Channel choice | one noise sample per channel, at every power-up | a 1.5 s noise survey, at pairing | 🟢 |
| Channel after pairing | hops whenever the keyboard goes quiet | moves only to a quieter channel | 🟢 |

The official dongle drops every packet but the last when several arrive at once, a keyboard report
while USB is busy, and every report for about 2 s after the PC resumes. SpareDongle reads every
packet and queues reports while USB is busy.

SpareDongle picks the three channels with the fewest Wi-Fi bursts, away from the Bluetooth LE
advertising channels. The official dongle picks new ones at every power-up while the keyboard keeps
its own, so the link runs on the seven fixed channels built into the keyboard.

SpareDongle keeps measuring the noise on the keyboard's channels between packets and moves to a
clearly quieter one. Only noise strong enough to matter at the keyboard's signal counts, so a
keyboard close to the dongle rarely causes a move.

Both lose a packet that fails NuPhy's checksum ([Channels and delivery](#channels-and-delivery)).

## Security

| | NuPhy dongle | SpareDongle |  |
|---|---|---|---|
| Keystroke encryption | none | none | ⚪ |
| Who can type | anyone sending to the dongle's address | the same | ⚪ |
| A new keyboard's pair request | answered for 30 s after plugging in | answered while pairing is open | 🟢 |
| A pair request reusing the dongle's ID | answered at any time | ignored | 🟢 |
| A frame restarting the dongle into its bootloader | obeyed | ignored | 🟢 |
| Firmware | NuPhy-signed only | any image, see [Firmware updates](../README.md#firmware-updates) | 🔴 |

The dongle's address is derived from its ID, which it sends in the clear in its pair reply and in
every status frame. Anyone who has heard it can type into the PC through either dongle, and can take
the official one offline with a bootloader frame. A closed pairing window keeps out other keyboards,
not an attacker.

## On air

The keyboard and the dongle talk over Nordic Enhanced ShockBurst (ESB), the proprietary 2.4 GHz
protocol of the nRF52 radio. The keyboard's radio module is the transmitter (PTX) and the dongle is
the receiver (PRX). Everything the dongle sends back is an ACK payload, attached to the hardware
ACK of the keyboard's next packet on that pipe.

### Radio settings

The ESB configuration is in `nuphy_run()` in `src/nuphy_main.c`. What the code does not show:

- Pipe 0 carries pairing. After pairing, the keyboard sends everything else on pipe 1.
- The keyboard transmits at +4 dBm, and at -20 dBm while pairing.

### Messages

Frame layouts, type codes and the checksum are in `src/nuphy.h` and `src/nuphy.c`. What the
code does not show:

- Keyboard, consumer and system reports carry the current key state, not key events.
- The keyboard gives every transmission a new sequence number, including a resend of the same
  report.
- A hello follows any recovery from failed sends, a wake included, so it does not mark a wake.
- A pair request carries the ID of the dongle the keyboard was last paired with.
- NuPhy's dongle answers `0x86`, a pair request with its own ID, at any time, and restarts into its
  bootloader on `0x40`.
- The radio module passes a USB suspend message on to the keyboard's main controller, which runs
  QMK.
- The keyboard sends `0x62` only with its Win/Mac switch on Win, and then only for the keys past
  the six in its `0x61` report.

### Pairing

1. The keyboard enters pairing and sends `0x80` on pipe 0, hopping over the
   pairing channels 2, 50 and 80.
2. The dongle answers with `0x32`. The keyboard stores the dongle ID and the three channels.
3. The keyboard sends `0x81`. The dongle checks both IDs.
4. The dongle queues a `0x30` status on pipe 1. Once the keyboard receives it, pairing is complete.

### Second pairing slot

- A second keyboard pairs the same way, with `0x84`, `0x85` and `0x87` in place of `0x80`, `0x81`
  and `0x86`, in the same 30 s window. The dongle answers with `0x36`.
- The second keyboard sends on pipe 2.
- From any packet on pipe 2 until about 1 s after the last one, the dongle merges both keyboards'
  key reports. Mouse, consumer and system reports have one slot shared by both keyboards.
- While it merges, a `0x61` report also drops the packet queued after it.
- The Air75 V2's radio module never sends the second-slot frames and does not handle a `0x36` reply.

### Channels and delivery

Both sides use the same 13-entry channel table: the pairing channels 2, 50 and 80, then the three
channels from the pair reply, then the seven fixed channels 16, 44, 72, 22, 48, 58 and 78 built into
the keyboard. ESB channel n is 2400 + n MHz.

The keyboard resends a report until it is ACKed. After a failed send it moves to the next of its ten
non-pairing channels and resends there. It stays on a channel that works.

The radio hardware ACKs a packet once its CRC passes, before the firmware checks the `0x5A` marker
and the checksum. A packet that passes the CRC but fails the checksum is lost: it has already been
ACKed.

The keyboard treats the link as lost after about 1 s without a `0x30` status.

QMK sends every report three times to the radio module and repeats the current state about every
50 ms, so the dongle sees many duplicate frames.
