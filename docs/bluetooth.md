# Bluetooth LE

This page compares SpareDongle with the PC's own Bluetooth, then describes the link as the dongle
uses it. The Air75 V2, the MX Keys and the MX Master 3S were tested.

The dongle holds one keyboard and one mouse at a time, and the two can come from different makers.

- [Features](#features)
- [Reliability](#reliability)
- [Security](#security)
- [On air](#on-air)

In the comparison tables, 🟢 marks a row where SpareDongle does better, ⚪ one where both do the same,
🔴 one where the PC's Bluetooth does better, and no mark one where the other side was not checked.

## Features

| | PC's Bluetooth | SpareDongle |  |
|---|---|---|---|
| **Devices** | | | |
| Keyboards connected at once | as many as the PC pairs | one | 🔴 |
| Keyboards stored | as many as the PC pairs | up to eight, mice included | 🔴 |
| Mice | as many as the PC pairs | one, beside the keyboard | 🔴 |
| Mouse wheel | high resolution with Logitech Options or Linux's HID++ driver[^hidpp-driver] | high resolution on Logitech mice | ⚪ |
| **Keys** | | | |
| Keyboard keys (6KRO) | yes | yes | ⚪ |
| More than six held keys | yes | yes | ⚪ |
| Media keys | yes | yes | ⚪ |
| Power and sleep keys | yes | yes | ⚪ |
| Vendor-specific keys | yes | no, besides the brightness keys it diverts | 🔴 |
| **Back to the keyboard** | | | |
| Lock LEDs | yes | yes, where the device has them | ⚪ |
| Wakes a suspended PC | on any key or mouse movement | the same | ⚪ |
| **Pairing** | | | |
| Pairing opens | in the PC's settings | at plug-in while nothing is paired, or on request | ⚪ |
| Forgets a device | in the PC's settings | yes, with the tool | ⚪ |
| **Use** | | | |
| Boot menus | no | yes | 🟢 |
| Moves to another PC with the dongle | no: a pairing per PC | yes | 🟢 |
| **Shown by the tool** | | | |
| Name | in the PC's settings | yes | ⚪ |
| Battery | in the PC's settings | yes, with charging on Logitech devices ([Reports](#reports)) | ⚪ |
| Signal | in the PC's settings | yes | ⚪ |
| Signal history | no | per minute | 🟢 |
| Noise history | no | per minute, across the data channels | 🟢 |
| Noise on the keyboard's channels | no | no: the devices hop across every channel | ⚪ |
| Noise map | no | no | ⚪ |
| Latency | no | yes | 🟢 |
| Events | no | connects, disconnects, pairings and refused pairings | 🟢 |
| **[Logitech HID++](unifying.md#hid)** | | | |
| HID++ used by the dongle | no: left to software on the PC | brightness keys, battery and wheel resolution ([Reports](#reports)) | 🟢 |
| HID++ passed to Logitech Options and Solaar | yes | no | 🔴 |
| **Bluetooth only** | | | |
| PCs without Bluetooth | no | yes | 🟢 |

A report map the dongle cannot parse refuses the device, and so does a keyboard's map with no
keyboard in it or a mouse's map with no relative pointer, such as a tablet's.

The dongle switches a Logitech mouse's vertical wheel to high resolution when the PC turns on the
dongle's high-resolution wheel, and the thumb wheel stays in notches. Whether input wakes the PC
depends on its Bluetooth settings for its own Bluetooth and on its USB settings for the dongle. On
the PC, Logitech Options, Solaar or Linux's HID++ driver[^hidpp-driver] speaks HID++.

## Reliability

The dongle connects to whichever stored device of each kind it hears first, and the others wait
until that one disconnects. A device switched on reconnects within about a second. After the dongle
restarts, the MX Keys can come back only at its next key press, which reaches the PC a second or
two late.

## Security

A Bluetooth link is as secure as the way its device paired, and the device decides that way. The
dongle accepts all four, as the PC's Bluetooth does, so the table compares the ways rather than the
two hosts; its marks are the colors the [tool](tool.md#bluetooth-mode) shows for each way. Every way
encrypts the link, and whoever holds the link's key can read its keys and type keys of their own as
the device.

| Pairing | Who can read and type as the device | Tool shows |
|---|---|---|
| LE Secure Connections, passkey | no one | 🟢 secure, passkey |
| LE Secure Connections, no passkey | one who stood in the middle of the pairing | 🟡 secure, no passkey |
| Legacy, passkey | one who recorded the pairing, by trying every passkey[^crackle] | 🔴 legacy, passkey |
| Legacy, no passkey | one who recorded the pairing[^crackle] | 🔴 legacy, no passkey |

Each attack needs someone in radio range during the pairing itself, and a recorded legacy pairing
also opens the traffic recorded before the key was found. Later, an attacker can only make the
link drop by jamming the radio, whatever the way, and the device reconnects once the jamming stops,
at the latest when it is next used.

Outside pairing, only a stored device connects. While pairing is open without a passkey, any device
in range can pair and be stored. With a passkey, only a device the passkey is typed on can pair,
and the dongle offers each passkey to one attempt only, since a failed attempt can reveal it. Each
pairing shows in the events and in `spare-dongle devices`.

The MX Master 3S pairs with LE Secure Connections and no passkey, since it has no keys to type one
on; the Air75 V2 pairs legacy with no passkey step, and the MX Keys legacy with the passkey typed.
No device tested pairs with LE Secure Connections and a passkey. Pairing again does not change a
device's way.

| | PC's Bluetooth | SpareDongle |  |
|---|---|---|---|
| Firmware | the adapter maker's, updated with the PC's system | any image, see [Firmware updates](../README.md#firmware-updates) | 🔴 |

Firmware written to the dongle can read the bond keys it keeps in flash.

## On air

The dongle is the Bluetooth LE central and each device a peripheral. As a HID over GATT host it
reads each device's report map at every connection and translates a keyboard's keyboard, media and
system reports and a mouse's pointer reports into the dongle's own USB reports. It holds each
device in a slot for its kind, a keyboard or a mouse, taken from the device's advert.

Scanning, connecting, pairing and the GATT reads are in `src/ble.c`, the choice of device and the
pairing policy in `src/btpolicy.c`, the report-map parse in `src/hidmap.c`, and HID++ in
`src/hidpp.c` and `src/bluetooth_main.c`.

### Scanning and connecting

What the code does not show:

- While a slot is free and pairing is closed, the dongle scans for a few milliseconds a second: a
  scan of 30 ms every 60 ms took about an eighth of a moving mouse's reports.
- The MX Keys reconnects to its bonded host with a short burst of directed adverts only, which
  Zephyr's host drops unless the central scans under its identity address, and which the slow scan
  can miss after the dongle restarts. The Air75 V2 reconnects with undirected adverts carrying the
  HID service.
- About one connect in six to the Air75 V2 is never established: the keyboard sends no first packet
  and the link fails with reason `0x3e`, at a 7.5 ms and a 30 ms connection interval alike.
- After the dongle connects at 7.5 ms, the MX Keys moves to a 25 ms interval with peripheral latency
  20 and the MX Master 3S to 11.25 ms with latency 44.
- A key pressed on the MX Keys while it is disconnected is sent once the dongle has subscribed,
  late by the connection setup.

### Pairing

What the code does not show:

- Zephyr refuses security level 3 for a legacy pairing with a passkey, so a security level cannot
  demand the passkey step.
- The MX Keys and the MX Master 3S take a new address at every pairing, which leaves the old bond
  stored.
- Zephyr keeps the pairing method in the bond's keys, so it reads the same after a reboot and for a
  device that is not connected.

### Reports

What the code does not show:

- The MX Keys's keyboard report is 7 bytes, the modifiers and six keys with no reserved byte, and
  its consumer report carries two 16-bit usages. Its report map also declares a mouse.
- The MX Master 3S sends every button, its motion and both wheels in report 2, and HID++ both ways
  in report 17 on a vendor page.
- The MX Master 3S's vertical wheel reports notches until high resolution is switched on over HID++
  feature `0x2121`, which reports 15 steps per notch.
- The MX Master 3S's Battery Service carries only the level; its charging state comes over HID++.

[^hidpp-driver]: Linux [`hid-logitech-hidpp.c`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-logitech-hidpp.c):
    reports a HID++ device's battery level and charging state, and turns on a Logitech mouse's
    high-resolution wheel.
[^crackle]: Mike Ryan, [crackle](https://github.com/mikeryan/crackle): recovers the key of a recorded
    LE legacy pairing, by brute force when a passkey was used.
