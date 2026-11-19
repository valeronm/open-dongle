# Working on SpareDongle

## Module boundaries

- `src/nuphy.*`, `src/reports.*`, `src/chanpick.*`, `src/noise.*`, `src/link.*`,
  `src/diagproto.*`, `src/history.*`, `src/protocol.*`, `src/pairwin.*`, `src/unifying.*`,
  `src/chanwatch.*`, `src/hidmap.*`, `src/hidkeys.*`, `src/btpolicy.*`, `src/btlist.*`,
  `src/bootinfo.*`, `src/chanmove.h` and `src/device.h` include only the C standard library and
  each other. Their tests build them with the host compiler, where no Zephyr, nrfx or ESB header
  exists.
- Radio, USB, flash and the clock reach those modules only through `src/core.c`,
  `src/nuphy_main.c`, `src/unifying_main.c`, `src/bluetooth_main.c`, `src/main.c`, `src/diag.c`
  and `src/store.c`.
- `src/radio.c` is the only file that uses the ESB library, and `src/radio.h` must not change if
  it is replaced by a receiver that checks the NuPhy checksum before acknowledging.
- `src/ble.c` is the only file that uses the Bluetooth stack, and one radio stack starts per boot:
  `src/bluetooth_main.c` starts Bluetooth and never starts ESB, and `src/nuphy_main.c` and
  `src/unifying_main.c` start ESB through `src/radio.c`, which first shuts down the Bluetooth
  controller's radio layer.

## Diagnostic protocol

- The firmware and the tool each encode the diagnostic frames, and the hex files in
  `tool/fixtures` are the wire format both test suites check against, so a frame change starts in
  a fixture.
- A wire change bumps `DIAG_VERSION` in `src/diagproto.h` and `VERSION` in `tool/src/protocol.rs`,
  and the version byte in `status.hex` holds both to the same number.

## CHANGELOG.md

- An entry is a change a user can observe: in what the dongle does, in what the tool shows or
  accepts, or in what installing and flashing need. A change nothing observable shows gets none.
- Each entry is one plain sentence saying what changed for the user, not how: no timings, queues,
  thresholds or internal names the user never sees.
- A renamed or removed command, key or option names the old and the new, and a diagnostic protocol
  bump names the tool versions it leaves behind.
- Unreleased describes the state the next release ships: an entry a later change reverses or
  extends is rewritten, not followed by a second one.
