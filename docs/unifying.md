# Logitech Unifying

This page compares SpareDongle with a Logitech Unifying receiver, from Logitech's public
specifications, its receiver release notes and the Unifying security research, then describes the
link on air. Only the original MX Keys was tested. HID++ is Logitech's protocol for configuring
the receiver and its devices.

- [Features](#features)
- [Reliability](#reliability)
- [Security](#security)
- [On air](#on-air)

In the comparison tables, 🟢 marks a row where SpareDongle does better, ⚪ one where both do the same,
🔴 one where the Unifying receiver does better, and no mark one where the other side was not checked.

## Features

| | Unifying receiver | SpareDongle |  |
|---|---|---|---|
| **Devices** | | | |
| Keyboards connected at once | up to six devices[^unifying] | one | 🔴 |
| Keyboards stored | up to six devices | one | 🔴 |
| Mice | yes | no | 🔴 |
| Mouse wheel | with a paired mouse | no mice | 🔴 |
| **Keys** | | | |
| Keyboard keys (6KRO) | yes | yes | ⚪ |
| More than six held keys | no: the keyboard sends six | the same | ⚪ |
| Media keys | yes | yes | ⚪ |
| Power and sleep keys | yes[^dj] | not forwarded | 🔴 |
| Vendor-specific keys | with Logitech Options or Solaar | no, besides the brightness keys it diverts | 🔴 |
| **Back to the keyboard** | | | |
| Lock LEDs | yes[^dj] | yes ([After pairing](#after-pairing)) | ⚪ |
| Wakes a suspended PC | on any key | the same | ⚪ |
| **Pairing** | | | |
| Pairing opens | on request from Logitech Options or Solaar | 30 s after plugging in, or on request | 🟢 |
| Forgets a device | with Logitech Options or Solaar | yes, with the tool | ⚪ |
| **Use** | | | |
| Boot menus | yes | yes | ⚪ |
| Moves to another PC with the dongle | yes | yes | ⚪ |
| **Shown by the tool** | | | |
| Name | through HID++, with serial and type | yes, with serial and type | ⚪ |
| Battery | with Logitech Options, Solaar or Linux's HID++ driver[^hidpp-driver] | yes, with charging ([HID++](#hid)) | ⚪ |
| Signal | no | yes | 🟢 |
| Signal history | no | per minute | 🟢 |
| Noise history | no | per minute, on the channel it listens on, while pairing is closed | 🟢 |
| Noise on the keyboard's channels | no | yes | 🟢 |
| Noise map | no | yes | 🟢 |
| Latency | no | yes | 🟢 |
| Events | no | connects, silences, refused keystrokes, channel moves and pairings | 🟢 |
| **[Logitech HID++](#hid)** | | | |
| HID++ used by the dongle | no: left to software on the PC | brightness keys and battery ([HID++](#hid)) | 🟢 |
| HID++ passed to Logitech Options and Solaar | yes[^solaar] | no | 🔴 |

On the PC, Logitech Options, Solaar or Linux's HID++ driver[^hidpp-driver] speaks HID++. Pairing a
new keyboard replaces the one SpareDongle had. Both pair a keyboard that is switched off
and on while pairing is open[^solaar-pair] ([Pairing](#pairing)). Logitech's software cannot see the
keyboard through SpareDongle.

## Reliability

| | Unifying receiver | SpareDongle |  |
|---|---|---|---|
| Channel after pairing | moves after a missed keep-alive[^newlin] | moves only to a quieter channel | 🟢 |

SpareDongle keeps measuring the noise on the keyboard's channels between packets and moves to a
clearly quieter one. Only noise strong enough to matter at the keyboard's signal counts, so a
keyboard close to the dongle rarely causes a move.

No public source reports how often a Unifying receiver drops reports. SpareDongle lost none of
49,832 packets from an MX Keys at close range.

## Security

The receiver comes in two hardware versions with separate firmware lines: RQR12 on the C-U0007 and
RQR24 on the C-U0008[^bastille-11]. Logitech's release notes list some fixes for only one of them.

A keyboard picks a new counter at every boot, so a receiver cannot require counters to keep rising
([After pairing](#after-pairing)). Replayed frames recorded while known keys were typed let an
attacker type any key[^mengs-2]. A link key derived from a recorded pairing reads and types
keystrokes.

SpareDongle refuses a keystroke that fails decryption or carries a counter used since the keyboard
last connected, and forgets those counters at the keyboard's connect frame, which a single forged
frame can fake. Before the keyboard's counter is known, and for 5 s after a failed decryption, it
types a key press only once the next frame confirms it.

| | Unifying receiver | SpareDongle |  |
|---|---|---|---|
| Unencrypted keystrokes (MouseJack) | refused[^mousejack-fix] | refused | ⚪ |
| Unencrypted media keys | accepted[^newlin] | accepted, any usage ([After pairing](#after-pairing)) | ⚪ |
| Replayed keystrokes (KeyJack, CVE-2019-13053) | refused until 23 replays flush its cache[^keyjack-fix][^mengs-2] | refused until a forged connect frame | ⚪ |
| Key derived from a recorded pairing (CVE-2019-13052) | not fixed[^mengs-1][^logi-advisory] | not fixed | ⚪ |
| Pairing while pairing is closed (forced pairing) | refused on RQR12[^forced] | refused | ⚪ |
| Keyboard keys sent by a paired mouse | some refused[^rqr-10] | no mice pair | ⚪ |
| Link key read over USB (CVE-2019-13055) | refused on RQR24[^rqr-10][^logi-advisory] | no command returns it | ⚪ |
| Firmware | Logitech-signed only[^mengs-2] | any image, see [Firmware updates](../README.md#firmware-updates) | 🔴 |

The keyboard sends the brightness keys SpareDongle diverts unencrypted, so anyone in radio range
can change the screen brightness, as with the media keys.

## On air

The keyboard and the receiver talk over Nordic Enhanced ShockBurst (ESB) at 2 Mbps, as on the
[NuPhy link](nuphy.md#on-air). The original MX Keys has wireless product ID (WPID) `408A`.

### Radio settings

The ESB configuration is in `unifying_radio()` in `src/unifying_main.c`. The pairing address and the
channels a paired keyboard searches are in `src/unifying.h`. What the code does not show:

- Nordic's ESB matches a Unifying address only with its first four bytes reversed as the base and
  the fifth as the prefix. Neither the address written forward nor all five bytes reversed matched
  the MX Keys.
- A receiver moved off the pairing channels, to 38, 50 or 68, was never found again.

### Pairing

Frame layouts, the checksum and the key derivation are in `src/unifying.c`.

1. The keyboard sends Request 1 on the pairing address. The receiver answers with Response 1, which
   gives the keyboard its new address and the receiver's WPID.
2. The keyboard sends Request 2 on its new address, carrying its nonce and serial. The receiver
   answers with its own nonce. Both sides then derive the link key.
3. The keyboard sends Request 3 with its name. The receiver answers with the final response.
4. The keyboard sends a completion frame, which gets no reply.

What the code does not show:

- The MX Keys sends Request 1 only at power-on, with a genuine receiver too. Holding an Easy-Switch
  key until it blinks fast makes it advertise over Bluetooth LE on that slot; the Unifying request
  goes out once the keyboard is then switched off and on.
- The filler bytes in the responses are values the MX Keys accepted.

### After pairing

- At every connect, the keyboard first sends two frames to the receiver's own address (device octet
  `0x00`) and moves to the keyboard's address only once they are ACKed.
- The keyboard sets its own keep-alive timeout; the receiver need not send one.
- The keystroke counter rises by one per frame, starts from a new value after a new pairing, and
  survives a receiver restart.
- Media keys arrive unencrypted, one HID consumer usage per frame; a release carries usage 0. Byte 6
  of the frame is `0x34` while Fn is held.
- The keyboard lights its Caps Lock LED from an LED frame with marker `0x0e`, and ignores the same
  frame with marker `0x4e`.
- In its default Windows mode, the MX Keys's top row sends Windows shortcuts and media usages, and
  F1 to F12 with Fn. Its lock key sends Super+L, and it has no power or sleep key.
- In macOS mode (Fn+O held, Fn+P back to Windows), F1 and F2 send the brightness usages `0x70` and
  `0x6f`, F3 to F5 and the lock key send macOS shortcuts, and Alt and Super swap places. The key
  labeled Clear in macOS mode sends Num Lock `0x53` in both modes.
- Fn and the keyboard's own backlight changes arrive as unencrypted HID++ notifications (type
  `0x51`) that carry no key: Fn as feature index `0x08` with value `0x0034` while held and 0 on
  release, the backlight as feature index `0x0b` with its new level.
- In Windows mode, F1 and F2 (screen brightness without Fn) send no frame of any type until the
  receiver diverts them over HID++, as Solaar does on a genuine receiver[^solaar-1374].

### HID++

The requests and the control IDs the receiver diverts are in `src/unifying.c`. What the code does
not show:

- The keyboard reads a HID++ request from an ACK payload whose type is `0x10` or `0x11`, and copies
  its device index byte into the reply unchecked. A request typed `0x50` got no reply.
- A request can go unanswered, with the keyboard heard meanwhile.
- The MX Keys has feature `0x1B04` at index 8 and lists Fn as control `0x0034` in its events, though
  nothing diverts it.
- The MX Keys forgets a divert whenever it reconnects, a receiver restart included.
- Feature index `0x0e` on the MX Keys reports its lock state in HID LED bits at connect and on
  each change: Num Lock `0x01`, Caps Lock `0x02`, Scroll Lock `0x04`.
- Feature index `0x10` on the MX Keys reports a switch of mode in byte 6: `0x00` for Windows,
  `0x01` for macOS.
- The MX Keys has feature `0x1000` (battery) at index 7 and sends its battery event at every
  connect, before a receiver can have looked the feature up.
- While it charges, the MX Keys reports its battery level as 0, which means unknown.

### Implementations

The frame layouts and the key derivation come from public implementations of the keyboard side,
checked against the MX Keys on air:

- [LOGITacker](https://github.com/RoganDawes/LOGITacker): pairing frame offsets and the key
  derivation.
- Marcus Mengs' [UnifyingVulnsDisclosureRepo](https://github.com/mame82/UnifyingVulnsDisclosureRepo)
  and [munifying](https://github.com/mame82/munifying): the key derivation, and the capability and
  device-type values.
- [decrazyo/unifying](https://github.com/decrazyo/unifying): frame structures, channel tables and
  the keystroke encryption.
- Marc Newlin's [MouseJack](https://github.com/BastilleResearch/mousejack) research: radio
  parameters and addressing.
- [Solaar](https://github.com/pwr-Solaar/Solaar): the HID++ functions and control IDs for diverting
  keys, and an MX Keys feature table in issue #1374.

### Open questions

- How long the MX Keys keeps searching for a receiver it lost.
- Which response filler bytes a keyboard checks.
- What the connect frames' fields mean beyond their observed bytes.

[^unifying]: Logitech, [What is Unifying](https://www.logitech.com/en-hk/resource-center/what-is-unifying.html):
    "Connect up to six Logitech Unifying wireless mice or keyboards".
[^newlin]: Marc Newlin, [MouseJack white paper](https://github.com/BastilleResearch/mousejack/blob/master/doc/pdf/DEFCON-24-Marc-Newlin-MouseJack-Injecting-Keystrokes-Into-Wireless-Mice.whitepaper.pdf):
    Unifying keyboards encrypt keyboard packets with 128-bit AES but not multimedia key packets, and
    a dongle and its device stay on a channel until a keepalive is missed (section 5.2.2).
[^dj]: Linux [`hid-logitech-dj.c`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-logitech-dj.c):
    the receiver's system control reports (System Sleep, System Power Down) and LED output reports.
[^hidpp-driver]: Linux [`hid-logitech-hidpp.c`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-logitech-hidpp.c):
    reports a HID++ device's battery level and charging state, and turns on a Logitech mouse's
    high-resolution wheel.
[^solaar]: [Solaar](https://github.com/pwr-Solaar/Solaar) manages devices connected to a Unifying
    receiver.
[^solaar-1374]: [Solaar issue #1374](https://github.com/pwr-Solaar/Solaar/issues/1374): an MX Keys
    on a genuine receiver sends no keycode for these keys unless Solaar diverts them.
[^solaar-pair]: Solaar's [pairing window](https://github.com/pwr-Solaar/Solaar/blob/master/lib/solaar/ui/pair_window.py):
    "If the device is already turned on, turn it off and on again."
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
[^mengs-2]: Marcus Mengs, [report 2](https://github.com/mame82/UnifyingVulnsDisclosureRepo/blob/master/vulnerability_reports/report2_git.pdf), tested on signed RQR24.07 firmware.
[^mengs-1]: Marcus Mengs, [report 1](https://github.com/mame82/UnifyingVulnsDisclosureRepo/blob/master/vulnerability_reports/report1_git.pdf).
[^logi-advisory]: Logitech, [Unifying Receiver Update](https://support.logi.com/hc/en-us/articles/360026159553-Logitech-Unifying-Receiver-Update):
    CVE-2019-13052 and CVE-2019-13053 will not be fixed; firmware since August 2019 addresses
    CVE-2019-13054/55.
[^forced]: Logitech's release notes for [RQR12.05](https://github.com/Logitech/fw_updates/blob/master/RQR12/RQR12.05/RQR12.05_B0028.txt), fixing
    Bastille's [advisory 1](https://github.com/BastilleResearch/mousejack/blob/master/doc/advisories/bastille-1.logitech.public.txt), which tested only RQR12 firmware.
[^rqr-10]: Logitech's release notes for [RQR24.10](https://github.com/Logitech/fw_updates/blob/master/RQR24/RQR24.10/RQR24.10_B0036.txt), fixing
    CVE-2019-13055 and refusing some keystrokes from pointing devices, and for
    [RQR12.10](https://github.com/Logitech/fw_updates/blob/master/RQR12/RQR12.10/RQR12.10_B0032.txt), refusing the keystrokes only.
