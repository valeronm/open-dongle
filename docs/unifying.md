# Logitech Unifying protocol

The keyboard and the receiver talk over Nordic Enhanced ShockBurst (ESB) at 2 Mbps, as on the [NuPhy
link](nuphy-v2-2.4ghz.md). The keyboard is the transmitter (PTX) and the receiver the PRX, so
everything the receiver sends is an ACK payload, attached to the hardware ACK of the keyboard's next
packet. Only the MX Keys (wireless product ID, or WPID, `408A`; the original Unifying version) has
been tested.

## Radio settings

The ESB configuration is `unifying_radio()` in `src/unifying_main.c`. The pairing address and the
channels a paired keyboard searches are in `src/unifying.h`. What the code does not show:

- Nordic's ESB matches a Unifying address only with its first four bytes reversed as the base and
  the fifth as the prefix. Neither the address written forward nor all five bytes reversed matched
  the MX Keys.
- A receiver moved off the pairing channels, to 38, 50 or 68, was never found again.

## Pairing

Frame layouts, the checksum and the key derivation are in `src/unifying.c`.

1. The keyboard sends Request 1 on the pairing address. The receiver answers with Response 1, which
   gives the keyboard its new address and the receiver's WPID.
2. The keyboard sends Request 2 on its new address, carrying its nonce and serial. The receiver
   answers with its own nonce. Both sides then derive the link key.
3. The keyboard sends Request 3 with its name. The receiver answers with the final response.
4. The keyboard sends a completion frame, which gets no reply.

What the code does not show:

- The MX Keys sends Request 1 only at power-on, with a genuine receiver too. Holding an Easy-Switch
  key until it blinks fast makes it advertise over Bluetooth LE on that slot; the Unifying request
  goes out once the keyboard is then switched off and on.
- The filler bytes in the responses are values the MX Keys accepted.

## After pairing

- At every connect, the keyboard first sends two frames to the receiver's own address (device octet
  `0x00`) and moves to the keyboard's address only once they are ACKed.
- The keyboard sets its own keep-alive timeout; the receiver need not send one.
- The keystroke counter rises by one per frame, starts from a new value after a new pairing, and
  survives a receiver restart.
- Media keys arrive unencrypted, one HID consumer usage per frame; a release carries usage 0. Byte 6
  of the frame is `0x34` while Fn is held.
- The keyboard lights its Caps Lock LED from an LED frame with marker `0x0e`, and ignores the same
  frame with marker `0x4e`.
- The MX Keys's top row sends Windows shortcuts and media usages, and F1 to F12 with Fn. Its lock
  key sends Super+L, and it has no power or sleep key.
- Fn and the keyboard's own backlight changes arrive as unencrypted HID++ notifications (type
  `0x51`) that carry no key: Fn as feature index `0x08` with value `0x0034` while held and 0 on
  release, the backlight as feature index `0x0b` with its new level.
- F1 and F2 (screen brightness without Fn) send no frame of any type. On a genuine receiver the MX
  Keys sends such keys only once Solaar diverts them over HID++
  ([Solaar issue #1374](https://github.com/pwr-Solaar/Solaar/issues/1374)).

## Sources

The frame layouts and the key derivation come from public implementations of the keyboard side,
checked against the MX Keys on air:

- [LOGITacker](https://github.com/RoganDawes/LOGITacker): pairing frame offsets and the key
  derivation.
- Marcus Mengs' [UnifyingVulnsDisclosureRepo](https://github.com/mame82/UnifyingVulnsDisclosureRepo)
  and [munifying](https://github.com/mame82/munifying): the key derivation, and the capability and
  device-type values.
- [decrazyo/unifying](https://github.com/decrazyo/unifying): frame structures, channel tables and
  the keystroke encryption.
- Marc Newlin's [MouseJack](https://github.com/BastilleResearch/mousejack) research: radio
  parameters and addressing.

## Open questions

- How long the MX Keys keeps searching for a receiver it lost.
- Which response filler bytes a keyboard checks.
- Which HID++ write from the receiver makes F1 and F2 send frames without Fn.
- What the connect frames' fields mean beyond their observed bytes.
