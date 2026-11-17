# Supported protocols and how they compare

OpenDongle speaks three keyboard radio protocols, one at a time: NuPhy's 2.4G link, tested with
the Air75 V2; Logitech Unifying, tested with an original MX Keys; and Bluetooth LE, tested with
both. This page compares NuPhy and Unifying with each keyboard's own receiver and Bluetooth with
the PC's own Bluetooth, then lists what OpenDongle adds. How the 2.4G links work on air is in
[nuphy-v2-2.4ghz.md](nuphy-v2-2.4ghz.md) and [unifying.md](unifying.md).

In the tables, 🟢 marks a row where OpenDongle does better, ⚪ one where both do the same, and 🔴
one where the official receiver does better.

## NuPhy 2.4G

The comparison is with NuPhy's nRF52 dongle as decompiled from NuPhy's update image; NuPhy publishes
no documentation of it.

| | NuPhy dongle | OpenDongle |  |
|---|---|---|---|
| Keyboard keys (6KRO) | yes | yes | ⚪ |
| Media and system keys | yes | yes | ⚪ |
| N-key rollover reports | yes | ignored | 🔴 |
| Mouse reports | yes | ignored | 🔴 |
| Caps Lock and other lock LEDs on the keyboard | yes | yes | ⚪ |
| Tells the keyboard the PC suspended | yes | yes | ⚪ |
| Wakes a suspended PC | on the keyboard's hello or any key | the same | ⚪ |
| Pairing window | 30 s after plugging in | 30 s after plugging in or on request | 🟢 |
| Keyboard name | used as the dongle's USB name | shown by the tool | ⚪ |

Both close the window once the paired keyboard connects. With no keyboard paired, OpenDongle keeps
it open until one pairs.

### Reliability

| | NuPhy dongle | OpenDongle |  |
|---|---|---|---|
| Valid reports dropped | some | none | 🟢 |
| Channel choice | one noise sample per channel, at every power-up | a 1.5 s noise survey, at pairing | 🟢 |
| Channel after pairing | hops whenever the keyboard goes quiet | moves only to a quieter channel | 🟢 |

The official dongle drops every packet but the last when several arrive at once, a keyboard report
while USB is busy, and every report for about 2 s after the PC resumes. OpenDongle reads every
packet and queues reports while USB is busy.

OpenDongle picks the three channels with the fewest Wi-Fi bursts, away from the BLE advertising
channels. The official dongle picks new ones at every power-up while the keyboard keeps its own, so
the link runs on the seven fixed channels built into the keyboard.

