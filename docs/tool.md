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

- `status`: the link, USB state and counter totals, and each device's signal, battery and
  counters.
- `history`: per-minute counters, and each device's packets and signal, oldest first.
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
idle MX Keys sleeps after a few minutes. Each time a Unifying keyboard connects or wakes, an event
records how many keys the dongle diverted over HID++, or what stopped it.

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

## Devices

The dongle reports each device it holds in a numbered slot: NuPhy and Unifying have one, the
keyboard, and Bluetooth has two, a keyboard and a mouse. `status`, `history` and the live view
show every slot that has a device paired or connected, each with its own name, signal, last packet
and counters; the channel, noise, USB and the other link-wide counters are shared. The live view
shows the shared state in one dongle box, and below it each slot's box and chart, side by side
when the terminal is wide enough, and otherwise `o` steps through them. Event lines name the
device they are about.

Each connected device's latency is the delay from a report's arrival at the dongle to the PC
fetching it over USB, as its average and maximum since the last `clear`, or "no report yet" before
the first one. It leaves out the time on air and in the Bluetooth controller, which the dongle
cannot see. Reports the dongle makes up itself, such as the release when a device drops out, are
not counted.

`devices` lists the paired devices with their kind and the slot each is connected in, and
`forget <n>` removes the one at that place in the list. In the live view `d` shows the list in place
of the chart, and a device's number asks to forget it. A NuPhy or Unifying dongle that forgets its
keyboard stops taking its keys and opens pairing until a keyboard pairs.

## Bluetooth mode

The devices hop across all channels, so the dongle has no channel, gaps or moves to show, and
refuses `noise`, `moves`, `move` and `channels`. Its noise is read across the data channels the
devices hop over, leaving out the three advertising channels, where nearby devices advertise all the
time. Each device's signal is its connection's, read from the radio once a second while it is
connected. The events record each device's connects, disconnects with their reason, pairings and
refused pairings, each device forgotten, on request or for a report map it cannot use, and a device
that lost its pairing, which has to be forgotten and paired again.

- `pair` waits for the keyboard to connect, prints the passkey to type on it, and once pairing
  closes says whether the keyboard paired; `pair --no-passkey` opens pairing without one. In the
  live view `a` asks which of the two, and the footer shows the passkey once the keyboard waits for
  it. A passkey serves one attempt, and a failed one closes pairing.
- `devices` lists the stored keyboards and mice, the most recently connected first. The dongle
  stores up to eight, and a full list refuses a new pairing until one is forgotten. The MX Keys and
  the MX Master 3S pair under a new address each time, so pairing one again leaves its old entry to
  forget.
- `devices` and the live view's list give each stored device's pairing, and `status` and the live
  view's device box the connected one's: secure for LE Secure Connections or legacy, and whether a
  passkey was typed. [Bluetooth security](protocols.md#bluetooth-security) says what each way protects
  against and how the live view colors it.

Event lines name a stored device and give a forgotten one's address.

## Firmware and reset

- `flash <file.uf2>` writes firmware to a running dongle, a board in its bootloader, or a new board
  still running its factory program.
- `bootloader` restarts the dongle into its UF2 bootloader, and `reboot` restarts its firmware.
- `clear` zeroes the counters, history and events.
