# Logitech Unifying pairing, from the receiver's side

Reference for the first open-dongle Unifying milestone: complete the pairing handshake with a
Unifying keyboard (an MX Keys, original Unifying version) and derive the 16-byte AES link key.
USB output and channel hopping are out of scope here; §5 covers decrypting the keystrokes and
the frames the receiver sends back.

Open-dongle plays the **receiver** (the dongle). In Enhanced ShockBurst terms the keyboard is the
transmitter (PTX) and the receiver is the PRX, so every receiver→keyboard frame is an ESB **ACK
payload** attached to the ACK of a keyboard transmission. This is the same PTX/PRX split the NuPhy
link already uses (`docs/nuphy-v2-2.4ghz.md`).

Every fact below carries a source. Flags:

- **[VERIFIED]** — read directly from source code, agreed by two or more independent sources, or
  cross-checked against a captured frame. **[VERIFIED on air]** marks what open-dongle's Unifying
  build saw or had accepted while pairing an MX Keys (WPID `408A`) and decrypting its keystrokes;
  such a value is sufficient for that keyboard, not proven required by every device.
- **[INFERRED]** — follows from the sources but is not stated outright; usually the receiver side of
  a behavior the sources only show from the keyboard side.
- **[UNVERIFIED]** — one source only, or sources disagree, or the value is a guess a real
  implementation must confirm on air.

## Primary sources

| Tag | What it is | Role |
|---|---|---|
| LOGITacker | `github.com/RoganDawes/LOGITacker`, nRF52840 firmware | Parses pairing frames and derives the key; plays the keyboard side for forced pairing |
| mame82-go | `github.com/mame82/UnifyingVulnsDisclosureRepo` `tools/mjackit/unifying/logitacker.go` | Marcus Mengs' key-derivation and frame-parse reference in Go |
| munifying | `github.com/mame82/munifying` `unifying/*.go` | Host-side USB tool; HID++/DJ constants |
| decrazyo | `github.com/decrazyo/unifying` `src/*` | Clean C reimplementation; plays the keyboard side, pairing complete |
| Newlin | DEF CON 24 "MouseJack: Injecting Keystrokes into Wireless Mice" slides; `BastilleResearch/mousejack` | Radio-layer parameters and addressing |
| dev.to-port | `dev.to/arsenapple/porting-logitech-unifying-to-nrf-connect-sdk-...` | A port to Nordic's nRF Connect SDK ESB; captured Phase-1 frame and ESB gotchas |
| SensePost | `sensepost.com/blog/2019/obtaining-shells-via-logitech-unifying-dongles/` | ACK-payload preload timing |
| Lekensteyn | `lekensteyn.nl/logitech-unifying.html`, `git.lekensteyn.nl/ltunify`, HID++ 1.0 spec PDF | HID++ framing, pairing registers |

A note on direction: LOGITacker, mame82-go and decrazyo all play the **keyboard** (they pair
themselves *to* a real receiver), so their request frames are exactly what open-dongle will
**receive**, and their parse of the responses tells open-dongle what it must **send**. No public
source found builds the receiver's response frames on air, so response-frame filler bytes are the
weakest part of this document and are flagged throughout.

---

## 1. Radio layer for pairing

| Property | Value | Flag | Source |
|---|---|---|---|
| Protocol | Enhanced ShockBurst (ESB), dynamic payload length | [VERIFIED] | Newlin slides; dev.to-port quotes `NRF_ESB_PROTOCOL_ESB_DPL` |
| Data rate | 2 Mbps | [VERIFIED] | Newlin slides; dev.to-port `NRF_ESB_BITRATE_2MBPS` |
| Address width | 5 bytes | [VERIFIED] | Newlin slides; decrazyo `UNIFYING_ADDRESS_LEN = 5` (`unifying_const.h`); LOGITacker promiscuous-frame comment "addr 5 bytes" (`logitacker_radio.c`) |
| CRC | 2 bytes (16-bit) | [VERIFIED] | Newlin slides; dev.to-port `NRF_ESB_CRC_16BIT`; LOGITacker comment "crc16 2 bytes" |
| Preamble | 1 byte (8-bit standard at 2 Mbps) | [VERIFIED] | dev.to-port: tried 8-bit preamble, reverted to standard 16-bit framing default — see note below |
| Auto-ACK | on | [VERIFIED] | Newlin slides ("automatic ACKs") |
| Auto-retransmit | on | [VERIFIED] | Newlin slides ("automatic retransmit"); count/delay values not published — see §6 |
| Max payload | 22 bytes on air (ESB caps at 32) | [VERIFIED] | decrazyo `UNIFYING_MAX_PAYLOAD_LEN = 22`; dev.to-port used 32-byte buffer |

**Pairing address** `BB:0A:DC:A5:75` (MSB first), fixed in the keyboard and the receiver.
[VERIFIED] — three independent sources with identical bytes:
- LOGITacker `UNIFYING_GLOBAL_PAIRING_ADDRESS = {0xbb,0x0a,0xdc,0xa5,0x75}` (`logitacker_unifying.h`)
- decrazyo `unifying_pairing_address = {0xBB,0x0A,0xDC,0xA5,0x75}` (`unifying_const.c`)
- Newlin DEF CON 24 slides list `BB:0A:DC:A5:75` as the fixed pairing-request address.

**Address byte order in Nordic's ESB.** [VERIFIED on air] A 5-byte address `a[0..4]` as printed
here is matched by an ESB base of `{a[3], a[2], a[1], a[0]}` with `a[4]` as the prefix: the pairing
address is base `A5 DC 0A BB`, prefix `0x75`. Neither the address written forward nor all five
bytes reversed matched the MX Keys. The same rule holds for the device and receiver addresses.

