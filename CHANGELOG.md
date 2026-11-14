# Changelog

## [Unreleased]

- One firmware runs the NuPhy Air75 V2 or a Logitech Unifying keyboard (tested with the MX Keys): `open-dongle protocol` switches between them, keeping each one's pairing.
- A keyboard pairs only while pairing is open, after `open-dongle pair` or the dongle being plugged
  in; a NuPhy dongle used to accept pairing whenever it had no link.
- The history, events and noise map work in Unifying mode as well as NuPhy mode. A silence there is
  logged but not counted as a link loss, because an idle MX Keys sleeps after a few minutes.
- In Unifying mode the dongle moves to a quieter channel on its own, judging noise at the
  keyboard's signal level, and logs each move with the noise on both channels. It moves only among
  the pairing channels, the only ones the keyboard searches after a failed send.
- `open-dongle move`, or `[` and `]` in the live view, moves the link to one of the keyboard's
  channels in either mode while a keyboard is paired and pairing is closed, for testing how the
  keyboard follows and how the dongle moves away from noise.
- A channel move, requested or for noise, waits until no key is held, since a release sent on the
  channel just left reaches the host only once the keyboard finds the new one.
- A paired NuPhy dongle that loses its keyboard waits on its channel while pairing is closed,
  instead of spending every other 400 ms on pairing channel 2: the keyboard sweeps its channels
  within milliseconds of a failed send.
- The event that starts a silence records the minutes since the last key, and the event that ends it
  records whether the keyboard woke.
- The live view's `p` key switches the protocol after asking, which restarts the history, and `a`
  opens pairing for 30 s.
- The live view's status box has a row each for the link, the keyboard's channels (the current one
  and the paired ones marked) and the dongle, colors the link state and signal, and names the
  protocol in its title. The history panel charts channel changes per minute. In Unifying mode,
  which counts no losses or gaps, the panel and `open-dongle history` leave them out.
- An install script that installs the latest release.
- The channels pairing offers can be set by hand, for interference too brief for the noise survey
  to see.
- The tool names the errors the dongle reports.
- Each minute of history records the home channel's noise, and each link loss or gap in the events
  the noise on the channel the link was on, read between keyboard packets: the loudest reading and
  the shares at or above -85 and -60 dBm.
  The tool prints it, and the live view charts the loudest reading beside the signal and the share
  at or above -85 dBm.
- The live view labels its signal chart in dBm and wall-clock time, and draws link losses, gaps,
  dropped packets and the noise share under it as bars that each carry their own scale, in one panel
  that fits a 24-row terminal.
- The live view shows why it lost the dongle or why a reply failed, and keeps its history through
  a failed reply.
- The live view's noise map fits its legend in 80 columns.
- `status` prints a keyboard ID the dongle has not received since it booted as "not seen since
  boot".
- The diagnostic protocol is now 2, so the 0.1.0 tool reads only `flash` from new firmware.

## [0.1.0] - 2026-11-11

First release.

- Receiver for the NuPhy Air75 V2 in 2.4G mode on a Pro Micro nRF52840: keyboard, media and system
  keys, with reports queued while USB is busy.
- Pairing measures the radio noise and gives the keyboard the three quietest channels.
- `open-dongle`: a live view, status, per-minute history for the last day, recent events and a
  radio noise map, and `flash`, which writes firmware to a running dongle, a board in its
  bootloader, or a new board on its factory program.
