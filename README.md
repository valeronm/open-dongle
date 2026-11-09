# nuphy-dongle

Replacement 2.4 GHz dongle for the NuPhy Air75 V2 keyboard, running on a Pro Micro nRF52840 (a nice!nano clone).

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