**Channels.** ESB channel *n* is 2400 + *n* MHz. [VERIFIED] decrazyo `unifying_const.c`:

- Pairing channels (11): `5, 8, 17, 32, 35, 41, 44, 62, 65, 71, 74`
- Normal channels (25): `5, 8, 11, 14, 17, 20, 23, 26, 29, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65, 68, 71, 74, 77` (step 3)

[INFERRED] The receiver listening for pairing hops the 11 pairing channels; the keyboard also hops
them and camps on whichever one gets an ACK. The dev.to-port observed a real receiver alternating
between channels 44 and 35 in normal operation. The hop interval during pairing is not published —
**[UNVERIFIED]**. [VERIFIED on air] A receiver that steps through the pairing channels every 20 ms
until a request arrives, then stays on that channel, catches the MX Keys; after pairing, and when
it reconnects, the keyboard hops until it finds the receiver, so the receiver can hold one channel.
[VERIFIED on air] After pairing, the MX Keys searches only the 11 pairing channels after an
unacknowledged packet: a receiver moved to 41, 44, 62 or 71 was found within seconds of a key
press, one moved to 38, 50 or 68 never.

**ESB S1 / ACK-request bit (critical ESB-library gotcha).** [VERIFIED] dev.to-port. In raw ESB the
9-bit packet-control field has an S1 bit that carries "no-ack". Logitech transmits with the ACK
requested; Nordic's nRF Connect SDK computes `pdu->type.dpl_pdu.ack = !noack`, which inverts the
sense, and `esb_write_payload` then gets no ACK from a real receiver until the bit is corrected.
Open-dongle is the PRX, so it must honor the keyboard's ACK request and must enable ACK payloads.

**Receiver address vs. serial.** [VERIFIED] (naming) The low (5th) octet of a 5-byte Unifying
address is the device index; `0x00` is the receiver's own pipe and paired devices take `0x01`..;
the factory default device octet is `0x07` and increments per pairing (Newlin slides). The high 4
octets are a base shared by the receiver and its devices. In the key derivation (§4) those 4 base
bytes double as the receiver "serial" — mame82-go names the field `dongleSerial` but copies it from
`RfAddress[0:4]` (`logitacker.go`), and LOGITacker copies the same 4 bytes from the device's assigned
address (`logitacker_unifying_crypto.c`). **[UNVERIFIED]** whether those 4 bytes are literally the
receiver's printed serial or just an internal value; for pairing it does not matter, because the
receiver *chooses* the address it hands out in Response 1.

**Device address formation.** [VERIFIED] The **receiver** assigns the new device's full 5-byte
address and sends it in Pairing Response 1 (§3). The keyboard adopts it verbatim — decrazyo
`unifying_state_address_set(state, pair_response_1.address)` (`unifying.c`). So open-dongle picks the
base and the device octet; it is not derived from anything the keyboard sent. Newlin's "increments
when a new device is paired" is the conventional receiver policy, not a protocol requirement —
**[INFERRED]** for open-dongle's own choice.

> Note on preamble: the dev.to-port's remark concerns on-air framing length at 2 Mbps; the exact
> preamble is handled by the ESB hardware/library and needs no configuration beyond selecting the
> 2 Mbps mode. Treat the "8-bit vs 16-bit" remark as a porting anecdote, not a settable Unifying
> parameter.

---

## 2. Frame format

**Common shape.** A Unifying RF frame is: byte 0 a context byte (see below), byte 1 the frame
type/marker, then type-specific fields, and the **last byte is a checksum**. [VERIFIED] across
LOGITacker (`logitacker_unifying.c` comment "byte 1 is rf report type, byte 0 is device prefix"),
decrazyo structs (every struct ends in `uint8_t checksum`), and the dev.to-port capture.

**Byte 0.** [VERIFIED] In *normal operation* byte 0 is the device index (the low octet of the
address). During *pairing* byte 0 is a **random pairing id the response must echo** — decrazyo field
`id` ("Random value used for verifying the early stage of the pairing process", `unifying_data.h`),
and decrazyo rejects a Response 1 whose `id` does not match the request (`unifying.c`). mame82-go
calls the same byte a sequence number that "responses must echo" (`logitacker.go`). The dev.to-port
capture confirms it: request `E1 5F 01 ...`, receiver reply `E1 1F 01 ...` — the `E1` is echoed.

**Frame-type / report-type byte (byte 1).** [VERIFIED] LOGITacker `logitacker_unifying.h`. The low
5 bits (`& 0x1f`) are the report type; bit `0x40` flags keep-alive; bit `0x80` is "unknown"/vendor.

| Report type (`& 0x1f`) | Name | Value |
|---|---|---|
| Plain keyboard | `UNIFYING_RF_REPORT_PLAIN_KEYBOARD` | `0x01` |
| Plain mouse | `UNIFYING_RF_REPORT_PLAIN_MOUSE` | `0x02` |
| Plain multimedia | `UNIFYING_RF_REPORT_PLAIN_MULTIMEDIA` | `0x03` |
| Plain system control | `UNIFYING_RF_REPORT_PLAIN_SYSTEM_CTL` | `0x04` |
| LED (host→device) | `UNIFYING_RF_REPORT_LED` | `0x0e` |
| Set keep-alive | `UNIFYING_RF_REPORT_SET_KEEP_ALIVE` | `0x0f` |
| HID++ short | `UNIFYING_RF_REPORT_HIDPP_SHORT` | `0x10` |
| HID++ long | `UNIFYING_RF_REPORT_HIDPP_LONG` | `0x11` |
| Encrypted keyboard | `UNIFYING_RF_REPORT_ENCRYPTED_KEYBOARD` | `0x13` |
| Encrypted HID++ long | `UNIFYING_RF_REPORT_ENCRYPTED_HIDPP_LONG` | `0x1b` |
| Pairing | `UNIFYING_RF_REPORT_PAIRING` | `0x1f` |
| (bit) keep-alive | `UNIFYING_RF_REPORT_BIT_KEEP_ALIVE` | `0x40` |
| (bit) unknown | `UNIFYING_RF_REPORT_BIT_UNKNOWN` | `0x80` |

