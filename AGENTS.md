# Working on SpareDongle

## Module boundaries

- `src/nuphy.*`, `src/reports.*`, `src/chanpick.*`, `src/noise.*`, `src/link.*`,
  `src/history.*`, `src/protocol.*`, `src/pairwin.*`, `src/unifying.*`, `src/chanwatch.*`,
  `src/hidmap.*`, `src/hidkeys.*`, `src/hidpp.*`, `src/btpolicy.*`, `src/btlist.*`,
  `src/bootinfo.*`, `src/chanmove.h` and `src/device.h` include only the C standard library, each
  other and the header `ffi` generates. Their tests build them with the host compiler, where no
  Zephyr, nrfx or ESB header exists.
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

- `wire` is the one definition of the protocol's numbers, the records the firmware stores and
  sends, and their byte layout; the firmware reaches it only through `ffi`, whose C header cbindgen
  generates from both crates on every build.
- The hex files in `tool/fixtures` are the wire format as shipped, and `wire`'s tests check its
  encoding against them, so a frame change starts in a fixture.
- A wire change bumps `VERSION` in `wire/src/lib.rs` once per release, and the version byte in
  `status.hex` holds it.
- C sets a `struct diag_detail`'s tag only to a generated `DIAG_DETAIL_*` value: Rust reads any
  other tag as undefined behavior, and no check stands between them.
- `ffi`'s library exports only `diag_*` symbols; the firmware build localizes everything else, so
  an export under another name fails to link.

## CHANGELOG.md

- An entry is a change a user can observe: in what the dongle does, in what the tool shows or
  accepts, or in what installing and flashing need. A change nothing observable shows gets none.
- Each entry is one plain sentence saying what changed for the user, not how: no timings, queues,
  thresholds or internal names the user never sees.
- A renamed or removed command, key or option names the old and the new, and a diagnostic protocol
  bump names the tool versions it leaves behind.
- Unreleased describes the state the next release ships: an entry a later change reverses or
  extends is rewritten, not followed by a second one.
