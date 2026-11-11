# OpenDongle

An open replacement 2.4 GHz receiver for wireless keyboards, running on a Pro Micro nRF52840 (a
nice!nano clone). The supported keyboard is the NuPhy Air75 V2.

NuPhy's own dongle loses keys even with the keyboard 20 cm away. This one reads every packet the
radio delivers, queues reports while USB is busy, and picks quiet channels when it pairs. The
keyboard keeps its own firmware.

## Pairing

Put the keyboard in 2.4G mode and hold Fn+4 for 3 seconds. The dongle accepts pairing whenever it
has no link. It measures the radio noise for about 1.5 seconds and gives the keyboard the three
quietest channels, which both remember. The keyboard transmits at low power while pairing, so keep
it close to the dongle. If Wi-Fi around you changes and the link gets worse, pair again.

## LED

| LED | Meaning |
|-----|---------|
| short flash every second | no keyboard link |
| fast blink | pairing |
| solid | connected |
| off | the PC has suspended USB |
| three flashes every two seconds | the radio failed to start |

## Diagnostics

`tool/` holds `open-dongle`, a tool that reads the dongle over a vendor HID interface: link state
and signal, per-minute counters for the last day, recent events and a radio noise map, plus reboot,
bootloader and clear actions. With no arguments it shows a live view; `--help` lists the
subcommands. The history lives in the dongle's RAM, so it covers only the time since the dongle was
last plugged in or reset. A noise map stops the keyboard for about 1.5 s, because the radio cannot
listen while it measures.

`open-dongle flash <file.uf2>` writes firmware to a running dongle, to a board in its bootloader, or
to a new board still running its factory program, restarting it into the bootloader as needed. The
firmware needs a bootloader on SoftDevice S140 6.x.

Build the tool on the machine that runs it: a macOS binary cannot be built on Linux without Apple's
SDK, and on macOS the HID library needs the Xcode command line tools. On Linux, the rule in
`tools/udev` lets the tool reach the dongle, its bootloader and a new board's factory program
without root.

## Limits

Mouse keys, more than six simultaneous keys, and NuPhy's configuration channel are not supported.

## Building, testing and flashing

Requires nRF Connect SDK v3.4.2 in `/opt/ncs/v3.4.2` with Zephyr SDK 1.0.1 in
`/opt/ncs/zephyr-sdk-1.0.1`. The ESB library and the USB device stack change their APIs between
SDK releases, so other versions are untested.
`./build.sh test` and `./build.sh flash` run the tool, so they need a Rust toolchain.

```
./build.sh build
./build.sh test
./build.sh flash [serial]
```

Without this firmware, the board enters its bootloader when RST is shorted to GND twice quickly.
