# nuphy-dongle

Replacement 2.4 GHz dongle for the NuPhy Air75 V2 keyboard, running on a Pro Micro nRF52840 (a nice!nano clone).

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

## Limits

Mouse keys, more than six simultaneous keys, and NuPhy's configuration channel are not supported.

## Building, testing and flashing

Requires nRF Connect SDK v3.4.2 in `/opt/ncs/v3.4.2` with Zephyr SDK 1.0.1 in
`/opt/ncs/zephyr-sdk-1.0.1`, and `adafruit-nrfutil` in `/opt/ncs/nrfutil-venv`. The ESB library
and the USB device stack change their APIs between SDK releases, so other versions are untested.

```
./build.sh build
./build.sh test
./build.sh flash [/dev/ttyACM0]
```

Without this firmware, the board enters its bootloader when RST is shorted to GND twice quickly.
