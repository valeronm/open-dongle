# The spare-dongle tool

`spare-dongle` reads and controls a running dongle over its vendor HID interface while the keyboard
types, and flashes its firmware. `spare-dongle --help` lists the commands and each one's options;
`--serial` picks a board when more than one is plugged in. `noise`, `reboot`, `bootloader`, `flash`,
`clear` and `forget` ask first, and `--yes` skips the question.

## Live view

`spare-dongle` with no arguments shows the link state, signal and counter totals, a chart of the
per-minute history and the latest events, refreshed as they change. Its footer lists the keys: the
arrow keys scroll the chart and the events, `n` takes a noise map and `m` shows the noise that
decides the next channel move, each in place of the chart until `esc`. The view can also open
pairing, step the link to the keyboard's next or previous channel, switch the protocol, restart the
dongle into its firmware or its bootloader, and clear the history.

## Reading the dongle

- `status`: the link, signal, USB state and counter totals.
- `history`: per-minute counters and signal, oldest first.
- `events`: the latest events, oldest first, among them every channel move with the noise on both
  channels.
- `noise`: a noise map of every channel.
- `moves`: the noise on each of the keyboard's channels, which decides the dongle's next move.

The history lives in the dongle's RAM, so it covers only the time since the dongle was last plugged
in, reset or switched to the other protocol. A noise map takes the same 1.5 s noise survey as NuPhy
pairing and stops the keyboard meanwhile, because the radio cannot listen while it measures. Each
minute of history also records the noise on the home channel, the one the dongle listens on, and
each gap or loss event records the noise on the link's channel. Both are read between keyboard
packets. A NuPhy keyboard's home channel is the first of its three. In Unifying mode the minute's
noise is recorded only while pairing is closed, because while pairing is open the dongle steps
across channels. A Unifying keyboard's silence is logged but not counted as a link loss, because an
idle MX Keys sleeps after a few minutes.

## Pairing and channels

- `pair` opens pairing for 30 seconds.
- `move <channel>` moves the link to one of the keyboard's channels, until the dongle moves it again
  or restarts.
- `protocol` shows the protocol the dongle runs; `protocol nuphy`, `unifying` or `bluetooth`
  switches it, and the tool waits for the dongle to come back in the new one.

### Forced channels

`channels <a> <b> <c>` makes every NuPhy pairing offer those three channels instead of the ones the
noise survey picks, and `channels auto` returns to the survey. The dongle keeps them in flash. A
Unifying keyboard's channels are fixed by its firmware, so the command exists only in NuPhy mode.

## Bluetooth mode

The keyboard hops across all channels, so the dongle has no channel, noise, gaps or moves to show,
and refuses `noise`, `moves`, `move` and `channels`. The signal is the connection's, read from the
radio once a second while a keyboard is connected. The events record each keyboard's connects,
disconnects with their reason, pairings and refused pairings, and a keyboard that lost its pairing,
which has to be forgotten and paired again.

- `pair` prints the passkey to type on the keyboard; `pair --no-passkey` pairs a keyboard that has
  no passkey step. In the live view `a` asks which of the two.
- `keyboards` lists the stored keyboards, the most recently connected first, and `forget <n>`
  removes the one at that place in the list. In the live view `k` shows the list in place of the
  chart, and a keyboard's number asks to forget it.

Event lines name a stored keyboard and give a forgotten one's address.

## Firmware and reset

- `flash <file.uf2>` writes firmware to a running dongle, a board in its bootloader, or a new board
  still running its factory program.
- `bootloader` restarts the dongle into its UF2 bootloader, and `reboot` restarts its firmware.
- `clear` zeroes the counters, history and events.