Both lose a packet that fails NuPhy's checksum, because the radio acknowledges it before the
firmware checks it. [Channels and delivery](nuphy-v2-2.4ghz.md#channels-and-delivery) has the
details.

### Security

| | NuPhy dongle | OpenDongle |  |
|---|---|---|---|
| Keystroke encryption | none | none | ⚪ |
| Who can type | anyone sending to the dongle's address | the same | ⚪ |
| A new keyboard's pair request (`0x80`) | answered for 30 s after plugging in | answered while pairing is open | 🟢 |
| A pair request with the dongle's own ID (`0x86`) | answered at any time | ignored | 🟢 |
| A bootloader frame (`0x40`) | restarts the dongle into its bootloader | ignored | 🟢 |
| Firmware | NuPhy-signed only | any image, see [Firmware updates](#firmware-updates) | 🔴 |

The dongle's address is derived from its ID, which it sends in the clear in its pair reply and in
every status frame. Anyone who has heard it can type into the PC through either dongle, and can take
the official one offline with a `0x40` frame. A closed pairing window keeps out other keyboards, not
an attacker.

## Logitech Unifying

The comparison is with a Logitech Unifying receiver, from Logitech's public specifications, its
receiver release notes and the Unifying security research. HID++ is Logitech's protocol for
configuring the receiver and its devices.

| | Unifying receiver | OpenDongle |  |
|---|---|---|---|
| Paired devices | up to six[^unifying] | one keyboard | 🔴 |
| Mice and other devices | yes[^unifying] | no | 🔴 |
| Encrypted keystrokes | yes[^newlin] | yes | ⚪ |
| Media keys | yes | yes | ⚪ |
| Power and sleep keys | yes[^dj] | not forwarded | 🔴 |
| Keys enabled over HID++, such as the MX Keys's F1 | with Logitech Options or Solaar[^solaar-1374] | not sent[^no-frame] | 🔴 |
| Caps Lock LED on the keyboard | yes[^dj] | yes[^led] | ⚪ |
| Logitech Options and Solaar | yes[^solaar] | no | 🔴 |
| Keyboard name, serial and type | through HID++ | shown by the tool | ⚪ |
| Pairing | on request from Logitech's software | after plugging in or on request | 🟢 |

Pairing a new keyboard replaces the one OpenDongle had. Both pair a keyboard that is switched off
and on while pairing is open.[^solaar-pair][^mx-pairing] OpenDongle does not implement HID++, so
Logitech's software cannot see it.

OpenDongle refuses a keystroke that fails decryption or carries a counter used since the keyboard
last connected. Before the keyboard's counter is known, and for 5 s after a failed decryption, it
types a key press only once the next frame confirms it.

No public source describes a message telling a keyboard the PC suspended, and OpenDongle sends none.

### Reliability

| | Unifying receiver | OpenDongle |  |
|---|---|---|---|
| Channel after pairing | moves after a missed keep-alive[^newlin] | moves only to a quieter channel | 🟢 |

No public source reports how often a Unifying receiver drops reports. OpenDongle lost none of 49,832
packets from an MX Keys at close range.

### Security

The receiver comes in two hardware versions with separate firmware lines: RQR12 on the C-U0007 and
RQR24 on the C-U0008.[^bastille-11] Logitech's release notes list some fixes for only one of them.

A keyboard picks a new counter at every boot, so a receiver cannot require counters to keep rising
([After pairing](unifying.md#after-pairing)). The Unifying receiver remembers its last 23 counters,
so 23 replayed frames let older ones through again, and frames recorded while known keys were typed
let an attacker type any key.[^mengs-2] OpenDongle resets the counter on the keyboard's connect
frame, which a single forged frame can fake. Anyone who records the unencrypted pairing can derive
the link key, then read and type keystrokes.[^mengs-1]

| | Unifying receiver | OpenDongle |  |
|---|---|---|---|
| Unencrypted keystrokes (MouseJack) | refused[^mousejack-fix] | refused | ⚪ |
| Unencrypted media keys | accepted[^newlin] | accepted, any usage[^media] | ⚪ |
| Replayed keystrokes (KeyJack, CVE-2019-13053) | refused until 23 replays flush its cache[^keyjack-fix][^mengs-2] | refused until a forged connect frame | ⚪ |
| Key derived from a recorded pairing (CVE-2019-13052) | not fixed[^mengs-1][^logi-advisory] | not fixed | ⚪ |
| Pairing while pairing is closed (forced pairing) | refused on RQR12[^forced] | refused | ⚪ |
| Keyboard keys sent by a paired mouse | some refused[^rqr-10] | no mice pair | ⚪ |
| Link key read over USB (CVE-2019-13055) | refused on RQR24[^rqr-10][^logi-advisory] | no command returns it | ⚪ |
| Firmware | Logitech-signed only[^mengs-2] | any image, see [Firmware updates](#firmware-updates) | 🔴 |

## Bluetooth LE

The dongle is a HID over GATT host: it reads the keyboard's report map at every connection and
translates its keyboard, media and system reports into the dongle's own USB reports.

| | PC's Bluetooth | OpenDongle |  |
|---|---|---|---|
| Keyboard keys (6KRO) | yes | yes | ⚪ |
| Media and system keys | yes | yes | ⚪ |
| Vendor reports, such as keys set up by Logitech Options | yes | ignored | 🔴 |
| Mice | yes | no | 🔴 |
| Keyboards | as many as the PC pairs | four stored, one connected | 🔴 |
| Bluetooth Classic keyboards | yes | no | 🔴 |
| Boot menus and PCs without Bluetooth | no | yes | 🟢 |
| Moving the keyboard to another PC | a pairing per PC | moves with the dongle | 🟢 |
| Keyboard name and signal | in the PC's settings | shown by the tool | ⚪ |

The nRF52840 has no Bluetooth Classic radio. A report map the dongle cannot parse, or one with no
keyboard in it, refuses the keyboard.

### Reliability

The dongle connects to whichever stored keyboard it hears first, and the others wait until that one
disconnects. The MX Keys reconnects with adverts aimed at the dongle alone, and after the dongle
restarts several times in a row it waits for a key press before it reconnects.

### Security

Both keyboards tested pair only with LE legacy pairing, so the dongle allows it. Anyone who
records a legacy pairing can derive its key and read the keyboard afterwards, with or without a
passkey.[^crackle] The passkey makes the dongle pair the keyboard it was typed on; without one, any
keyboard in pairing mode nearby can pair while the window is open. After pairing the link is
encrypted, and only a stored keyboard connects.

## Added by OpenDongle

- **Noise-aware channel moves**, in NuPhy and Unifying mode. The dongle keeps measuring the noise
  on the keyboard's channels between packets and moves to a clearly quieter one. Only noise strong
  enough to matter at the keyboard's signal counts, so a keyboard close to the dongle rarely causes
  a move.
- **One firmware for every protocol**, [switched by the tool](../README.md#keyboard-protocols).
- **Diagnostics** through the [open-dongle tool](tool.md).
- **An [LED](../README.md#led) that shows the link state.**
- **Open firmware**, [flashed by the tool](../README.md#flashing-the-firmware).

## Firmware updates

The board's UF2 bootloader accepts any image, and a diagnostic command restarts the dongle into it
over USB. Any program that can open the dongle's diagnostic interface, or anyone who plugs the
dongle into their own computer, can replace its firmware, including with one that reads the stored
Unifying link key.

## Sources

[^unifying]: Logitech, [What is Unifying](https://www.logitech.com/en-hk/resource-center/what-is-unifying.html):
    "Connect up to six Logitech Unifying wireless mice or keyboards".
[^newlin]: Marc Newlin, [MouseJack white paper](https://github.com/BastilleResearch/mousejack/blob/master/doc/pdf/DEFCON-24-Marc-Newlin-MouseJack-Injecting-Keystrokes-Into-Wireless-Mice.whitepaper.pdf):
    Unifying keyboards encrypt keyboard packets with 128-bit AES but not multimedia key packets, and a
    dongle and its device stay on a channel until a keepalive is missed (section 5.2.2).
[^dj]: Linux [`hid-logitech-dj.c`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-logitech-dj.c):
    the receiver's system control reports (System Sleep, System Power Down) and LED output reports.
[^solaar]: [Solaar](https://github.com/pwr-Solaar/Solaar) manages devices connected to a Unifying
    receiver.
[^solaar-1374]: [Solaar issue #1374](https://github.com/pwr-Solaar/Solaar/issues/1374): an MX Keys
    on a genuine receiver sends no keycode for these keys unless Solaar diverts them.
[^solaar-pair]: Solaar's [pairing window](https://github.com/pwr-Solaar/Solaar/blob/master/lib/solaar/ui/pair_window.py):
    "If the device is already turned on, turn it off and on again."
[^mx-pairing]: [Pairing](unifying.md#pairing): an MX Keys paired with a genuine receiver only when
    switched off and on.
[^no-frame]: [After pairing](unifying.md#after-pairing): F1 sent no frame of any type to OpenDongle.
[^media]: [After pairing](unifying.md#after-pairing), seen on air from an MX Keys.
[^led]: [After pairing](unifying.md#after-pairing), seen on air with an MX Keys.
[^bastille-11]: Bastille, [advisory 11](https://github.com/BastilleResearch/mousejack/blob/master/doc/advisories/bastille-11.logitech.public.txt): the C-U0007 on
    firmware 012.005 and the C-U0008 on 024.003.
[^mousejack-fix]: Logitech's release notes for [RQR12.05](https://github.com/Logitech/fw_updates/blob/master/RQR12/RQR12.05/RQR12.05_B0028.txt),
    [RQR24.03](https://github.com/Logitech/fw_updates/blob/master/RQR24/RQR24.03/RQR24.03_B0027.txt),
    [RQR12.07](https://github.com/Logitech/fw_updates/blob/master/RQR12/RQR12.07/RQR12.07_B0029.txt) and
    [RQR24.05](https://github.com/Logitech/fw_updates/blob/master/RQR24/RQR24.05/RQR24.05_B0029.txt), fixing Bastille's advisories
    [2](https://github.com/BastilleResearch/mousejack/blob/master/doc/advisories/bastille-2.logitech.public.txt) and [11](https://github.com/BastilleResearch/mousejack/blob/master/doc/advisories/bastille-11.logitech.public.txt).
[^keyjack-fix]: Logitech's release notes for [RQR12.08](https://github.com/Logitech/fw_updates/blob/master/RQR12/RQR12.08/RQR12.08_B0030.txt)
    and [RQR24.06](https://github.com/Logitech/fw_updates/blob/master/RQR24/RQR24.06/RQR24.06_B0030.txt), fixing Bastille's
    [advisory 13](https://github.com/BastilleResearch/keyjack/blob/master/doc/advisories/bastille-13.logitech.public.txt).
[^mengs-2]: Marcus Mengs, [report 2](https://github.com/mame82/UnifyingVulnsDisclosureRepo/blob/master/vulnerability_reports/report2_git.pdf), tested on signed RQR24.07 firmware: the
    receiver keeps its last 23 counters in RAM, frames under new counters flush them, and keystrokes
    recorded once while known keys were typed allow typing any key.
[^mengs-1]: Marcus Mengs, [report 1](https://github.com/mame82/UnifyingVulnsDisclosureRepo/blob/master/vulnerability_reports/report1_git.pdf): the link key is derived from a recorded
    pairing.
[^logi-advisory]: Logitech, [Unifying Receiver Update](https://support.logi.com/hc/en-us/articles/360026159553-Logitech-Unifying-Receiver-Update):
    CVE-2019-13052 and CVE-2019-13053 will not be fixed; firmware since August 2019 addresses
    CVE-2019-13054/55.
[^forced]: Logitech's release notes for [RQR12.05](https://github.com/Logitech/fw_updates/blob/master/RQR12/RQR12.05/RQR12.05_B0028.txt), fixing
    Bastille's [advisory 1](https://github.com/BastilleResearch/mousejack/blob/master/doc/advisories/bastille-1.logitech.public.txt), which tested only RQR12 firmware.
[^crackle]: Mike Ryan, [crackle](https://github.com/mikeryan/crackle): recovers the key of a recorded
    LE legacy pairing, by brute force when a passkey was used.
[^rqr-10]: Logitech's release notes for [RQR24.10](https://github.com/Logitech/fw_updates/blob/master/RQR24/RQR24.10/RQR24.10_B0036.txt), fixing
    CVE-2019-13055 and refusing some keystrokes from pointing devices, and for
    [RQR12.10](https://github.com/Logitech/fw_updates/blob/master/RQR12/RQR12.10/RQR12.10_B0032.txt), refusing the keystrokes only.
