# AGENTS.md

- `src/nuphy.*`, `src/reports.*`, `src/chanpick.*`, `src/noise.*`, `src/link.*`,
  `src/diagproto.*`, `src/history.*`, `src/protocol.*` and `src/unifying.*` include only the C
  standard library and each other.
  Their tests build them with the host compiler, where no Zephyr, nrfx or ESB header exists.
- Radio, USB, flash and the clock reach those modules only through `src/main.c`, or
  `src/unifying_main.c` in the Unifying build.
- `src/radio.c` is the only file that uses the ESB library, and `src/radio.h` must not change if
  it is replaced by a receiver that checks the NuPhy checksum before acknowledging.
