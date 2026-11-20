use anyhow::Result;
pub use diagwire::{
    BUCKETS as NOISE_BUCKETS, BUSY_DBM, BtLink as BluetoothLink, CHUNK, Command, Detail,
    DeviceKind, Divert, EVENT_ADDR_LEN, ErrorCode, Forgotten, LinkState as NuphyState, Moved,
    NoiseChannel, NoiseRecord as Noise, Part, Protocol, REPORT_LEN, Reader, Refused, Rssi,
    STRONG_DBM, Up, Wheel, bucket_floor, channels_arg, parse_part, request,
};
use diagwire::{
    Charging, NAME_MAX, NO_BATTERY, NO_RSSI, NO_SLOT, NUPHY_FIXED_CHANNELS, PAIRING_KNOWN,
    PAIRING_PASSKEY, PAIRING_SECURE, SLOTS, UNIFYING_CHANNELS, VERSION, WatchChannel, Window,
};

/// A status reply in a diagnostic protocol other than [`VERSION`].
#[derive(Debug)]
pub struct VersionMismatch {
    pub dongle: u8,
}

impl std::fmt::Display for VersionMismatch {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        let fix = if self.dongle < VERSION {
            concat!(
                "flash the firmware from this tool's release with `",
                env!("CARGO_BIN_NAME"),
                " flash`"
            )
        } else {
            concat!(
                "install the ",
                env!("CARGO_BIN_NAME"),
                " tool from the firmware's release"
            )
        };
        write!(
            f,
            "the dongle speaks diagnostic protocol {}, this tool {VERSION}; {fix}",
            self.dongle
        )
    }
}

impl std::error::Error for VersionMismatch {}

pub trait ErrorText {
    fn name(self) -> &'static str;
}

impl ErrorText for ErrorCode {
    fn name(self) -> &'static str {
        match self {
            Self::UnknownCommand => "unknown command",
            Self::Radio => "the radio failed",
            Self::BadChannels => "channels not allowed",
            Self::Store => "saving to flash failed",
            Self::WrongProtocol => "not available in the protocol the dongle runs",
            Self::NotMovable => {
                "it moves the link only with a keyboard paired, pairing closed and the host awake"
            }
            Self::KeyHeld => {
                "a key stayed down for a second, and a move then would lose its release: release every key and move again"
            }
            Self::ListFull => {
                concat!(
                    "the dongle already stores as many devices as it can hold: see them with `",
                    env!("CARGO_BIN_NAME"),
                    " devices` and remove one with `",
                    env!("CARGO_BIN_NAME"),
                    " forget`"
                )
            }
            Self::NoSuchDevice => "the dongle has no such device",
        }
    }
}

pub trait ProtocolInfo: Sized {
    /// Whether the dongle reports how each device paired.
    fn reports_pairing(self) -> bool;
    fn counts(self, counter: Counter) -> bool;
    fn counted<T>(self, counter: Counter, value: T) -> Option<T>;
    fn name(self) -> &'static str;
    /// The counters this protocol's firmware counts, in the order status lists them.
    fn counters(self) -> &'static [Counter];
}

impl ProtocolInfo for Protocol {
    fn reports_pairing(self) -> bool {
        self == Self::Bluetooth
    }

    fn counts(self, counter: Counter) -> bool {
        self.counters().contains(&counter)
    }

    fn counted<T>(self, counter: Counter, value: T) -> Option<T> {
        self.counts(counter).then_some(value)
    }

    fn name(self) -> &'static str {
        match self {
            Self::Nuphy => "NuPhy",
            Self::Unifying => "Unifying",
            Self::Bluetooth => "Bluetooth",
        }
    }

    fn counters(self) -> &'static [Counter] {
        use Counter::*;
        match self {
            Self::Nuphy => &[
                Packets, Invalid, Dropped, LinkLosses, Gaps, UsbRetries, Merges, Surveys,
            ],
            Self::Unifying => &[Packets, Invalid, Dropped, UsbRetries, Merges],
            Self::Bluetooth => &[Packets, Dropped, UsbRetries, Merges],
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Counter {
    Packets,
    Invalid,
    Dropped,
    LinkLosses,
    Gaps,
    UsbRetries,
    Merges,
    Surveys,
}

impl Counter {
    pub fn name(self) -> &'static str {
        match self {
            Self::Packets => "packets",
            Self::Invalid => "invalid",
            Self::Dropped => "dropped",
            Self::LinkLosses => "losses",
            Self::Gaps => "gaps",
            Self::UsbRetries => "usb-retries",
            Self::Merges => "merges",
            Self::Surveys => "surveys",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    Reboot,
    Bootloader,
    Clear,
}

impl Action {
    pub fn command(self) -> Command {
        match self {
            Action::Reboot => Command::Reboot,
            Action::Bootloader => Command::Bootloader,
            Action::Clear => Command::Clear,
        }
    }
}

pub(crate) fn read_protocol(code: u8) -> Result<Protocol> {
    Protocol::from_code(code).ok_or_else(|| unknown("protocol"))
}

pub(crate) fn is_reply_to(part: &Part, command: Command, seq: u8) -> bool {
    let (c, s) = match *part {
        Part::Data { command, seq, .. } | Part::Error { command, seq, .. } => (command, seq),
    };
    c == command as u8 && s == seq
}

/// The link-wide counter totals.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counters {
    pub invalid: u32,
    pub dropped: u32,
    pub usb_retries: u32,
    pub merges: u32,
    pub surveys: Option<u32>,
}

impl Counters {
    /// `None` for a counter the dongle's protocol does not count or counts per slot.
    pub fn get(&self, counter: Counter) -> Option<u32> {
        match counter {
            Counter::Invalid => Some(self.invalid),
            Counter::Dropped => Some(self.dropped),
            Counter::UsbRetries => Some(self.usb_retries),
            Counter::Merges => Some(self.merges),
            Counter::Surveys => self.surveys,
            Counter::Packets | Counter::LinkLosses | Counter::Gaps => None,
        }
    }
}

fn read_kind(code: u8) -> Result<DeviceKind> {
    DeviceKind::from_code(code).ok_or_else(|| unknown("device kind"))
}

/// A numbered place for one device, as the status's slot table lists it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Slot {
    pub kind: DeviceKind,
    /// Whether a device is paired for it.
    pub stored: bool,
    /// Whether its device is connected and ready.
    pub connected: bool,
}

