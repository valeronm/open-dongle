# Changelog

## Unreleased

- In NuPhy mode the PC receives every held key in the keyboard's Windows mode, not only six.
- In Bluetooth mode the PC receives every held key, where a seventh key used to make it drop them
  all.
- In NuPhy mode the mouse reports a keyboard sends reach the PC.
- In Unifying and Bluetooth mode a Logitech keyboard's screen brightness keys, such as the MX
  Keys's F1 and F2, work without Logitech's software.
- The events record each time the dongle diverts a Logitech keyboard's keys or sets a Logitech
  mouse's wheel resolution, or what stopped it.
- In Bluetooth mode a Logitech mouse's wheel scrolls in high resolution where the PC turns it on,
  as Linux does, and in notches elsewhere.
- In Bluetooth mode the history and the live view's chart show the radio noise again.
- In Bluetooth mode the tool shows the passkey once the keyboard has connected and waits for it,
  naming the keyboard where it advertises a name, and `pair` waits until pairing closes and says
  whether the keyboard paired.
- In Bluetooth mode a failed passkey pairing closes pairing, and opening it again gives a new
  passkey.
- The events record each time a keyboard waits for the passkey.
- In the live view, the protocol to switch to is chosen with `n` for NuPhy, `u` for Unifying and
  `b` for Bluetooth, and Bluetooth pairing with `p` for a passkey and `n` for none, instead of `1`
  and `2`.
- In NuPhy mode `devices` names the keyboard as `status` does, without "Dongle" at the end.
- In NuPhy mode the dongle remembers the paired keyboard's ID and name across restarts for
  `status` and `devices`.
- In NuPhy mode a dongle with no keyboard paired ignores keyboards until one pairs, so a keyboard
  paired before this update has to pair again once with Fn+4.
- The tool shows a legacy Bluetooth pairing in red even when a passkey was typed.
- The tool shows a device's battery level and whether it is charging, as far as the device
  reports them, in `status` and in the title of the live view's device box.
- A connected device's latency reads "no report yet" until its first report reaches the PC.
- The dongle no longer counts channel changes, so the live view's chart, `status`, `history` and the
  console's counters line show none; each channel move is in the events.
- The installer offers to install the udev rule with sudo, and to flash the release's firmware onto
  a plugged-in dongle or board.
- On Linux, `flash` reports a missing udev rule before it asks to flash, not after.
- The diagnostic protocol is now 5, so the 0.4.0 tool can only `flash` new firmware.

## [0.4.0] - 2026-11-19

- In Bluetooth mode the dongle connects a mouse beside the keyboard, or either alone, and stores up
  to eight devices.
- A mouse pairs in any pairing window without a passkey, and a device that pairs replaces the
  connected one of its kind.
- The tool shows each device separately, with its own signal, latency and pairing security; the
  live view puts the device boxes side by side below the dongle's own, or steps through them with
  `o` in a narrow terminal.
- In Bluetooth mode the live view leaves out the channel noise and busy readings, since Bluetooth
  hops across every channel, and its dropped count counts Bluetooth events lost to a full queue.
- A link loss or disconnect gives the time since the last key to the second, up to 18 h.
- Events name the device they are about, and record each Bluetooth device the dongle forgets.
- The LED shows connected while a keyboard or a mouse is connected.
- `spare-dongle keyboards` is now `spare-dongle devices`, and the live view's `k` is now `d`.
- Bluetooth keyboards' reports reach the PC sooner.
- A Bluetooth keyboard may need a key press to reconnect after the dongle restarts.
- The tool shows the bootloader's and the SoftDevice's versions.
- On Linux, a dongle the tool cannot access gets an error naming the udev rule and the commands
  that install it.
- The installer's udev commands apply the rule to a board already plugged in, with no replug.
- The diagnostic protocol is now 4, so the 0.3.0 tool can only `flash` new firmware.

## [0.3.0] - 2026-11-18

- The project is renamed SpareDongle, and its tool `spare-dongle`.
- A third protocol, Bluetooth LE, stores up to four keyboards and connects one at a time.
- In Bluetooth mode `spare-dongle pair` prints a passkey to type on the keyboard.
- `spare-dongle pair --no-passkey` pairs a keyboard without a passkey step, such as the Air75 V2.
- `spare-dongle keyboards` lists the stored Bluetooth keyboards and `spare-dongle forget` removes
  one.
- The history records Bluetooth connects, disconnects, pairings and refused pairings.
- In Bluetooth mode the live view's `k` lists and forgets keyboards.
- The status and live view show a Bluetooth keyboard's name, signal and last packet.
- Before the first packet the status shows no signal level instead of 0 dBm.
- The diagnostic protocol is now 3, so the 0.2.0 tool can only `flash` new firmware.

## [0.2.0] - 2026-11-16

- One firmware runs either the NuPhy Air75 V2 or a Logitech Unifying keyboard such as the MX Keys.
- `open-dongle protocol` switches between the two and keeps each one's pairing.
- A keyboard pairs only while pairing is open, after `open-dongle pair` or plugging the dongle in.
- A NuPhy dongle without a link no longer accepts pairing at any time.
- The history, events and noise map work in Unifying mode too.
- In Unifying mode a silence is logged but not counted as a link loss, since an idle MX Keys sleeps.
- In Unifying mode the live view and `open-dongle history` leave out losses and gaps.
- The dongle moves to a quieter channel on its own and logs the noise on both channels.
- A Unifying dongle moves only among the pairing channels, the only ones its keyboard searches.
- A NuPhy dongle moves among the keyboard's three paired and seven fixed channels.
- `open-dongle moves`, or `m` in the live view, shows the noise behind those moves without stopping
  the keyboard.
- The noise map marks the channels a move can reach.
- `open-dongle move`, or `[` and `]` in the live view, moves the link by hand for testing.
- A NuPhy dongle does not move while the PC is suspended.
- A channel move waits until no key is held, so no key release is lost on the old channel.
- A paired NuPhy dongle that loses its keyboard stays on its channel instead of checking pairing
  channel 2.
- Silence events record the minutes since the last key and whether the keyboard woke.
- In the live view, `p` switches the protocol after asking and `a` opens pairing for 30 s.
- The live view's status box gives the link, the keyboard's channels and the dongle a row each.
- The history panel charts channel changes per minute.
- `open-dongle status` and the live view name the paired keyboard in both modes.
- An install script installs the latest release.
- The channels pairing offers can be set by hand, for interference too brief for the survey to see.
- The tool names the errors the dongle reports.
- The history and events record the noise on the link's channel, read between keyboard packets.
- The live view charts that noise beside the signal.
- The live view's signal chart is labeled in dBm and wall-clock time and fits a 24-row terminal.
- The live view shows why it lost the dongle or why a reply failed.
- The live view's noise map fits in 80 columns.
- The diagnostic protocol is now 2, so the 0.1.0 tool can only `flash` new firmware.

## [0.1.0] - 2026-11-11

First release.

- The dongle receives the NuPhy Air75 V2's keyboard, media and system keys in 2.4G mode.
- It runs on a Pro Micro nRF52840 and queues reports while USB is busy.
- Pairing measures the radio noise and gives the keyboard the three quietest channels.
- `open-dongle` shows a live view, status, a day of per-minute history, events and a noise map.
- `open-dongle flash` writes firmware to a running dongle, a board in its bootloader, or a new
  board.