For pairing frames byte 1 is a marker, not a plain report type:

| Direction | Marker (byte 1) | Source |
|---|---|---|
| Keyboard→receiver request | `0x5f` (or `0xDF` = `0x5f \| 0x80`) | LOGITacker `logitacker_pairing_parser.c` (`data[1]==0x5f \|\| 0xDF`); dev.to-port capture `5F` |
| Receiver→keyboard response | `0x1f` | LOGITacker parser (`data[1]==0x1f`); dev.to-port reply `1F` |
| Receiver→keyboard final response | `0x0f` | LOGITacker parser (`length==10 && data[1]==0x0f`); decrazyo `pair_response_3` |

> `0x0f` collides numerically with `SET_KEEP_ALIVE`; the two are told apart by length and by being
> in the pairing exchange vs. normal operation. [VERIFIED] LOGITacker classifies the final response
> only when `length==10`.

**Byte 2 (pairing).** The pairing phase/step. [VERIFIED] `0x01`, `0x02`, `0x03` for request/response
phases 1–3; the **final** response carries step `0x06`, not `0x03` — decrazyo checks
`pair_response_3.step != 6` (`unifying.c`), and mame82-go matches the final on `pay[2]==0x06`
(`logitacker.go`).

**Checksum.** [VERIFIED] The last byte is the negated 8-bit sum of all preceding bytes
(two's complement): `checksum = (0x00 - Σ bytes) & 0xff`.
- LOGITacker `logitacker_unifying_calculate_checksum`: start `0x00`, `checksum -= byte` for each byte
  (`logitacker_unifying.c`).
- mame82-go `LogitechChecksum`: start `0xff`, subtract each byte, then `+1` — algebraically the same
  (`logitacker.go`).
- decrazyo validates with `unifying_checksum_verify` before accepting a frame (`unifying.c`).
- **Validation quirk [VERIFIED]:** LOGITacker accepts a frame if the last byte equals the computed
  checksum **or** the checksum with its low bit cleared (`chksum & 0xfe`), with the comment that the
  last bit is sometimes wrong even on a good payload (`logitacker_unifying.c`). A receiver should be
  lenient on the low bit when validating, and compute it normally when sending.

**Device index / prefix constant byte in Response 1.** LOGITacker splits the assigned address into a
4-byte base at offset 3 and a 1-byte prefix at offset 7 (`RSP1_OFFSET_BASE_ADDR=3`,
`RSP1_OFFSET_ADDR_PREFIX=7`, `logitacker_pairing_parser.h`); decrazyo reads all 5 bytes as one
`address[5]` at offset 3. Same bytes, two framings. [VERIFIED]

---

## 3. Pairing flow end to end (receiver's side)

The exchange is three request/response phases plus a completion, all initiated by the keyboard. The
receiver answers each request; its answer rides in the ACK payload (§6).

**What starts it on an MX Keys** [VERIFIED against a genuine receiver, RQR24.11 firmware, with
Solaar's `pair`]. Holding an Easy-Switch key until its LED blinks fast only makes the keyboard
advertise over Bluetooth LE: with the receiver's lock open, the blinking keyboard paired neither
on a slot already linked to that receiver nor on one that was not. It paired when switched off and
on with the slot blinking and the lock open, so the Unifying pairing request goes out at power-on. A
receiver therefore has to be listening before the power cycle, not merely while the LED blinks. Offsets below are **[VERIFIED]**
from LOGITacker `logitacker_pairing_parser.h`/`.c` and decrazyo `unifying_data.h`, cross-checked
against the dev.to-port capture; receiver-emitted filler bytes are **[UNVERIFIED]** and marked.

### Phase 1 — identify

**Keyboard → receiver: Pairing Request 1** (22 bytes, marker `0x5f`, step `0x01`). open-dongle
receives this.

| Off | Size | Field | Flag | Source |
|---|---|---|---|---|
| 0 | 1 | random pairing id (echo it back) | [VERIFIED] | decrazyo `id`; dev.to capture `E1` |
| 1 | 1 | marker `0x5f` | [VERIFIED] | parser; capture |
| 2 | 1 | step `0x01` | [VERIFIED] | parser; capture |
| 3 | 5 | previously-paired RF address or random | [VERIFIED] | decrazyo `unknown_3_7[5]` "Previously paired RF address or random data" |
| 8 | 1 | keep-alive/timeout hint, observed `0x08` | [UNVERIFIED] | decrazyo `timeout` "Observed as 0x08"; capture `08`; MX Keys `14` |
| 9 | 2 | **device WPID** | [VERIFIED] | `REQ1_OFFSET_DEVICE_WPID=9`; capture `13 37` |
| 11 | 1 | protocol, `0x04` | [VERIFIED] | decrazyo `protocol`; capture `04` |
| 12 | 1 | unknown | [UNVERIFIED] | decrazyo `unknown_12`; capture `00` |
| 13 | 2 | **device type** (keyboard `0x01`) | [VERIFIED] | `REQ1_OFFSET_DEVICE_TYPE=13`; capture `01` (then `05` — see note) |
| 14 | 1 | **device capabilities** | [VERIFIED] | `REQ1_OFFSET_DEVICE_CAPS=14`; capture `05` |
| 15 | 6 | unknown (observed zero, byte 20 non-zero) | [UNVERIFIED] | decrazyo `unknown_15_19`, `unknown_20` |
| 21 | 1 | checksum | [VERIFIED] | capture `A9` |

> LOGITacker reads device type as 1 byte at offset 13 and caps as 1 byte at 14; decrazyo declares
> `uint16_t device_type` at 13–14. The capture `...01 05 00...` fits "type `0x01`, caps `0x05`",
> i.e. device type is effectively one byte. [VERIFIED] the offsets, [UNVERIFIED] the width of
> `device_type`.

**Capabilities byte** [VERIFIED] (munifying, mame82-go):
`0x01` = link encryption supported; `0x04` = Unifying-compatible. The MX Keys sends `0x07`
(both set, plus `0x02`, which no source names) [VERIFIED on air].

**An MX Keys Request 1** [VERIFIED on air]:
`56 5f 01 55 01 97 b1 74 14 40 8a 04 02 01 07 00 00 00 00 02 00 4a` — id `56`, WPID `408A`,
protocol `04`, offset 12 `02`, type `01`, caps `07`, byte 19 `02`. **If the Unifying-compatible bit (`0x04`) is clear the receiver must abort** — munifying
comment "pairing aborts with 'not Unifying compatible'".

**Device types** [VERIFIED] (mame82-go / munifying): keyboard `0x01`, mouse `0x02`, numpad `0x03`,
presenter `0x04`, remote `0x07`, trackball `0x08`, touchpad `0x09`, tablet `0x0a`, gamepad `0x0b`,
joystick `0x0c`.

**Receiver → keyboard: Pairing Response 1** (22 bytes, marker `0x1f`, step `0x01`). open-dongle
sends this; it **assigns the device's address here**.

| Off | Size | Field | Flag | Source |
|---|---|---|---|---|
| 0 | 1 | echoed pairing id | [VERIFIED] | decrazyo rejects on mismatch; capture echoes `E1` |
| 1 | 1 | marker `0x1f` | [VERIFIED] | parser; capture |
| 2 | 1 | step `0x01` | [VERIFIED] | parser; capture |
| 3 | 4 | **assigned RF address base** (key-derivation bytes 0–3) | [VERIFIED] | `RSP1_OFFSET_BASE_ADDR=3`; decrazyo `address[5]` |
| 7 | 1 | **assigned address prefix** (device octet) | [VERIFIED] | `RSP1_OFFSET_ADDR_PREFIX=7` |
| 8 | 1 | unknown, observed `0x08` | [UNVERIFIED] | decrazyo `unknown_8` "Observed as 0x08" |
| 9 | 2 | **dongle WPID** | [VERIFIED] | `RSP1_OFFSET_DONGLE_WPID=9`; munifying `WPID_DONGLE_MSB=0x88, LSB=0x02` |
| 11 | 2 | unknown | [UNVERIFIED] | decrazyo `unknown_11_12` |
| 13 | 2 | device type (echo) | [UNVERIFIED] | decrazyo `device_type` |
| 15 | 6 | unknown | [UNVERIFIED] | decrazyo `unknown_15_20` |
| 21 | 1 | checksum | [VERIFIED] | — |

**A Response 1 the MX Keys accepted** [VERIFIED on air]:
`56 1f 01 26 c1 21 59 07 08 88 02 00 00 01 07 00 00 00 00 00 00 88` — offset 8 `08`, offsets 11–12
zero, offset 13 the device type, offset 14 the device's caps echoed, offsets 15–20 zero.

The receiver's WPID feeds the key (§4). mame82-go uses `0x8802` for a Unifying dongle; real
dongles vary by hardware (munifying notes CU0008/CU0012 report `0x8808`). Open-dongle may pick a
fixed WPID, but **both sides must agree on the same value**, which is guaranteed because the keyboard
takes it from this frame. [VERIFIED]

### Phase 2 — nonces and serial

**Keyboard → receiver: Pairing Request 2** (22 bytes, marker `0x5f`, step `0x02`). [VERIFIED on
air] It arrives on the device address assigned in Response 1, not on the pairing address, and so
does everything the keyboard sends after it.

| Off | Size | Field | Flag | Source |
|---|---|---|---|---|
| 0 | 1 | pairing id | [VERIFIED on air] | decrazyo `unknown_0`; MX Keys repeats Request 1's id |
| 1 | 1 | marker `0x5f` | [VERIFIED] | parser |
| 2 | 1 | step `0x02` | [VERIFIED] | parser |
| 3 | 4 | **device nonce** (key bytes 8–11) | [VERIFIED] | `REQ2_OFFSET_DEVICE_NONCE=3`; decrazyo `crypto` |
| 7 | 4 | device serial | [VERIFIED] | `REQ2_OFFSET_DEVICE_SERIAL=7`; decrazyo `serial` |
| 11 | 4 | report types (LE 32-bit) | [VERIFIED] | `REQ2_OFFSET_DEVICE_REPORT_TYPES_LE=11`; decrazyo `capabilities` at 11 (see note) |
| 15 | 1 | usability info | [VERIFIED] | `REQ2_OFFSET_DEVICE_USABILITY_INFO=15` |
| 16 | 5 | unknown | [UNVERIFIED] | decrazyo `unknown_13_20` |
| 21 | 1 | checksum | [VERIFIED] | — |

> Device **serial** is informational for pairing: it is parsed but **not** used in the key (§4).
> [VERIFIED] — absent from LOGITacker `derive_key_material` and mame82-go `keydata`.
> decrazyo labels offset 11 `capabilities (uint16)` where LOGITacker labels 11–14 `report_types
> (LE32)`; both read bytes starting at 11, so this is a naming disagreement, not an offset one.
> **[UNVERIFIED]** which interpretation the keyboard intends; neither field enters the key.

**Report-type bitfield** [VERIFIED] (munifying): keyboard `1<<1`, mouse `1<<2`, multimedia `1<<3`,
power keys `1<<4`, media center `1<<8`, keyboard LED `1<<14`, short HID++ `1<<16`, long HID++ `1<<17`.

**Receiver → keyboard: Pairing Response 2** (22 bytes, marker `0x1f`, step `0x02`).

| Off | Size | Field | Flag | Source |
|---|---|---|---|---|
| 0 | 1 | pairing id | [VERIFIED on air] | — |
| 1 | 1 | marker `0x1f` | [VERIFIED] | parser |
| 2 | 1 | step `0x02` | [VERIFIED] | parser |
| 3 | 4 | **dongle nonce** (key bytes 12–15) | [VERIFIED] | `RSP2_OFFSET_DONGLE_NONCE=3`; decrazyo `crypto` |
| 7 | 4 | serial (echo) | [VERIFIED on air] | decrazyo `serial` |
| 11 | 2 | capabilities (echo) | [VERIFIED on air] | decrazyo `capabilities` |
| 13 | 8 | unknown | [VERIFIED on air] zero | decrazyo `unknown_13_20` |
| 21 | 1 | checksum | [VERIFIED] | — |

**A Response 2 the MX Keys accepted** [VERIFIED on air]:
`56 1f 02 1d 3d 42 a0 b8 bc d5 b3 1e 40 00 00 00 00 00 00 00 00 f3` — the dongle nonce, then
Request 2's serial (`b8 bc d5 b3`) and its bytes 11–12 echoed, the rest zero.

After phase 2 both sides hold all four key inputs (device WPID, dongle WPID, device nonce, dongle
nonce) plus the address base; **the key can be computed now** — LOGITacker derives it in the
Response-2 handler (`logitacker.go` `FT_PAIRING_RSP_PHASE2`; parser note "key material is complete
when REQ1, REQ2, RSP1, RSP2 have all been received"). [VERIFIED]

### Phase 3 — name

**Keyboard → receiver: Pairing Request 3** (22 bytes, marker `0x5f`, step `0x03`).

| Off | Size | Field | Flag | Source |
|---|---|---|---|---|
| 0 | 1 | `0x00`, **not** the pairing id | [VERIFIED on air] | MX Keys `00 5f 03 01 07 4d 58 …` |
| 1 | 1 | marker `0x5f` | [VERIFIED] | parser |
| 2 | 1 | step `0x03` | [VERIFIED] | parser |
| 3 | 1 | unknown, observed `0x01` | [VERIFIED on air] | decrazyo `unknown_3`; MX Keys `01` |
| 4 | 1 | **name length** (clamp to 16) | [VERIFIED] | `REQ3_OFFSET_DEVICE_NAME_LEN=4` |
| 5 | ≤16 | **device name** (ASCII, no NUL) | [VERIFIED] | `REQ3_OFFSET_DEVICE_NAME=5`; decrazyo `name[16]` |
| 21 | 1 | checksum | [VERIFIED] | — |

**Receiver → keyboard: Pairing Response 3 / final** (10 bytes, marker `0x0f`, step `0x06`).

| Off | Size | Field | Flag | Source |
|---|---|---|---|---|
| 0 | 1 | pairing id (Request 1's) | [VERIFIED on air] | — |
| 1 | 1 | marker `0x0f` | [VERIFIED] | parser (`length==10 && data[1]==0x0f`) |
| 2 | 1 | step `0x06` | [VERIFIED] | decrazyo `step != 6`; mame82-go `pay[2]==0x06` |
| 3 | 6 | echoes phase-2 crypto | [VERIFIED on air] | decrazyo `unknown_3_8` "Observed to contain crypto data from request/response 2"; accepted as device nonce then dongle nonce bytes 0–1 |
| 9 | 1 | checksum | [VERIFIED] | — |

### Completion

**Keyboard → receiver: Pairing Complete Request** (10 bytes). The keyboard sends this after the final
response; the receiver reads it and sends no reply. [VERIFIED] decrazyo `unifying_pair_complete()`
"No response is read for this step" (`unifying.c`).

| Off | Size | Field | Flag | Source |
|---|---|---|---|---|
| 0 | 1 | `0x00` | [VERIFIED on air] | decrazyo `unknown_0` |
| 1 | 1 | marker `0x4f` | [VERIFIED on air] | decrazyo `frame` |
| 2 | 1 | step `0x06` | [VERIFIED on air] | decrazyo `step` |
| 3 | 1 | observed `0x01` | [VERIFIED on air] | decrazyo `unknown_3` |
| 4 | 5 | zero | [VERIFIED on air] | decrazyo `unknown_4_8` |
| 9 | 1 | checksum | [VERIFIED] | — |

The MX Keys sends `00 4f 06 01 00 00 00 00 00 aa`.

### After the handshake: address and channel

- **Address.** The receiver already chose the device's 5-byte address in Response 1; from now the
  keyboard transmits on that address (base + device octet) and the receiver listens there instead of
  the pairing address. [VERIFIED] decrazyo adopts `pair_response_1.address` and leaves pairing mode.
- **The receiver's own address comes first.** [VERIFIED on air] After Pairing Complete, and at
  every reconnect, the MX Keys first sends two frames to the receiver's address, the base with
  octet `0x00`, carrying its device octet in byte 0: `07 51 07 04 00 01 01 01` then 14 zero bytes
  (checksum `9a`), and `07 4f 07 00 00 00 00 00 00 a3`. Only once these are ACKed does it move to
  its device address, opening with `00 51 07 07 00 32 14 …` and continuing with keep-alives. A
  receiver that listens only on the device address hears nothing after pairing.
- **Channel.** No source sets a fixed channel after pairing. [VERIFIED] decrazyo `unifying.c` ("does
  not set a specific channel after pairing"); dev.to-port saw a real receiver on 35/44. Its notion
  that both sides hop the 25 normal channels does not hold for the MX Keys, which searches only the
  pairing channels (see Channels).
- **Device index assignment** (which slot 1–6 the device gets) is the receiver's bookkeeping and is
  reflected in the device octet it handed out. [INFERRED] — Newlin's increment policy.

---

## 4. Key derivation (byte by byte)

[VERIFIED] — **three independent sources agree exactly**: LOGITacker
(`logitacker_unifying_crypto.c` `logitacker_pairing_derive_key_material` + `_derive_key`), mame82-go
(`logitacker.go` `CalculateLinkKey`), and decrazyo (`unifying_const.c` tables +
`unifying_proto.c`). It is **not** AES — it is a byte permutation with fixed XOR masks. AES-ECB
appears only in keystroke encryption (§5), under this key.

### Step A — assemble a 16-byte `keydata` buffer

| `keydata` | Source field | Where it came from |
|---|---|---|
| 0–3 | RF address base (4 bytes) | Response 1 offset 3 |
| 4–5 | device WPID | Request 1 offset 9 |
| 6–7 | dongle WPID | Response 1 offset 9 |
| 8–11 | device nonce | Request 2 offset 3 |
| 12–15 | dongle nonce | Response 2 offset 3 |

Note the address is 4 bytes here though the assigned address is 5 — only the base enters the key.
[VERIFIED] LOGITacker parser note "RSP1 copies 5 bytes, but derive_key_material uses only 4".

### Step B — permute into `key[16]` with XOR masks

For each output index, take `keydata[src]` and XOR the mask:

| key[i] | src (`keydata`) | XOR |
|---|---|---|
| 0 | 7 | — |
| 1 | 1 | `0xFF` |
| 2 | 0 | — |
| 3 | 3 | — |
| 4 | 10 | — |
| 5 | 2 | `0xFF` |
| 6 | 9 | `0x55` |
| 7 | 14 | — |
| 8 | 8 | — |
| 9 | 6 | — |
| 10 | 12 | `0xFF` |
| 11 | 5 | — |
| 12 | 13 | — |
| 13 | 15 | `0x55` |
| 14 | 4 | — |
| 15 | 11 | — |

decrazyo expresses the same thing as two tables (`unifying_const.c`):
- `unifying_aes_key_index = {0x07,0x01,0x00,0x03,0x0A,0x02,0x09,0x0E,0x08,0x06,0x0C,0x05,0x0D,0x0F,0x04,0x0B}`
  — identical to the `src` column.
- `unifying_aes_key_bitmask = {0xFF,0x00,0xFF,0xFF,0xFF,0x00,0xAA,0xFF,0xFF,0xFF,0x00,0xFF,0xFF,0xAA,0xFF,0xFF}`
  — the XNOR mask. `key[i] = keydata[index[i]] XNOR bitmask[i]`, i.e. bits where the mask is 0 are
  flipped. This is algebraically identical to the XOR column: mask `0xFF`→no flip, `0x00`→`^0xFF`,
  `0xAA`→`^0x55`. [VERIFIED] the two formulations match for every index.

That 16-byte `key` is the AES-128 link key. [VERIFIED]

---

## 5. After pairing: keep-alive, keystrokes and what the receiver sends back

### Keep-alive

[VERIFIED] (decrazyo). The keyboard keeps the link up by transmitting at an interval and the receiver
ACKs each transmission:

- **Keep-alive request** (5 bytes, `unifying_keep_alive_request`, `unifying_data.h`): byte 0 device
  index, byte 1 frame marker (keep-alive, carries bit `0x40`), bytes 2–3 timeout (16-bit), byte 4
  checksum. [VERIFIED] layout; [VERIFIED on air] the MX Keys sends marker `0x40`, as in
  `00 40 04 4c 70`.
- **Timing** [VERIFIED]: decrazyo retransmits before the timeout elapses —
  `next = last_tx + timeout * UNIFYING_TIMEOUT_COEFFICIENT`, coefficient `0.875` (`unifying_const.h`).
  Default timeout `20` for keyboards, `8` for mice (`UNIFYING_DEFAULT_TIMEOUT_KEYBOARD/MOUSE`).
  [VERIFIED on air] The MX Keys's timeouts are milliseconds: it settles on `0x044c` (1100), the
  ~1100 ms keep-alive seen in other captures. The receiver side does not need the interval — it only
  has to ACK what arrives and keep an ACK payload ready.
- **Set keep-alive** (report type `0x0f`; `unifying_set_timeout_request`, 10 bytes): bytes 3–4 carry
  a new 16-bit timeout (`unifying_data.h`, `unifying_set_timeout`). Either side can set it; the
  receiver may send it as an ACK payload to configure the keyboard's interval. [VERIFIED] it exists.
  [VERIFIED on air] The MX Keys sets its own (`00 4f 00 00 58 …` for 88 ms, then
  `00 4f 00 04 4c …` for 1100 ms) and sends encrypted keystrokes with the receiver never sending one.

The keyboard treats the link as lost after a timeout with no ACK and will fall back toward pairing/
wake-up. [INFERRED] from the NuPhy link's ~1 s lost-link behavior and decrazyo's timeout logic; the
exact Unifying lost-link timeout is [UNVERIFIED].

### Wake-up

[VERIFIED] (decrazyo `unifying_data.h`, `unifying_connect`). On reconnect the keyboard sends a
**short wake-up request** (10 bytes) whose byte 0 is `address[4]` (its device octet), byte 2 observed
`0x01`, byte 3 observed `0x4B`; there is also a **long wake-up request** (22 bytes). The receiver
should expect a wake-up (not a fresh pairing) from an already-paired keyboard at power-on.
[VERIFIED on air] The MX Keys's reconnect goes to the receiver's own address, as described under
§3 "After the handshake": its 22-byte `07 51 07 04 …` and 10-byte `07 4f 07 00 …` carry the device
octet in byte 0. A receiver whose address base is unchanged gets the keyboard back with no new
pairing.

### Encrypted keyboard frame

[VERIFIED] — layout agreed by LOGITacker (`logitacker_unifying.c`,
`extract_counter_from_encrypted_keyboard_frame`) and decrazyo
(`unifying_encrypted_keystroke_request`, `unifying_data.h`). 22 bytes, report type `0x13`
(`data[1] & 0x1f == 0x13`):

| Off | Size | Field |
|---|---|---|
| 0 | 1 | device index |
| 1 | 1 | frame (type `0x13`, may carry bit `0x40`) |
| 2 | 8 | ciphertext (8 bytes) |
| 10 | 4 | AES counter, **big-endian** 32-bit |
| 14 | 7 | unknown (zero-padding) |
| 21 | 1 | checksum |

[VERIFIED on air] The MX Keys sends these on its device address with byte 0 `0x00`, marker `0xd3`
(type `0x13` with bits `0x40` and `0x80`), zero padding, and a counter that rises by one per frame:
`00 d3 19 3f 57 a9 f9 e9 53 fd 5f 66 f4 59 00 … 91`.

A frame is recognized by length 22, `data[1] & 0x1f == 0x13`, a valid checksum and the counter at
bytes 10–13. [VERIFIED]

### Decrypting a keystroke

The 8 ciphertext bytes are XORed with the first 8 bytes of AES-128-ECB(link key, counter block).
The counter block is a fixed 16-byte template with the 4-byte counter at offset 7. [VERIFIED]
decrazyo `unifying_aes_nonce_prefix = {0x04,0x14,0x1D,0x1F,0x27,0x28,0x0D}` (bytes 0–6) and
`unifying_aes_nonce_suffix = {0x0A,0x0D,0x13,0x26,0x0E}` (bytes 11–15), with the counter at 7–10
(`unifying_const.c`); LOGITacker calls the same template `little_known_secret` and copies the counter
to `[7..10]` (`logitacker_unifying_crypto.c`). The counter is big-endian for Unifying/G700 and
little-endian for Lightspeed (LOGITacker work-mode switch).

[VERIFIED on air] With the link key derived in §4, the MX Keys's frames decrypt to an 8-byte HID
keyboard report:

| Plaintext byte | Content |
|---|---|
| 0 | HID modifier bits |
| 1–6 | HID key codes, `00` for an unused slot |
| 7 | `0xc9`, always |

| Key pressed | Plaintext |
|---|---|
| `a` | `00 04 00 00 00 00 00 c9` |
| Shift+`a` | `02 04 00 00 00 00 00 c9` |
| Left Ctrl | `01 00 00 00 00 00 00 c9` |
| Right Arrow | `00 4f 00 00 00 00 00 c9` |
| All keys up | `00 00 00 00 00 00 00 c9` |

Byte 7 comes out as `0xc9` only under the correct key and counter, so a receiver can use it to
reject forged or corrupted frames.

**Counter.** [VERIFIED on air] It rises by one per frame. The MX Keys picks a new counter on every
keyboard boot (power cycle), and after a new pairing it also starts from an unrelated value. While
the keyboard stays on, the counter continues across a receiver reboot without re-pairing. The
keyboard announces each reconnect with the connect frames on the receiver's own address (§3 "After
the handshake"), so a receiver can forget the counter there. A receiver that keeps a rising floor
across keyboard boots refuses every keystroke whenever the new counter lies below it.

### Media keys

[VERIFIED on air] Media keys arrive unencrypted, as a 10-byte frame on the device address:

| Off | Size | Field |
|---|---|---|
| 0 | 1 | `0x00` |
| 1 | 1 | `0xc3` (report type `0x03` with bits `0x40` and `0x80`) |
| 2 | 1 | HID consumer usage |
| 3 | 6 | zero |
| 9 | 1 | checksum |

Usages seen: volume up `e9`, volume down `ea`, play/pause `cd`. A release is the same frame with
usage `00`. Whether byte 3 is the usage's high byte is [UNVERIFIED]: every key seen used a usage
below `0x100`.

### LED state from the receiver

[VERIFIED on air] The receiver sends the host's LED state as a 10-byte ACK payload on the device
address:

| Off | Size | Field |
|---|---|---|
| 0 | 1 | ignored (`00` and the device octet both work) |
| 1 | 1 | marker `0x0e` |
| 2 | 1 | LED bits, as in the HID LED output report; `0x02` = Caps Lock, which lights the key's LED |
| 3 | 6 | zero |
| 9 | 1 | checksum |

The same frame with marker `0x4e` is ignored. The keyboard acknowledges each accepted LED frame with
a 22-byte `00 51 07 0e 00 <leds> …` on the device address.

### Keys that send no frame

[VERIFIED on air] F1 (brightness down by default on the MX Keys) sent no frame of any type to this
receiver. [UNVERIFIED] Such keys probably need to be enabled by HID++ from the receiver.

---

## 6. Pitfalls a receiver must get right

1. **The receiver's response rides in an ESB ACK payload, not its own transmission.** [VERIFIED]
   (SensePost; decrazyo). The keyboard is PTX; a dongle→keyboard frame is the ACK payload of a
   keyboard packet. SensePost: "due to the tight timing requirements, the response packet has to be
   pre-loaded into the radio." So open-dongle must enable ACK payloads on the PRX and, on receiving
   Pairing Request N, queue Response N as the pending ACK payload. **[INFERRED]** detail: an ESB ACK
   payload is delivered on the *next* ACK, so the response to request N is typically pulled by the
   keyboard's *next* transmission (a keep-alive) — decrazyo's receive path is exactly "transmit, then
   the reply lands in the receive buffer" via a keep-alive or a FIFO payload (`unifying.c`). Build the
   response in the Request-N handler and have it already queued before the keyboard polls again.

2. **ACK-request (S1) bit.** [VERIFIED] (dev.to-port). With Nordic's ESB the ack/no-ack sense is
   easy to invert; if the receiver does not ACK the keyboard's request, pairing never advances. On
   the PRX this means ACK payloads must be enabled and the pipe must auto-ACK.

3. **Checksum.** [VERIFIED]. Compute `(0x00 - Σ preceding bytes) & 0xff` and put it last. When
   validating incoming frames, accept the low bit either way (LOGITacker clears `0x01` before
   comparing) — a keyboard frame can have a wrong low bit on a good payload.

4. **Echo the pairing id (byte 0).** [VERIFIED]. Responses 1–3 must carry the same byte-0 id the
   keyboard sent in its request, or decrazyo-style keyboards reject the response and abort. Confirmed
   by the captured `E1 → E1` echo. Request 3 carries `0x00` in byte 0 instead [VERIFIED on air], so
   a receiver matches it by step, and answers it with Request 1's id.

5. **Match the step bytes, including the `0x06` final.** [VERIFIED]. Response 1 step `0x01`,
   Response 2 step `0x02`, final response step `0x06` (not `0x03`). A wrong step fails the keyboard's
   check (decrazyo `step != 6`).

6. **Honor the Unifying-compatible capability bit.** [VERIFIED]. If Request 1's caps byte lacks
   `0x04`, abort — the keyboard is not Unifying-compatible. The MX Keys sends `0x07` [VERIFIED on air].

7. **Assign a coherent address in Response 1 and keep its base for the key.** [VERIFIED]. The 4-byte
   base you put at offset 3 is both the keyboard's new address base and `keydata[0:3]`. The device
   octet at offset 7 must differ from the receiver's own `0x00` pipe. If the base you send and the
   base you feed the key differ, the derived keys will not match and later decryption fails silently.

8. **Dongle WPID consistency.** [VERIFIED]. The WPID you put in Response 1 (offset 9) is
   `keydata[6:7]`. Pick one value and use the same bytes in both places.

9. **Response filler bytes.** [VERIFIED on air] for the MX Keys: the Response 1, 2 and 3 shown in
   §3 completed pairing, so the observed values are sufficient for it. No public source constructs
   the receiver's responses, so whether any of them is required, or whether another device checks
   more, is [UNVERIFIED].

10. **Listen on the receiver's own address.** [VERIFIED on air]. After pairing and at every
    reconnect the keyboard talks to base + `0x00` before its device address (§3), so the receiver
    needs a pipe there as well as on the pairing and device addresses.

11. **Retransmit timing is unpublished.** [UNVERIFIED]. ESB auto-retransmit count and delay for
    Unifying are not in the sources; the dev.to-port found an ACK-wait of 160 µs too tight in some
    cases and tried 500 µs. The receiver side is less sensitive (it ACKs in hardware), but the ACK
    payload must be ready before the keyboard's retransmit window closes.

---

## Sourcing summary

**Well sourced (safe to build on):**

- Radio layer: 2 Mbps, 5-byte address, 16-bit CRC, ESB-DPL, auto-ACK/retransmit, pairing address
  `BB:0A:DC:A5:75`, and both channel tables — multiple independent sources, identical values.
- Frame shape: byte-0 context, byte-1 marker/report-type table, trailing checksum, the checksum
  algorithm and its low-bit quirk.
- Pairing-request layouts (what open-dongle receives): every parsed field offset is confirmed by two
  C/Go sources and validated against a captured Phase-1 frame.
- Key derivation: confirmed byte-for-byte by three independent implementations; this is the most
  solid part of the document.
- The encrypted-keyboard frame layout, counter position and decryption.
- Everything marked [VERIFIED on air]: the ESB address byte order, the response bytes the MX Keys
  accepted, Request 3's `0x00` id, the connect frames on the receiver's own address, keep-alive
  units, the decrypted keystroke layout, media-key frames and the LED frame.

**Inferred (the receiver side of keyboard-side evidence):**

- That each receiver response rides in an ACK payload pulled by the keyboard's next keep-alive — the
  mechanism is clear from ESB and from decrazyo's receive path, and pairing on air works this way,
  but which keyboard frame pulls each response was not traced.
- The receiver's address-assignment and device-index policy (Newlin's "increment" convention).
- Lost-link / wake-up-vs-repair behavior at power-on.

**Open unknowns:**

1. **Which response filler bytes are required.** The values in §3 are sufficient for the MX Keys;
   whether a device checks any of them is unknown.
2. **The lost-link timeout** for the MX Keys.
3. **Retransmit count/delay and the keyboard's pairing-channel hop interval.**
4. **`device_type` width and the offset-11 field** (report-types LE32 vs. capabilities u16) — a
   naming disagreement between LOGITacker and decrazyo; neither affects the key.
5. **The connect frames' fields** on the receiver's own address (`07 51 07 04 …`, `07 4f 07 …`) and
   the first device-address frame (`00 51 07 07 00 32 14 …`) beyond their observed bytes.
6. **Keys that send no frame.** F1 and similar special keys sent nothing to this receiver; whether
   HID++ from the receiver enables them is untested.
7. **The media usage's high byte.** Whether byte 3 of the media frame carries it is untested,
   since every key seen used a usage below `0x100`.