/// The slots the tool shows, in slot order: those stored or connected, or slot 0, the one pairing
/// fills first, when there are none.
pub fn shown_slots(slots: &[Slot]) -> Vec<u8> {
    let shown: Vec<u8> = (0..)
        .zip(slots)
        .filter(|(_, slot)| slot.stored || slot.connected)
        .map(|(n, _)| n)
        .collect();
    if shown.is_empty() { vec![0] } else { shown }
}

/// The status of the slot a status request asked for.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SlotStatus {
    pub slot: u8,
    /// Printable ASCII only.
    pub name: Option<String>,
    pub signal: Option<i8>,
    pub last_packet_ms: Option<u32>,
    pub packets: u32,
    pub gaps: Option<u32>,
    pub link_losses: Option<u32>,
    /// `None` before a report from the slot reached the PC since the last clear.
    pub latency: Option<Latency>,
    /// `None` until the slot's connection is encrypted, and under NuPhy and Unifying.
    pub pairing: Option<Pairing>,
    /// `None` until the device reports its battery.
    pub battery: Option<Battery>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Battery {
    /// Percent, with the charging state when the device reports it.
    Level(u8, Option<ChargeState>),
    /// The charging state of a device whose level is unknown.
    Charge(ChargeState),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChargeState {
    Discharging,
    Charging,
    /// On power with the battery full.
    Charged,
    Fault,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Pairing {
    /// LE Secure Connections, not legacy pairing.
    pub secure: bool,
    pub passkey: bool,
}

/// The delay from a report's arrival at the dongle to the PC fetching it over USB.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Latency {
    pub average_us: u32,
    pub max_us: u32,
}

impl SlotStatus {
    /// `None` for a counter the dongle's protocol does not count or counts link-wide.
    pub fn get(&self, counter: Counter) -> Option<u32> {
        match counter {
            Counter::Packets => Some(self.packets),
            Counter::Gaps => self.gaps,
            Counter::LinkLosses => self.link_losses,
            _ => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NuphyLink {
    pub state: NuphyState,
    /// The channels the keyboard got at pairing.
    pub own: Option<[u8; 3]>,
    /// The channels every pairing offers; `None` while pairing surveys.
    pub forced: Option<[u8; 3]>,
    /// Set only from a pairing request since the dongle booted.
    pub keyboard_id: Option<[u8; 4]>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnifyingState {
    Connected,
    Silent,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UnifyingLink {
    pub wpid: [u8; 2],
    pub serial: [u8; 4],
    pub kind: u8,
    pub state: UnifyingState,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Link {
    Nuphy(NuphyLink),
    /// `None` until a keyboard pairs.
    Unifying(Option<UnifyingLink>),
    Bluetooth(BluetoothLink),
}

impl Link {
    pub fn protocol(&self) -> Protocol {
        match self {
            Self::Nuphy(_) => Protocol::Nuphy,
            Self::Unifying(_) => Protocol::Unifying,
            Self::Bluetooth(_) => Protocol::Bluetooth,
        }
    }
}

/// The SoftDevice the bootloader keeps in flash below the firmware.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SoftDevice {
    Absent,
    /// Its information structure is too short to hold the ID.
    Unnamed {
        fwid: u16,
    },
    /// `id` is the number in its name, 140 for S140.
    Named {
        id: u32,
        version: Option<[u32; 3]>,
    },
}

fn softdevice(fwid: u16, id: u32, version: u32) -> SoftDevice {
    match (fwid, id) {
        (0, _) => SoftDevice::Absent,
        (fwid, 0) => SoftDevice::Unnamed { fwid },
        (_, id) => SoftDevice::Named {
            id,
            // Nordic packs a SoftDevice version as major * 1000000 + minor * 1000 + bugfix.
            version: (version != 0).then_some([
                version / 1_000_000,
                version / 1000 % 1000,
                version % 1000,
            ]),
        },
    }
}

/// One status reply: the link-wide fields, the slot table and the asked slot's status.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Status {
    pub firmware_version: [u8; 3],
    /// `None` when no bootloader left its version for the firmware.
    pub bootloader_version: Option<[u8; 3]>,
    pub softdevice: SoftDevice,
    pub uptime_ms: u64,
    pub reset_cause: u32,
    pub channel: u8,
    pub usb_live: bool,
    pub usb_suspended: bool,
    pub counters: Counters,
    pub minute_next: u32,
    pub minute_oldest: u32,
    pub events_total: u32,
    pub events_oldest: u32,
    pub pairing_window: PairingWindow,
    pub link: Link,
    pub slots: Vec<Slot>,
    pub device: SlotStatus,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PairingWindow {
    Closed,
    Open,
    PasskeyAwaited(AwaitedPasskey),
}

/// A keyboard waiting for its passkey to be typed on it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AwaitedPasskey {
    pub passkey: u32,
    /// The name it advertised.
    pub name: Option<String>,
}

impl PairingWindow {
    fn from_wire(window: u8, passkey: u32, name: Option<String>) -> Result<Self> {
        match Window::from_code(window) {
            Some(Window::Closed) => Ok(Self::Closed),
            Some(Window::Open) => Ok(Self::Open),
            Some(Window::PasskeyAwaited) => {
                Ok(Self::PasskeyAwaited(AwaitedPasskey { passkey, name }))
            }
            None => Err(unknown("pairing window")),
        }
    }

    pub fn is_open(&self) -> bool {
        *self != Self::Closed
    }
}

impl Status {
    pub fn protocol(&self) -> Protocol {
        self.link.protocol()
    }

    pub fn signal_shown(&self, device: &SlotStatus) -> bool {
        self.protocol().has_esb() || device.signal.is_some()
    }

    pub fn slot(&self, slot: u8) -> Option<&Slot> {
        self.slots.get(usize::from(slot))
    }

    /// The kind of device the slot holds, "device" for a slot the status does not list.
    pub fn kind_name(&self, slot: u8) -> &'static str {
        self.slot(slot).map_or("device", |s| s.kind.name())
    }

    pub fn connected(&self, slot: u8) -> bool {
        self.slot(slot).is_some_and(|s| s.connected)
    }

    /// The channels a move can send the link to, ascending.
    pub fn keyboard_channels(&self) -> Vec<u8> {
        let mut channels: Vec<u8> = match &self.link {
            Link::Unifying(_) => UNIFYING_CHANNELS.to_vec(),
            Link::Bluetooth(_) => Vec::new(),
            Link::Nuphy(nuphy) => NUPHY_FIXED_CHANNELS
                .into_iter()
                .chain(nuphy.own.into_iter().flatten())
                .collect(),
        };
        channels.sort_unstable();
        channels.dedup();
        channels
    }
}

#[derive(Clone, Copy)]
pub enum Step {
    Up,
    Down,
}

/// The nearest of `channels` (ascending) above or below `from`, wrapping around at either end.
pub fn step_channel(channels: &[u8], from: u8, step: Step) -> Option<u8> {
    match step {
        Step::Up => channels
            .iter()
            .find(|&&c| c > from)
            .or(channels.first())
            .copied(),
        Step::Down => channels
            .iter()
            .rev()
            .find(|&&c| c < from)
            .or(channels.last())
            .copied(),
    }
}

/// Fails on a reply too short for its layout or carrying an unknown [`Protocol`] code; a reply in
/// another diagnostic [`VERSION`] is the inner `Err`.
pub fn read_status(payload: &[u8]) -> Result<Result<Status, VersionMismatch>> {
    if let Some(&dongle) = payload.first()
        && dongle != VERSION
    {
        return Ok(Err(VersionMismatch { dongle }));
    }
    let w = diagwire::Status::decode(payload)?;
    let protocol = read_protocol(w.protocol)?;
    let slots = w.slots[..usize::from(w.slot_count).min(SLOTS)]
        .iter()
        .map(|s| {
            Ok(Slot {
                kind: read_kind(s.kind)?,
                stored: s.stored,
                connected: s.ready,
            })
        })
        .collect::<Result<_>>()?;
    let name = read_name(&w.name);
    Ok(Ok(Status {
        firmware_version: w.firmware_version,
        bootloader_version: Some(w.boot.bootloader).filter(|v| *v != [0; 3]),
        softdevice: softdevice(
            w.boot.softdevice_fwid,
            w.boot.softdevice_id,
            w.boot.softdevice_version,
        ),
        uptime_ms: w.uptime_ms,
        reset_cause: w.boot.reset_cause,
        channel: w.channel,
        usb_live: w.usb_live,
        usb_suspended: w.usb_suspended,
        counters: Counters {
            invalid: w.invalid,
            dropped: w.dropped,
            usb_retries: w.usb_retries,
            merges: w.merges,
            surveys: protocol.counted(Counter::Surveys, w.surveys),
        },
        minute_next: w.minute_next,
        minute_oldest: w.minute_oldest,
        events_total: w.events_total,
        events_oldest: w.events_oldest,
        pairing_window: PairingWindow::from_wire(
            w.pairing_window,
            w.passkey,
            read_name(&w.passkey_name),
        )?,
        link: read_link(&w, protocol)?,
        slots,
        device: SlotStatus {
            slot: w.slot,
            name: match protocol {
                // A NuPhy keyboard sends the name its dongle should show on USB.
                Protocol::Nuphy => {
                    name.map(|name| name.strip_suffix(" Dongle").unwrap_or(&name).into())
                }
                _ => name,
            },
            signal: (w.last_rssi != NO_RSSI).then_some(w.last_rssi),
            last_packet_ms: (w.last_rx_age_ms != u32::MAX).then_some(w.last_rx_age_ms),
            packets: w.packets,
            gaps: protocol.counted(Counter::Gaps, w.gaps),
            link_losses: protocol.counted(Counter::LinkLosses, w.link_losses),
            latency: (w.latency_average_us != u32::MAX).then_some(Latency {
                average_us: w.latency_average_us,
                max_us: w.latency_max_us,
            }),
            pairing: read_pairing(w.pairing),
            battery: read_battery(w.battery)?,
        },
    }))
}

fn read_battery(b: diagwire::Battery) -> Result<Option<Battery>> {
    let charge = match Charging::from_code(b.charging) {
        Some(Charging::Discharging) => Some(ChargeState::Discharging),
        Some(Charging::Charging) => Some(ChargeState::Charging),
        Some(Charging::Charged) => Some(ChargeState::Charged),
        Some(Charging::BatteryFault) => Some(ChargeState::Fault),
        Some(Charging::Unknown) => None,
        None => return Err(unknown("battery state")),
    };
    Ok(match ((b.level != NO_BATTERY).then_some(b.level), charge) {
        (Some(level), charge) => Some(Battery::Level(level, charge)),
        (None, Some(charge)) => Some(Battery::Charge(charge)),
        (None, None) => None,
    })
}

fn read_pairing(flags: u8) -> Option<Pairing> {
    (flags & PAIRING_KNOWN != 0).then_some(Pairing {
        secure: flags & PAIRING_SECURE != 0,
        passkey: flags & PAIRING_PASSKEY != 0,
    })
}

fn unknown(what: &str) -> anyhow::Error {
    anyhow::anyhow!("the dongle reports a {what} this tool does not know")
}

/// Printable ASCII only; `None` for an empty name.
fn read_name(bytes: &[u8; NAME_MAX]) -> Option<String> {
    let name: String = bytes
        .iter()
        .take_while(|&&b| b != 0)
        .map(|&b| {
            if b.is_ascii_graphic() || b == b' ' {
                char::from(b)
            } else {
                '?'
            }
        })
        .collect();
    (!name.is_empty()).then_some(name)
}

/// A device the dongle stores: under Bluetooth a bond, under NuPhy and Unifying the paired
/// keyboard.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StoredDevice {
    /// Under Bluetooth the address type, then the address bytes, least significant first; under
    /// NuPhy the keyboard ID; under Unifying the wireless PID, then the serial.
    pub addr: [u8; 7],
    pub kind: DeviceKind,
    pub name: Option<String>,
    /// The slot it is connected in.
    pub slot: Option<u8>,
    /// `None` under NuPhy and Unifying.
    pub pairing: Option<Pairing>,
}

/// The stored devices; under Bluetooth the most recently connected first.
pub fn read_devices(payload: &[u8]) -> Result<Vec<StoredDevice>> {
    diagwire::decode_devices(payload)?
        .map(|d| {
            let d = d?;
            Ok(StoredDevice {
                addr: d.addr,
                kind: read_kind(d.kind)?,
                name: read_name(&d.name),
                slot: (d.slot != NO_SLOT).then_some(d.slot),
                pairing: read_pairing(d.pairing),
            })
        })
        .collect()
}

fn read_link(w: &diagwire::Status, protocol: Protocol) -> Result<Link> {
    let id = w.keyboard_id;
    let state = || unknown("link state");
    Ok(match protocol {
        Protocol::Bluetooth => {
            Link::Bluetooth(BluetoothLink::from_code(w.link_state).ok_or_else(state)?)
        }
        Protocol::Unifying => {
            let state = match diagwire::UnifyingStatus::from_code(w.link_state).ok_or_else(state)? {
                diagwire::UnifyingStatus::NotPaired => None,
                diagwire::UnifyingStatus::Connected => Some(UnifyingState::Connected),
                diagwire::UnifyingStatus::Silent => Some(UnifyingState::Silent),
            };
            Link::Unifying(state.map(|state| UnifyingLink {
                wpid: [id[0], id[1]],
                serial: w.keyboard_serial,
                kind: w.keyboard_type,
                state,
            }))
        }
        Protocol::Nuphy => Link::Nuphy(NuphyLink {
            state: NuphyState::from_code(w.link_state).ok_or_else(state)?,
            own: present(w.own),
            forced: present(w.forced),
            keyboard_id: present(id),
        }),
    })
}

/// A minute's link-wide counters and the asked slot's own.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Minute {
    pub invalid: u8,
    pub dropped: u8,
    pub usb_retries: u8,
    pub noise: Option<Noise>,
    pub device: SlotMinute,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SlotMinute {
    pub packets: u16,
    pub gaps: Option<u8>,
    pub link_losses: Option<u8>,
    pub rssi: Option<Rssi>,
}

impl Minute {
    /// `None` for a counter the dongle keeps only as a total, counts per slot or its protocol
    /// does not count.
    pub fn get(&self, counter: Counter) -> Option<u16> {
        match counter {
            Counter::Invalid => Some(self.invalid.into()),
            Counter::Dropped => Some(self.dropped.into()),
            Counter::UsbRetries => Some(self.usb_retries.into()),
            _ => None,
        }
    }
}

impl SlotMinute {
    /// `None` for a counter the dongle counts link-wide or its protocol does not count.
    pub fn get(&self, counter: Counter) -> Option<u16> {
        match counter {
            Counter::Packets => Some(self.packets),
            Counter::LinkLosses => self.link_losses.map(Into::into),
            Counter::Gaps => self.gaps.map(Into::into),
            _ => None,
        }
    }
}

/// A reading the dongle had nothing for arrives as all zero bytes.
fn present<T: Default + PartialEq>(v: T) -> Option<T> {
    (v != T::default()).then_some(v)
}

pub fn decode_minutes(payload: &[u8], protocol: Protocol) -> Result<(u32, Vec<Minute>)> {
    let (first, items) = diagwire::decode_minutes(payload)?;
    let minutes = items
        .map(|m| {
            let m = m?;
            Ok(Minute {
                invalid: m.invalid,
                dropped: m.dropped,
                usb_retries: m.usb_retries,
                noise: present(m.noise),
                device: SlotMinute {
                    packets: m.packets,
                    gaps: protocol.counted(Counter::Gaps, m.gaps),
                    link_losses: protocol.counted(Counter::LinkLosses, m.link_losses),
                    rssi: present(m.rssi),
                },
            })
        })
        .collect::<Result<_>>()?;
    Ok((first, minutes))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Event {
    pub time_ms: u64,
    /// `Err` with the type byte of an event this tool does not know.
    pub detail: Result<Detail, u8>,
    /// The slot of the device it is about.
    pub slot: Option<u8>,
    pub noise: Option<Noise>,
}

pub fn decode_noise(payload: &[u8]) -> Result<Vec<NoiseChannel>> {
    Ok(diagwire::decode_noise(payload)?.collect::<Result<_, _>>()?)
}

/// The noise the dongle decides its moves from, read between keyboard packets on the home
/// channel and in short scans on the others, fading over minutes.
#[derive(Clone, Debug, PartialEq)]
pub struct Watch {
    pub home: Option<u8>,
    /// The quietest bucket counted against a channel at the keyboard's current signal; `None`
    /// without a keyboard packet in the current or previous minute.
    pub threshold: Option<u8>,
    /// Whether the dongle holds the link where it is whatever the noise.
    pub paused: bool,
    pub hold_left_ms: u32,
    pub channels: Vec<WatchChannel>,
}

impl Watch {
    pub fn counted(&self, c: &WatchChannel) -> Option<u16> {
        (self.threshold.is_some() && c.readings() > 0).then_some(c.counted)
    }

    /// Home is never moved to.
    pub fn too_few(&self, c: &WatchChannel) -> bool {
        !c.enough && Some(c.channel) != self.home
    }
}

pub fn decode_watch(payload: &[u8]) -> Result<Watch> {
    let (head, channels) = diagwire::decode_watch(payload)?;
    Ok(Watch {
        home: present(head.home),
        threshold: present(head.threshold),
        paused: head.paused,
        hold_left_ms: head.hold_left_ms,
        channels: channels.collect::<Result<_, _>>()?,
    })
}

pub fn decode_events(payload: &[u8]) -> Result<(u32, Vec<Event>)> {
    let (first, items) = diagwire::decode_events(payload)?;
    let events = items
        .map(|e| {
            let e = e?;
            Ok(Event {
                time_ms: e.time_ms,
                detail: Detail::of(&e),
                slot: Some(e.slot).filter(|&slot| slot != NO_SLOT),
                noise: present(e.noise),
            })
        })
        .collect::<Result<_>>()?;
    Ok((first, events))
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    fn fixture_text(name: &str) -> String {
        let path = format!("{}/fixtures/{name}", env!("CARGO_MANIFEST_DIR"));
        std::fs::read_to_string(path).unwrap()
    }

    fn parse_hex(text: &str) -> Vec<u8> {
        text.split_whitespace()
            .map(|b| u8::from_str_radix(b, 16).unwrap())
            .collect()
    }

    pub(crate) fn fixture(name: &str) -> Vec<u8> {
        parse_hex(&fixture_text(name))
    }

    pub(crate) fn awaited(passkey: u32, name: Option<&str>) -> PairingWindow {
        PairingWindow::PasskeyAwaited(AwaitedPasskey {
            passkey,
            name: name.map(str::to_string),
        })
    }

    /// `payload`, a status reply, with `edit` applied to its fields.
    pub(crate) fn edit(payload: &[u8], edit: impl FnOnce(&mut diagwire::Status)) -> Vec<u8> {
        let mut s = diagwire::Status::decode(payload).unwrap();
        edit(&mut s);
        let mut out = [0; diagwire::PAYLOAD_MAX];
        let n = s.encode(&mut out);
        out[..n].to_vec()
    }

    pub(crate) fn edited(name: &str, f: impl FnOnce(&mut diagwire::Status)) -> Vec<u8> {
        edit(&fixture(name), f)
    }

    /// A minutes reply from `first` holding `n` minutes with `packets` from the asked slot.
    pub(crate) fn minutes_page(first: u32, n: usize, packets: u16) -> Vec<u8> {
        let minute = diagwire::Minute {
            packets: [packets; diagwire::SLOTS],
            ..Default::default()
        };
        let mut out = [0; diagwire::PAYLOAD_MAX];
        let len = diagwire::encode_minutes(&mut out, first, &vec![minute; n], 0);
        out[..len].to_vec()
    }

    pub(crate) fn events_page(first: u32, events: &[diagwire::Event]) -> Vec<u8> {
        let mut out = [0; diagwire::PAYLOAD_MAX];
        let len = diagwire::encode_events(&mut out, first, events);
        out[..len].to_vec()
    }

    fn devices_edited(edit: impl FnOnce(&mut [diagwire::Device])) -> Vec<u8> {
        let mut devices: Vec<diagwire::Device> = diagwire::decode_devices(&fixture("devices.hex"))
            .unwrap()
            .collect::<Result<_, _>>()
            .unwrap();
        edit(&mut devices);
        let mut out = [0; diagwire::DEVICES_MAX];
        let len = diagwire::encode_devices(&mut out, &devices);
        out[..len].to_vec()
    }

    fn run(s: &mut diagwire::Status, code: u8) {
        s.protocol = code;
        if code == Protocol::Unifying as u8 {
            s.link_state = diagwire::UnifyingStatus::Connected as u8;
            s.channel = 62;
            s.own = [0; 3];
            s.forced = [0; 3];
        }
        if code == Protocol::Bluetooth as u8 {
            s.link_state = BluetoothLink::NoneReady as u8;
            s.slots[0].ready = false;
        }
    }

    /// A status reply from a dongle running `protocol`, with its pairing window and event count.
    pub(crate) fn status_with_window(
        protocol: Protocol,
        window: PairingWindow,
        events_total: u32,
    ) -> Vec<u8> {
        edited("status.hex", |s| {
            run(s, protocol as u8);
            let (code, passkey, name) = match window {
                PairingWindow::Closed => (Window::Closed, 0, None),
                PairingWindow::Open => (Window::Open, 0, None),
                PairingWindow::PasskeyAwaited(a) => (Window::PasskeyAwaited, a.passkey, a.name),
            };
            s.pairing_window = code as u8;
            s.passkey = passkey;
            s.passkey_name = [0; NAME_MAX];
            if let Some(name) = name {
                s.passkey_name[..name.len()].copy_from_slice(name.as_bytes());
            }
            s.events_total = events_total;
        })
    }

    pub(crate) fn status_running(code: u8) -> Vec<u8> {
        edited("status.hex", |s| run(s, code))
    }

    pub(crate) fn status_of(protocol: Protocol) -> Status {
        read_status(&status_running(protocol as u8))
            .unwrap()
            .unwrap()
    }

    /// A Bluetooth dongle with a keyboard stored and a mouse connected, answering for the mouse,
    /// with pairing closed.
    pub(crate) fn mouse_status() -> Status {
        let mut s = read_status(&fixture("status-slot1.hex")).unwrap().unwrap();
        s.pairing_window = PairingWindow::Closed;
        s
    }

    pub(crate) fn nuphy(s: &mut Status) -> &mut NuphyLink {
        match &mut s.link {
            Link::Nuphy(nuphy) => nuphy,
            _ => panic!("not a NuPhy status"),
        }
    }

    pub(crate) fn unifying(s: &mut Status) -> &mut UnifyingLink {
        match &mut s.link {
            Link::Unifying(Some(unifying)) => unifying,
            _ => panic!("no Unifying keyboard"),
        }
    }

    pub(crate) fn status_in_protocol(version: u8) -> Vec<u8> {
        let mut payload = fixture("status.hex");
        payload[0] = version;
        payload
    }

    pub(crate) fn fixture_lines(name: &str) -> Vec<Vec<u8>> {
        fixture_text(name).lines().map(parse_hex).collect()
    }

    fn decoded(payload: &[u8]) -> Option<Status> {
        read_status(payload).ok()?.ok()
    }

    #[test]
    fn stored_devices_match_fixture() {
        assert_eq!(
            read_devices(&fixture("devices.hex")).ok(),
            Some(vec![
                StoredDevice {
                    addr: [1, 0xd9, 0x9a, 0xa6, 0x49, 0xbb, 0xeb],
                    kind: DeviceKind::Keyboard,
                    name: Some("MX Keys".into()),
                    slot: Some(0),
                    pairing: Some(Pairing {
                        secure: false,
                        passkey: true,
                    }),
                },
                StoredDevice {
                    addr: [1, 0x5a, 0x3c, 0x11, 0xe0, 0xf2, 0xc8],
                    kind: DeviceKind::Mouse,
                    name: Some("MX Master 3S".into()),
                    slot: Some(1),
                    pairing: Some(Pairing {
                        secure: true,
                        passkey: false,
                    }),
                },
            ])
        );
    }

    #[test]
    fn a_stored_device_in_no_slot_reads_as_not_connected() {
        let payload = devices_edited(|d| d[0].slot = diagwire::NO_SLOT);
        let devices = read_devices(&payload).unwrap();
        assert_eq!(devices.first().map(|d| d.slot), Some(None));
    }

    #[test]
    fn a_device_list_cut_short_or_of_an_unknown_kind_is_refused() {
        let payload = devices_edited(|d| d[0].kind = 5);
        assert!(read_devices(&payload).is_err());
        let mut payload = fixture("devices.hex");
        payload.truncate(20);
        assert!(read_devices(&payload).is_err());
        assert_eq!(read_devices(&[0]).unwrap(), []);
    }

    #[test]
    fn a_bluetooth_status_reads_its_link_states() {
        let payload = |state: u8| {
            edit(&status_running(Protocol::Bluetooth as u8), |s| {
                s.link_state = state
            })
        };
        for link in BluetoothLink::ALL {
            assert_eq!(
                decoded(&payload(link as u8)).map(|s| s.link),
                Some(Link::Bluetooth(link))
            );
        }
        assert!(read_status(&payload(BluetoothLink::ALL.len() as u8)).is_err());
    }

    #[test]
    fn a_bluetooth_status_reads_as_bluetooth_with_no_channels() {
        let s = status_of(Protocol::Bluetooth);
        assert_eq!(s.protocol(), Protocol::Bluetooth);
        assert!(s.keyboard_channels().is_empty());
        assert_eq!(Protocol::from_code(3), Some(Protocol::Bluetooth));
        assert_eq!(Protocol::Bluetooth.name(), "Bluetooth");
    }

    #[test]
    fn a_bluetooth_dongle_counts_no_channel_or_link_events() {
        use Counter::*;
        assert_eq!(
            Protocol::Bluetooth.counters(),
            [Packets, Dropped, UsbRetries, Merges]
        );
    }

    #[test]
    fn a_keyboard_listens_on_its_protocols_channels() {
        let mut s = status_of(Protocol::Nuphy);
        nuphy(&mut s).own = Some([30, 26, 44]);
        assert_eq!(s.keyboard_channels(), [16, 22, 26, 30, 44, 48, 58, 72, 78]);
        nuphy(&mut s).own = None;
        assert_eq!(s.keyboard_channels(), NUPHY_FIXED_CHANNELS);
        assert_eq!(
            status_of(Protocol::Unifying).keyboard_channels(),
            UNIFYING_CHANNELS
        );
    }

    #[test]
    fn stepping_wraps_around_the_channels() {
        let channels = [5, 8, 11];
        assert_eq!(step_channel(&channels, 5, Step::Up), Some(8));
        assert_eq!(step_channel(&channels, 11, Step::Up), Some(5));
        assert_eq!(step_channel(&channels, 5, Step::Down), Some(11));
        assert_eq!(step_channel(&channels, 9, Step::Down), Some(8));
        assert_eq!(step_channel(&channels, 9, Step::Up), Some(11));
        assert_eq!(step_channel(&[], 9, Step::Up), None);
    }

    #[test]
    fn an_all_zero_keyboard_id_reads_as_none() {
        let payload = edited("status.hex", |s| s.keyboard_id = [0; 4]);
        let mut s = read_status(&payload).unwrap().unwrap();
        assert_eq!(nuphy(&mut s).keyboard_id, None);
    }

    #[test]
    fn the_reserved_signal_and_age_values_read_as_no_reading() {
        let payload = edited("status.hex", |s| {
            s.last_rssi = diagwire::NO_RSSI;
            s.last_rx_age_ms = u32::MAX;
        });
        let device = decoded(&payload).map(|s| s.device);
        assert_eq!(
            device.map(|d| (d.signal, d.last_packet_ms)),
            Some((None, None))
        );
    }

    #[test]
    fn status_fixture_decodes() {
        assert_eq!(
            decoded(&fixture("status.hex")),
            Some(Status {
                firmware_version: [1, 2, 3],
                bootloader_version: Some([0, 6, 0]),
                softdevice: SoftDevice::Named {
                    id: 140,
                    version: Some([6, 1, 1]),
                },
                uptime_ms: 0x01_0203_0405,
                reset_cause: 1,
                channel: 26,
                usb_live: true,
                usb_suspended: false,
                counters: Counters {
                    invalid: 1,
                    dropped: 2,
                    usb_retries: 5,
                    merges: 6,
                    surveys: Some(7),
                },
                minute_next: 1500,
                minute_oldest: 60,
                events_total: 600,
                events_oldest: 88,
                pairing_window: PairingWindow::Open,
                link: Link::Nuphy(NuphyLink {
                    state: NuphyState::Connected,
                    own: Some([26, 30, 34]),
                    forced: Some([62, 52, 44]),
                    keyboard_id: Some([0x70, 0x15, 0xa3, 0x29]),
                }),
                slots: vec![Slot {
                    kind: DeviceKind::Keyboard,
                    stored: true,
                    connected: true,
                }],
                device: SlotStatus {
                    slot: 0,
                    name: Some("MX Keys".into()),
                    signal: Some(-71),
                    last_packet_ms: Some(12),
                    packets: 1000,
                    gaps: Some(8),
                    link_losses: Some(4),
                    latency: Some(Latency {
                        average_us: 850,
                        max_us: 4200,
                    }),
                    pairing: None,
                    battery: Some(Battery::Level(85, Some(ChargeState::Charging))),
                },
            })
        );
        assert_eq!(
            decoded(&status_running(Protocol::Unifying as u8)).map(|s| s.link),
            Some(Link::Unifying(Some(UnifyingLink {
                wpid: [0x70, 0x15],
                serial: [0xb8, 0xbc, 0xd5, 0xb3],
                kind: 1,
                state: UnifyingState::Connected
            })))
        );
    }

    #[test]
    fn each_charge_code_decodes_to_its_state() {
        for (code, charge) in [
            (0, ChargeState::Discharging),
            (1, ChargeState::Charging),
            (2, ChargeState::Charged),
            (3, ChargeState::Fault),
        ] {
            let payload = edited("status.hex", |s| {
                s.battery = diagwire::Battery {
                    level: 0x40,
                    charging: code,
                }
            });
            assert_eq!(
                decoded(&payload).and_then(|s| s.device.battery),
                Some(Battery::Level(0x40, Some(charge)))
            );
        }
    }

    #[test]
    fn a_battery_charging_with_no_level_decodes_with_its_level_unknown() {
        let payload = edited("status.hex", |s| {
            s.battery = diagwire::Battery {
                level: diagwire::NO_BATTERY,
                charging: Charging::Charging as u8,
            }
        });
        assert_eq!(
            decoded(&payload).and_then(|s| s.device.battery),
            Some(Battery::Charge(ChargeState::Charging))
        );
    }

    #[test]
    fn a_mouse_slot_status_decodes_with_the_slot_table() {
        assert_eq!(
            decoded(&fixture("status-slot1.hex")),
            Some(Status {
                firmware_version: [1, 2, 3],
                bootloader_version: None,
                softdevice: SoftDevice::Absent,
                uptime_ms: 0x01_0203_0405,
                reset_cause: 1,
                channel: 0,
                usb_live: true,
                usb_suspended: false,
                counters: Counters {
                    invalid: 0,
                    dropped: 2,
                    usb_retries: 5,
                    merges: 6,
                    surveys: None,
                },
                minute_next: 1500,
                minute_oldest: 60,
                events_total: 600,
                events_oldest: 88,
                pairing_window: awaited(123_456, Some("MX Keys")),
                link: Link::Bluetooth(BluetoothLink::Ready),
                slots: vec![
                    Slot {
                        kind: DeviceKind::Keyboard,
                        stored: true,
                        connected: false,
                    },
                    Slot {
                        kind: DeviceKind::Mouse,
                        stored: true,
                        connected: true,
                    },
                ],
                device: SlotStatus {
                    slot: 1,
                    name: Some("MX Master 3S".into()),
                    signal: Some(-60),
                    last_packet_ms: Some(250),
                    packets: 2000,
                    gaps: None,
                    link_losses: None,
                    latency: None,
                    pairing: Some(Pairing {
                        secure: true,
                        passkey: false,
                    }),
                    battery: None,
                },
            })
        );
    }

    fn softdevice_with(id: u32, version: u32) -> Option<SoftDevice> {
        let payload = edited("status.hex", |s| {
            s.boot.softdevice_id = id;
            s.boot.softdevice_version = version;
        });
        decoded(&payload).map(|s| s.softdevice)
    }

    #[test]
    fn a_softdevice_without_an_id_is_known_by_its_firmware_id() {
        assert_eq!(
            softdevice_with(0, 0),
            Some(SoftDevice::Unnamed { fwid: 0x00b6 })
        );
    }

    #[test]
    fn a_softdevice_without_a_version_keeps_its_id() {
        assert_eq!(
            softdevice_with(140, 0),
            Some(SoftDevice::Named {
                id: 140,
                version: None
            })
        );
    }

    #[test]
    fn a_softdevice_version_splits_into_major_minor_and_bugfix() {
        assert_eq!(
            softdevice_with(132, 7_003_012),
            Some(SoftDevice::Named {
                id: 132,
                version: Some([7, 3, 12])
            })
        );
    }

    fn slot(kind: DeviceKind, stored: bool, connected: bool) -> Slot {
        Slot {
            kind,
            stored,
            connected,
        }
    }

    use DeviceKind::{Keyboard, Mouse};

    #[test]
    fn a_keyboard_alone_shows_only_its_slot() {
        let slots = [slot(Keyboard, true, false), slot(Mouse, false, false)];
        assert_eq!(shown_slots(&slots), [0]);
    }

    #[test]
    fn a_mouse_alone_shows_only_its_slot() {
        let slots = [slot(Keyboard, false, false), slot(Mouse, true, true)];
        assert_eq!(shown_slots(&slots), [1]);
    }

    #[test]
    fn slots_stored_or_connected_show_in_slot_order() {
        let slots = [slot(Keyboard, false, true), slot(Mouse, true, false)];
        assert_eq!(shown_slots(&slots), [0, 1]);
    }

    #[test]
    fn with_nothing_stored_or_connected_slot_0_shows() {
        let slots = [slot(Keyboard, false, false), slot(Mouse, false, false)];
        assert_eq!(shown_slots(&slots), [0]);
        assert_eq!(shown_slots(&[]), [0]);
    }

    #[test]
    fn a_slot_of_an_unknown_kind_is_refused() {
        let payload = edited("status.hex", |s| s.slots[0].kind = 7);
        assert!(read_status(&payload).is_err());
    }

    #[test]
    fn a_zero_pairing_byte_reads_as_closed() {
        let payload = edited("status.hex", |s| s.pairing_window = Window::Closed as u8);
        assert_eq!(
            read_status(&payload).unwrap().unwrap().pairing_window,
            PairingWindow::Closed
        );
    }

    #[test]
    fn an_unknown_pairing_window_is_refused() {
        let payload = edited("status.hex", |s| s.pairing_window = 3);
        assert!(read_status(&payload).is_err());
    }

    #[test]
    fn an_unknown_link_state_in_status_is_refused() {
        for protocol in Protocol::ALL {
            let payload = edit(&status_running(protocol as u8), |s| s.link_state = 9);
            assert!(read_status(&payload).is_err(), "{protocol:?}");
        }
    }

    #[test]
    fn a_unifying_dongle_has_a_keyboard_unless_its_link_state_is_not_paired() {
        let unifying = status_running(Protocol::Unifying as u8);
        let payload = edit(&unifying, |s| {
            s.link_state = diagwire::UnifyingStatus::NotPaired as u8
        });
        assert_eq!(
            read_status(&payload).unwrap().unwrap().link,
            Link::Unifying(None)
        );
        let payload = edit(&unifying, |s| {
            s.link_state = diagwire::UnifyingStatus::Silent as u8;
            s.keyboard_id = [0; 4];
            s.keyboard_serial = [0; 4];
            s.keyboard_type = 0;
        });
        assert_eq!(
            read_status(&payload).unwrap().unwrap().link,
            Link::Unifying(Some(UnifyingLink {
                wpid: [0, 0],
                serial: [0; 4],
                kind: 0,
                state: UnifyingState::Silent
            }))
        );
    }

    fn device_named(protocol: Protocol, name: &[u8]) -> Option<String> {
        let payload = edit(&status_running(protocol as u8), |s| {
            s.name = [0; NAME_MAX];
            s.name[..name.len()].copy_from_slice(name);
        });
        read_status(&payload).unwrap().unwrap().device.name
    }

    #[test]
    fn a_device_name_keeps_only_printable_ascii() {
        let name = device_named(Protocol::Unifying, b"MX\x1b[2J\xffKeys");
        assert_eq!(name.as_deref(), Some("MX?[2J?Keys"));
    }

    #[test]
    fn a_device_name_can_fill_its_field() {
        let name = device_named(Protocol::Nuphy, b"Twenty-four letters long");
        assert_eq!(name.as_deref(), Some("Twenty-four letters long"));
    }

    #[test]
    fn only_a_nuphy_name_loses_its_dongle_suffix() {
        for (protocol, shown) in [
            (Protocol::Nuphy, "NuPhy Air75 V2"),
            (Protocol::Unifying, "NuPhy Air75 V2 Dongle"),
        ] {
            let name = device_named(protocol, b"NuPhy Air75 V2 Dongle");
            assert_eq!(name.as_deref(), Some(shown));
        }
    }

    #[test]
    fn a_device_without_a_name_has_none() {
        assert_eq!(device_named(Protocol::Unifying, b""), None);
    }

    #[test]
    fn noise_fixture_decodes() {
        let n = decode_noise(&fixture("noise.hex")).unwrap();
        assert_eq!(n.len(), 79);
        assert_eq!(
            n[0],
            NoiseChannel {
                channel: 2,
                loudest: -100,
                average: -100
            }
        );
        assert_eq!(
            n[1],
            NoiseChannel {
                channel: 3,
                loudest: -50,
                average: -88
            }
        );
        assert_eq!(
            n[78],
            NoiseChannel {
                channel: 80,
                loudest: -30,
                average: -30
            }
        );
    }

    #[test]
    fn watch_fixture_decodes() {
        let w = decode_watch(&fixture("watch.hex")).unwrap();
        assert_eq!(w.home, Some(41));
        assert_eq!(w.threshold, Some(2));
        assert!(!w.paused);
        assert_eq!(w.hold_left_ms, 30_000);
        assert_eq!(
            w.channels,
            [
                WatchChannel {
                    channel: 2,
                    enough: true,
                    counted: 0,
                    buckets: [60, 0, 0, 0, 0, 0]
                },
                WatchChannel {
                    channel: 41,
                    enough: true,
                    counted: 291,
                    buckets: [1000, 200, 30, 5, 0, 1]
                },
                WatchChannel {
                    channel: 62,
                    enough: false,
                    counted: 0,
                    buckets: [10, 20, 0, 0, 0, 0]
                },
            ]
        );
    }

    #[test]
    fn a_paused_watch_without_home_or_threshold_decodes_as_none() {
        let w = decode_watch(&[0, 0, 1, 0, 0, 0, 0, 0]).unwrap();
        assert_eq!((w.home, w.threshold, w.paused), (None, None, true));
        assert!(w.channels.is_empty());
    }

    #[test]
    fn a_channel_without_readings_counts_nothing() {
        let mut payload = vec![41, 2, 0, 0, 0, 0, 0, 1, 41, 0, 0, 0];
        payload.extend([0; 4 * NOISE_BUCKETS]);
        let w = decode_watch(&payload).unwrap();
        assert_eq!(w.counted(&w.channels[0]), None);
    }

    fn refusal(version: u8) -> String {
        read_status(&status_in_protocol(version))
            .unwrap()
            .unwrap_err()
            .to_string()
    }

    #[test]
    fn an_older_dongle_is_told_to_take_this_tools_firmware() {
        assert_eq!(
            refusal(4),
            "the dongle speaks diagnostic protocol 4, this tool 5; flash the firmware from this \
             tool's release with `spare-dongle flash`"
        );
    }

    #[test]
    fn a_newer_dongle_is_told_to_take_its_releases_tool() {
        assert_eq!(
            refusal(6),
            "the dongle speaks diagnostic protocol 6, this tool 5; install the spare-dongle tool \
             from the firmware's release"
        );
    }

    #[test]
    fn short_payload_is_refused() {
        assert!(read_status(&fixture("status.hex")[..40]).is_err());
    }

    #[test]
    fn an_unknown_protocol_in_status_is_refused() {
        assert!(read_status(&status_running(7)).is_err());
    }

    #[test]
    fn minutes_fixture_decodes_into_link_wide_and_slot_parts() {
        assert_eq!(
            decode_minutes(&fixture("minutes.hex"), Protocol::Nuphy).ok(),
            Some((
                1440,
                vec![
                    Minute {
                        invalid: 0,
                        dropped: 0,
                        usb_retries: 1,
                        noise: Some(Noise {
                            peak: -41,
                            busy: 102,
                            strong: 5
                        }),
                        device: SlotMinute {
                            packets: 300,
                            gaps: Some(3),
                            link_losses: Some(0),
                            rssi: Some(Rssi {
                                min: -80,
                                avg: -70,
                                max: -60
                            }),
                        },
                    },
                    Minute {
                        invalid: 0,
                        dropped: 0,
                        usb_retries: 0,
                        noise: None,
                        device: SlotMinute {
                            packets: 0,
                            gaps: Some(0),
                            link_losses: Some(1),
                            rssi: None,
                        },
                    },
                ]
            ))
        );
    }

    #[test]
    fn events_fixture_decodes_with_each_events_slot() {
        assert_eq!(
            decode_events(&fixture("events.hex")).ok(),
            Some((
                88,
                vec![
                    Event {
                        time_ms: 5000,
                        detail: Ok(Detail::LinkLost {
                            rssi: -71,
                            since_input_s: 0,
                            gap_ms: 1200,
                        }),
                        slot: Some(0),
                        noise: Some(Noise {
                            peak: -50,
                            busy: 2500,
                            strong: 300
                        })
                    },
                    Event {
                        time_ms: 7000,
                        detail: Ok(Detail::Survey {
                            picked: [26, 30, 34],
                        }),
                        slot: None,
                        noise: None
                    },
                ]
            ))
        );
    }

    #[test]
    fn a_unifying_dongle_has_none_of_the_counters_only_nuphy_counts() {
        let s = status_of(Protocol::Unifying);
        assert_eq!(s.counters.surveys, None);
        assert_eq!((s.device.link_losses, s.device.gaps), (None, None));
        assert_eq!(s.counters.get(Counter::Merges), Some(6));
        let (_, m) = decode_minutes(&fixture("minutes.hex"), Protocol::Unifying).unwrap();
        assert_eq!(
            m.first().map(|m| (m.device.link_losses, m.device.gaps)),
            Some((None, None))
        );
        assert_eq!(m.first().and_then(|m| m.get(Counter::UsbRetries)), Some(1));
    }

    #[test]
    fn padding_after_the_count_is_ignored() {
        let mut payload = fixture("minutes.hex");
        payload.resize(CHUNK, 0);
        assert_eq!(
            decode_minutes(&payload, Protocol::Nuphy).unwrap().1.len(),
            2
        );
    }

    #[test]
    fn a_minute_of_other_devices_packets_has_no_signal() {
        let payload = minutes_page(0, 1, 5);
        let (_, m) = decode_minutes(&payload, Protocol::Nuphy).unwrap();
        assert_eq!(
            m.first()
                .map(|m| (m.device.packets, m.device.rssi, m.noise)),
            Some((5, None, None))
        );
    }
}
