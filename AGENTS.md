# AGENTS.md

- `src/protocol.*`, `src/reports.*`, `src/chanpick.*` and `src/link.*` include only the C standard
  library. Their tests build them for `native_sim`, where a Zephyr, nrfx or ESB call would not
  exist.
- Radio, USB, flash and the clock reach those modules only through `src/main.c`.
- `src/radio.c` is the only file that uses the ESB library, and `src/radio.h` must not change if
  it is replaced by a receiver that checks the NuPhy checksum before acknowledging.
