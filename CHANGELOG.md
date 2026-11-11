# Changelog

## [Unreleased]

- An install script that installs the latest release.

## [0.1.0] - 2026-11-11

First release.

- Receiver for the NuPhy Air75 V2 in 2.4G mode on a Pro Micro nRF52840: keyboard, media and system
  keys, with reports queued while USB is busy.
- Pairing measures the radio noise and gives the keyboard the three quietest channels.
- `open-dongle`: a live view, status, per-minute history for the last day, recent events and a
  radio noise map, and `flash`, which writes firmware to a running dongle, a board in its
  bootloader, or a new board on its factory program.
