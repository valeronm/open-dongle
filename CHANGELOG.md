# Changelog

## [Unreleased]

- One firmware runs the NuPhy Air75 V2 or a Logitech Unifying keyboard (tested with the MX Keys): `open-dongle protocol` switches between them, keeping each one's pairing, and `open-dongle pair` opens Unifying pairing for 30 s.
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
