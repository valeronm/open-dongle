# OpenDongle

An open replacement 2.4 GHz receiver for wireless keyboards, running on a [Pro Micro
nRF52840](#hardware). It supports the NuPhy Air75 V2 and Logitech Unifying keyboards, one
[protocol](#keyboard-protocols) at a time; the Unifying keyboard tested is the MX Keys.

NuPhy's own dongle loses keys even with the keyboard 20 cm away. This one reads every packet the
radio delivers, queues reports while USB is busy, and picks quiet channels when it pairs. The
keyboard keeps its own firmware.

## Hardware

- A Pro Micro nRF52840, often sold as a nice!nano clone.
- A USB-C cable to the PC.
- Optionally, the [printed case](enclosure/README.md).

No soldering is needed. The board needs the Adafruit UF2 bootloader on SoftDevice S140 6.x. New
boards ship with it under a factory program, which [`open-dongle flash`](#flashing-the-firmware)
replaces.

Only the Pro Micro has been tested. A nice!nano or another clone with the same bootloader should
work. Other nRF52840 boards need their own Zephyr board target, and `open-dongle flash`
and the udev rule recognize only this bootloader.

## Installing the tool

`open-dongle` flashes the dongle and reads its [diagnostics](#diagnostics). It runs on x86_64 Linux
and Apple Silicon macOS; Windows is untested. To install the latest release into `~/.local/bin`,
or into `PREFIX` when it is set:

```
curl -fsSL https://raw.githubusercontent.com/valeronm/open-dongle/main/tools/get.sh | sh
```

On Linux, a udev rule lets the tool reach the board without root in every state
[`open-dongle flash`](#flashing-the-firmware) handles. The installer leaves the rule in
`~/.local/share/open-dongle` and prints the commands that install it.

## Flashing the firmware

Download the `.uf2` from the [latest release](https://github.com/valeronm/open-dongle/releases/latest)
and run:

```
open-dongle flash open-dongle-<version>-promicro_nrf52840.uf2
```

`open-dongle flash` writes firmware to a running dongle, to a board in its bootloader, or to a new
board still running its factory program, restarting it into the bootloader as needed. A board
`open-dongle flash` does not find enters its bootloader when RST is shorted to GND twice quickly.

## Keyboard protocols

One firmware speaks both radio protocols, but only one runs at a time, chosen by a setting the
dongle keeps in flash. A new dongle runs NuPhy. `open-dongle protocol unifying` or
`open-dongle protocol nuphy` switches it, and the dongle reboots into the new protocol. Each
protocol keeps its own pairing, so switching back needs no new pairing.

Forced channels under [Diagnostics](#diagnostics) exist only in NuPhy mode. In Unifying mode a
silence is logged but not counted as a link loss, because an idle MX Keys sleeps after a few
minutes.

## Pairing

A NuPhy keyboard starts pairing itself: put it in 2.4G mode and hold Fn+4 for 3 seconds. The dongle
accepts pairing whenever it has no link. It measures the radio noise for about 1.5 seconds and gives
the keyboard the three quietest channels, which both remember. The keyboard transmits at low power
while pairing, so keep it close to the dongle. If Wi-Fi around you changes and the link gets worse,
pair again.

A Unifying keyboard pairs only while the dongle has pairing open. `open-dongle pair` opens it for
30 seconds; hold an Easy-Switch key on the keyboard until it blinks fast. A new keyboard replaces
the one paired before.

## LED

| LED | Meaning |
|-----|---------|
| short flash every second | no keyboard link |
| fast blink | pairing |
| solid | connected |
| off | the PC has suspended USB |
| three flashes every two seconds | the radio failed to start |

## Diagnostics

`open-dongle` reads the dongle over a vendor HID interface: link state and signal, counter totals,
per-minute counters for the last day, recent events and a radio noise map. With no arguments it
shows a live view, where `p` switches the protocol after asking and `a` opens Unifying pairing;
`--help` lists the subcommands. The history lives in the dongle's RAM, so it covers only the time
since the dongle was last plugged in, reset or switched to the other protocol. A noise map takes the
same measurement as NuPhy [pairing](#pairing) and stops the keyboard meanwhile, because the radio
cannot listen while it measures. Each minute of history also records the noise on the home channel,
the one the dongle listens on, and each gap or loss event the noise on the channel the link was on,
both read between keyboard packets. A NuPhy keyboard's home channel is the first of its three. In
Unifying mode the minute's noise is recorded only while pairing is closed, because the pairing
sweep moves the dongle across channels.

## Building from source

- [mise](https://mise.jdx.dev), which installs Rust, CMake, Ninja, Python and uv at the versions in
  `mise.toml`.
- A C compiler, for the tests.
- On macOS, the Xcode command line tools and `dtc` (for example `brew install dtc`).

In the clone:

```
mise trust
mise install
mise run setup
```

`mise run setup` downloads the nRF Connect SDK release named in `mise.toml` and the Zephyr SDK it
requires, about 8 GB, into `~/ncs`, or into `NCS_ROOT` when it is set. It skips what is already in
place, so it is safe to rerun. The ESB library and the USB device stack change their APIs between
SDK releases, so other versions are untested.

```
mise run build
mise run flash [serial]
cargo install --path tool
```

`mise run flash` flashes the image `mise run build` wrote. The udev rule is in `tools/udev`.

## Development

`mise run test` runs the firmware modules' tests on the host and the tool's tests. It does not need
the SDKs. `mise run lint` runs rustfmt, clippy and shellcheck. CI runs both on `main` and on pull
requests.

The keyboards' radio protocols, beyond what the code shows, are in
[docs/nuphy-v2-2.4ghz.md](docs/nuphy-v2-2.4ghz.md) for the NuPhy and
[docs/unifying-pairing.md](docs/unifying-pairing.md) for Logitech Unifying.

A release is built when a `v<version>` tag is pushed. Set the version in `tool/Cargo.toml`, which
the firmware reads too, add its section to `CHANGELOG.md`, commit, and push the tag. The release
fails when the tag, the version and the changelog disagree.
