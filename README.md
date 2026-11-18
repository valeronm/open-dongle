# SpareDongle

An open receiver for wireless keyboards, running on a [Pro Micro nRF52840](#hardware). It
supports the NuPhy Air75 V2's 2.4G link, Logitech Unifying keyboards and Bluetooth LE keyboards,
one [protocol](#keyboard-protocols) at a time; the keyboards tested are the Air75 V2 and the MX
Keys. [docs/protocols.md](docs/protocols.md) compares it with each keyboard's own receiver and with
pairing a Bluetooth keyboard to the PC. The keyboard keeps its own firmware.

## Hardware

- A Pro Micro nRF52840, often sold as a nice!nano clone.
- A USB-C cable to the PC.
- Optionally, the [printed case](enclosure/README.md).

No soldering is needed. The board needs the Adafruit UF2 bootloader on SoftDevice S140 6.x. New
boards ship with it under a factory program, which [`spare-dongle flash`](#flashing-the-firmware)
replaces.

Only the Pro Micro has been tested. A nice!nano or another clone with the same bootloader should
work. Other nRF52840 boards need their own Zephyr board target, and `spare-dongle flash`
and the udev rule recognize only this bootloader.

## Installing the tool

`spare-dongle` flashes the dongle and reads its [diagnostics](#diagnostics). It runs on x86_64 Linux
and Apple Silicon macOS; Windows is untested. To install the latest release into `~/.local/bin`,
or into `PREFIX` when it is set:

```
curl -fsSL https://raw.githubusercontent.com/valeronm/spare-dongle/main/tools/get.sh | sh
```

On Linux, a udev rule lets the tool reach the board without root in every state
[`spare-dongle flash`](#flashing-the-firmware) handles. The installer leaves the rule in
`~/.local/share/spare-dongle` and prints the commands that install it.

## Flashing the firmware

Download the `.uf2` from the [latest release](https://github.com/valeronm/spare-dongle/releases/latest)
and run:

```
spare-dongle flash spare-dongle-<version>-promicro_nrf52840.uf2
```

`spare-dongle flash` writes firmware to a running dongle, to a board in its bootloader, or to a new
board still running its factory program, restarting it into the bootloader as needed. A board
`spare-dongle flash` does not find enters its bootloader when RST is shorted to GND twice quickly.

## Keyboard protocols

One firmware speaks all three protocols, but only one runs at a time, chosen by a setting the
dongle keeps in flash. A new dongle runs NuPhy. `spare-dongle protocol nuphy`, `unifying` or
`bluetooth` switches it, and the dongle reboots into the new protocol. Each protocol keeps its own
pairing, so switching back needs no new pairing.

## Pairing

`spare-dongle pair`, or `a` in the live view, opens pairing for 30 seconds. A dongle with no
keyboard paired keeps pairing open from the moment it is plugged in until one pairs.

In NuPhy and Unifying mode, plugging the dongle in also opens pairing while a keyboard is paired,
until that keyboard connects or 30 seconds pass, so switch it off first or put it in pairing mode.
A connected NuPhy keyboard keeps `spare-dongle pair` from hearing another one, so switch it off too.
A new keyboard replaces the one paired before.

**NuPhy:** in 2.4G mode, hold Fn+4 for 3 seconds. Keep the keyboard close, since it transmits at
low power while pairing. If the link gets worse after the Wi-Fi around you changes, pair again.

**Unifying:** switch the keyboard off and on. On a keyboard with Easy-Switch keys, such as the MX
Keys, first hold an Easy-Switch key until it blinks fast.

**Bluetooth:** `spare-dongle pair` prints a six-digit passkey. Put the keyboard in Bluetooth pairing
mode, type the passkey on it and press Enter. A keyboard with no passkey step, such as the Air75
V2, pairs with `spare-dongle pair --no-passkey`. Opening pairing drops the connected keyboard until
the window closes, and once a keyboard is stored, plugging the dongle in opens nothing. The dongle
stores up to four keyboards, and a full list refuses a new pairing until one is
[forgotten](docs/tool.md#bluetooth-mode). The MX Keys pairs under a new address each time, so
pairing it again leaves its old entry to forget.

## LED

| LED | Meaning |
|-----|---------|
| short flash every second | no keyboard link |
| fast blink | pairing is open |
| solid | connected |
| off | the PC has suspended USB |
| three flashes every two seconds | the radio failed to start |

## Diagnostics

`spare-dongle` reads the dongle's link state and signal, counters, history, events and radio noise
over USB, in a live view or one command at a time. [docs/tool.md](docs/tool.md) describes what it
shows and every command.

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
cargo install --locked --path tool
```

`mise run flash` flashes the image `mise run build` wrote. The udev rule is in `tools/udev`.

## Development

`mise run test` runs the firmware modules' tests on the host and the tool's tests. It does not need
the SDKs. `mise run lint` runs rustfmt, clippy and shellcheck. CI runs both on `main` and on pull
requests.

The keyboards' radio protocols, beyond what the code shows, are in
[docs/nuphy-v2-2.4ghz.md](docs/nuphy-v2-2.4ghz.md) for the NuPhy and
[docs/unifying.md](docs/unifying.md) for Logitech Unifying.

A release is built when a `v<version>` tag is pushed. Set the version in `tool/Cargo.toml`, which
the firmware reads too, add its section to `CHANGELOG.md`, commit, and push the tag. The release
fails when the tag, the version and the changelog disagree.
