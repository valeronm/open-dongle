use anyhow::{Result, bail};

pub const REPORT_LEN: usize = 64;
pub const HEADER_LEN: usize = 4;
pub const CHUNK: usize = REPORT_LEN - HEADER_LEN;
pub const ERROR_FLAG: u8 = 0x80;
pub const VERSION: u8 = 4;
/// Equals `DIAG_NO_RSSI` in the firmware's `diagproto.h`.
const NO_RSSI: i8 = i8::MIN;

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Command {
    Status = 1,
    History,
    Events,
    Noise,
    Reboot,
    Bootloader,
    Clear,
    ForceChannels,
    Pair,
    Protocol,
    Move,
    Watch,
    Devices,
    Forget,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ErrorCode {
    UnknownCommand = 1,
    Radio,
    BadChannels,
    Store,
    WrongProtocol,
    NotMovable,
    KeyHeld,
    ListFull,
    NoSuchDevice,
}

impl ErrorCode {
    pub fn from_code(code: u8) -> Option<Self> {
        match code {
            1 => Some(Self::UnknownCommand),
            2 => Some(Self::Radio),
            3 => Some(Self::BadChannels),
            4 => Some(Self::Store),
            5 => Some(Self::WrongProtocol),
            6 => Some(Self::NotMovable),
            7 => Some(Self::KeyHeld),
            8 => Some(Self::ListFull),
            9 => Some(Self::NoSuchDevice),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Protocol {
    Nuphy = 1,
    Unifying = 2,
    Bluetooth = 3,
}

impl Protocol {
    pub const ALL: [Self; 3] = [Self::Nuphy, Self::Unifying, Self::Bluetooth];

    pub fn from_code(code: u8) -> Option<Self> {
        match code {
            1 => Some(Self::Nuphy),
            2 => Some(Self::Unifying),
            3 => Some(Self::Bluetooth),
            _ => None,
        }
    }

    pub fn has_channels(self) -> bool {
        self != Self::Bluetooth
    }

    pub fn counts(self, counter: Counter) -> bool {
        self.counters().contains(&counter)
    }

    fn counted<T>(self, counter: Counter, value: T) -> Option<T> {
        self.counts(counter).then_some(value)
    }

    pub(crate) fn read(r: &mut Reader) -> Result<Self> {
        Self::from_code(r.u8()?).ok_or_else(|| unknown("protocol"))
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Nuphy => "NuPhy",
            Self::Unifying => "Unifying",
            Self::Bluetooth => "Bluetooth",
        }
    }

    /// The counters this protocol's firmware counts, in the order status lists them.
    pub fn counters(self) -> &'static [Counter] {
        use Counter::*;
        match self {
            Self::Nuphy => &[
                Packets,
                Invalid,
                Dropped,
                ChannelChanges,
                LinkLosses,
                Gaps,
                UsbRetries,
                Merges,
                Surveys,
            ],
            Self::Unifying => &[
                Packets,
                Invalid,
                Dropped,
                ChannelChanges,
                UsbRetries,
                Merges,
            ],
            Self::Bluetooth => &[Packets, Dropped, UsbRetries, Merges],
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Counter {
    Packets,
    Invalid,
    Dropped,
    ChannelChanges,
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
            Self::ChannelChanges => "channels",
            Self::LinkLosses => "losses",
            Self::Gaps => "gaps",
            Self::UsbRetries => "usb-retries",
            Self::Merges => "merges",
            Self::Surveys => "surveys",
        }
    }
}

pub fn channels_arg(channels: Option<[u8; 3]>) -> u32 {
    channels.map_or(0, |[a, b, c]| u32::from_le_bytes([a, b, c, 0]))
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

/// `data` follows the argument: the slot for `Command::Status` and `Command::History`, the
/// address for `Command::Forget`.
pub fn request(command: Command, seq: u8, arg: u32, data: &[u8]) -> [u8; REPORT_LEN] {
    let mut r = [0u8; REPORT_LEN];
    r[0] = command as u8;
    r[1] = seq;
    r[2..6].copy_from_slice(&arg.to_le_bytes());
    r[6..6 + data.len()].copy_from_slice(data);
    r
}

#[derive(Debug, PartialEq, Eq)]
pub enum Part<'a> {
    Data {
        command: u8,
        seq: u8,
        index: u8,
        count: u8,
        chunk: &'a [u8],
    },
    Error {
        command: u8,
        seq: u8,
        code: u8,
        detail: u8,
    },
}

impl Part<'_> {
    pub(crate) fn is_reply_to(&self, command: Command, seq: u8) -> bool {
        let (c, s) = match *self {
            Part::Data { command, seq, .. } | Part::Error { command, seq, .. } => (command, seq),
        };
        c == command as u8 && s == seq
    }
}

pub fn parse_part(buf: &[u8]) -> Option<Part<'_>> {
    if buf.len() < REPORT_LEN {
        return None;
    }
    if buf[0] & ERROR_FLAG != 0 {
        return Some(Part::Error {
            command: buf[0] & !ERROR_FLAG,
            seq: buf[1],
            code: buf[4],
            detail: buf[5],
        });
    }
    Some(Part::Data {
        command: buf[0],
        seq: buf[1],
        index: buf[2],
        count: buf[3],
        chunk: &buf[HEADER_LEN..REPORT_LEN],
    })
}

pub(crate) struct Reader<'a> {
    rest: &'a [u8],
}

impl<'a> Reader<'a> {
    pub(crate) fn new(buf: &'a [u8]) -> Self {
        Self { rest: buf }
    }

    pub(crate) fn bytes<const N: usize>(&mut self) -> Result<[u8; N]> {
        let Some((head, rest)) = self.rest.split_first_chunk::<N>() else {
            bail!("reply payload is shorter than its layout");
        };
        self.rest = rest;
        Ok(*head)
    }

    pub(crate) fn u8(&mut self) -> Result<u8> {
        Ok(self.bytes::<1>()?[0])
    }

    pub(crate) fn i8(&mut self) -> Result<i8> {
        Ok(self.u8()? as i8)
    }

    pub(crate) fn u16(&mut self) -> Result<u16> {
        Ok(u16::from_le_bytes(self.bytes()?))
    }

    pub(crate) fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.bytes()?))
    }

    pub(crate) fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_le_bytes(self.bytes()?))
    }
}

/// The link-wide counter totals.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counters {
    pub invalid: u32,
    pub dropped: u32,
    pub channel_changes: u32,
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
            Counter::ChannelChanges => Some(self.channel_changes),
            Counter::UsbRetries => Some(self.usb_retries),
            Counter::Merges => Some(self.merges),
            Counter::Surveys => self.surveys,
            Counter::Packets | Counter::LinkLosses | Counter::Gaps => None,
        }
    }
}

// Equal to the firmware's `DIAG_NAME_MAX`.
const NAME_MAX: usize = 24;

/// The kinds the firmware's `enum device_kind` numbers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeviceKind {
    Keyboard,
    Mouse,
}

impl DeviceKind {
    fn read(r: &mut Reader) -> Result<Self> {
        match r.u8()? {
            0 => Ok(Self::Keyboard),
            1 => Ok(Self::Mouse),
            _ => Err(unknown("device kind")),
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Keyboard => "keyboard",
            Self::Mouse => "mouse",
        }
    }
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

const SLOT_STORED: u8 = 0x01;
const SLOT_READY: u8 = 0x02;
/// Equals `DEVICE_NO_SLOT` in the firmware's `device.h`.
const NO_SLOT: u8 = u8::MAX;
const DEVICE_NO_PASSKEY: u8 = 0x02;

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NuphyState {
    Searching,
    Surveying,
    Pairing,
    PairingConfirmed,
    Connected,
    MovingHome,
}

impl NuphyState {
    fn from_code(code: u8) -> Option<Self> {
        match code {
            0 => Some(Self::Searching),
            1 => Some(Self::Surveying),
            2 => Some(Self::Pairing),
            3 => Some(Self::PairingConfirmed),
            4 => Some(Self::Connected),
            5 => Some(Self::MovingHome),
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

impl UnifyingState {
    fn from_code(code: u8) -> Option<Self> {
        match code {
            1 => Some(Self::Connected),
            2 => Some(Self::Silent),
            _ => None,
        }
    }
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BluetoothLink {
    NothingConnected,
    /// A slot's device is connected and ready.
    Connected,
    StartFailed,
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

/// One status reply: the link-wide fields, the slot table and the asked slot's status.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Status {
    pub firmware_version: [u8; 3],
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
    pub pairing_open: bool,
    pub link: Link,
    pub slots: Vec<Slot>,
    pub device: SlotStatus,
}

/// The channels a NuPhy keyboard looks for its dongle on besides the three it gets at pairing.
pub const NUPHY_FIXED_CHANNELS: [u8; 7] = [16, 22, 44, 48, 58, 72, 78];

/// The channels a Unifying keyboard searches after a failed send, its pairing channels.
pub const UNIFYING_CHANNELS: [u8; 11] = [5, 8, 17, 32, 35, 41, 44, 62, 65, 71, 74];

impl Status {
    pub fn protocol(&self) -> Protocol {
        self.link.protocol()
    }

    pub fn signal_shown(&self, device: &SlotStatus) -> bool {
        self.protocol().has_channels() || device.signal.is_some()
    }

    pub fn slot(&self, slot: u8) -> Option<&Slot> {
        self.slots.get(usize::from(slot))
    }

    /// The kind of device the slot holds, "device" for a slot the status does not list.
    pub fn kind_name(&self, slot: u8) -> &'static str {
        self.slot(slot).map_or("device", |s| s.kind.name())
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
    let mut r = Reader::new(payload);
    let version = r.u8()?;
    if version != VERSION {
        return Ok(Err(VersionMismatch { dongle: version }));
    }
    let firmware_version = r.bytes::<3>()?;
    let uptime_ms = r.u64()?;
    let reset_cause = r.u32()?;
    let link_state = r.u8()?;
    let channel = r.u8()?;
    let own = r.bytes::<3>()?;
    let forced = r.bytes::<3>()?;
    let keyboard_id = r.bytes::<4>()?;
    let usb = r.u8()?;
    let [
        invalid,
        dropped,
        channel_changes,
        usb_retries,
        merges,
        surveys,
    ] = [r.u32()?, r.u32()?, r.u32()?, r.u32()?, r.u32()?, r.u32()?];
    let [minute_next, minute_oldest, events_total, events_oldest] =
        [r.u32()?, r.u32()?, r.u32()?, r.u32()?];
    let protocol = Protocol::read(&mut r)?;
    let counters = Counters {
        invalid,
        dropped,
        channel_changes,
        usb_retries,
        merges,
        surveys: protocol.counted(Counter::Surveys, surveys),
    };
    let pairing_open = r.u8()? != 0;
    let link = read_link(&mut r, protocol, link_state, own, forced, keyboard_id)?;
    let slots = (0..r.u8()?)
        .map(|_| {
            let kind = DeviceKind::read(&mut r)?;
            let flags = r.u8()?;
            Ok(Slot {
                kind,
                stored: flags & SLOT_STORED != 0,
                connected: flags & SLOT_READY != 0,
            })
        })
        .collect::<Result<_>>()?;
    let device = read_slot_status(&mut r, protocol)?;
    Ok(Ok(Status {
        firmware_version,
        uptime_ms,
        reset_cause,
        channel,
        usb_live: usb & 1 != 0,
        usb_suspended: usb & 2 != 0,
        counters,
        minute_next,
        minute_oldest,
        events_total,
        events_oldest,
        pairing_open,
        link,
        slots,
        device,
    }))
}

fn read_slot_status(r: &mut Reader, protocol: Protocol) -> Result<SlotStatus> {
    let slot = r.u8()?;
    let signal = r.i8()?;
    let age = r.u32()?;
    let [packets, gaps, link_losses] = [r.u32()?, r.u32()?, r.u32()?];
    let [average_us, max_us] = [r.u32()?, r.u32()?];
    let name = read_name(r)?;
    Ok(SlotStatus {
        slot,
        name: match protocol {
            // A NuPhy keyboard sends the name its dongle should show on USB.
            Protocol::Nuphy => {
                name.map(|name| name.strip_suffix(" Dongle").unwrap_or(&name).into())
            }
            _ => name,
        },
        signal: (signal != NO_RSSI).then_some(signal),
        last_packet_ms: (age != u32::MAX).then_some(age),
        packets,
        gaps: protocol.counted(Counter::Gaps, gaps),
        link_losses: protocol.counted(Counter::LinkLosses, link_losses),
        latency: (average_us != u32::MAX).then_some(Latency { average_us, max_us }),
    })
}

fn unknown(what: &str) -> anyhow::Error {
    anyhow::anyhow!("the dongle reports a {what} this tool does not know")
}

/// Printable ASCII only; `None` for an empty name.
fn read_name(r: &mut Reader) -> Result<Option<String>> {
    let name: String = r
        .bytes::<NAME_MAX>()?
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
    Ok((!name.is_empty()).then_some(name))
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
    pub no_passkey: bool,
}

/// The stored devices; under Bluetooth the most recently connected first.
pub fn read_devices(payload: &[u8]) -> Result<Vec<StoredDevice>> {
    let mut r = Reader::new(payload);
    (0..r.u8()?)
        .map(|_| {
            let addr = r.bytes::<7>()?;
            let kind = DeviceKind::read(&mut r)?;
            let flags = r.u8()?;
            let slot = r.u8()?;
            Ok(StoredDevice {
                addr,
                kind,
                name: read_name(&mut r)?,
                slot: (slot != NO_SLOT).then_some(slot),
                no_passkey: flags & DEVICE_NO_PASSKEY != 0,
            })
        })
        .collect()
}

fn read_link(
    r: &mut Reader,
    protocol: Protocol,
    state: u8,
    own: [u8; 3],
    forced: [u8; 3],
    id: [u8; 4],
) -> Result<Link> {
    let serial = r.bytes::<4>()?;
    let kind = r.u8()?;
    Ok(match protocol {
        Protocol::Bluetooth => Link::Bluetooth(match state {
            0 => BluetoothLink::NothingConnected,
            1 => BluetoothLink::Connected,
            2 => BluetoothLink::StartFailed,
            _ => return Err(unknown("link state")),
        }),
        Protocol::Unifying => Link::Unifying(match state {
            0 => None,
            state => Some(UnifyingLink {
                wpid: [id[0], id[1]],
                serial,
                kind,
                state: UnifyingState::from_code(state).ok_or_else(|| unknown("link state"))?,
            }),
        }),
        Protocol::Nuphy => Link::Nuphy(NuphyLink {
            state: NuphyState::from_code(state).ok_or_else(|| unknown("link state"))?,
            own: present(own),
            forced: present(forced),
            keyboard_id: present(id),
        }),
    })
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Rssi {
    pub min: i8,
    pub avg: i8,
    pub max: i8,
}

// Equal to the firmware's `NOISE_BUSY_DBM` and `NOISE_STRONG_DBM`.
pub const BUSY_DBM: i8 = -85;
pub const STRONG_DBM: i8 = -60;

/// `busy` and `strong` are shares of the readings in hundredths of a percent.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Noise {
    pub peak: i8,
    pub busy: u16,
    pub strong: u16,
}

/// A minute's link-wide counters and the asked slot's own.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Minute {
    pub invalid: u8,
    pub dropped: u8,
    pub channel_changes: u8,
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
            Counter::ChannelChanges => Some(self.channel_changes.into()),
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

fn read_noise(r: &mut Reader) -> Result<Option<Noise>> {
    Ok(present(Noise {
        peak: r.i8()?,
        busy: r.u16()?,
        strong: r.u16()?,
    }))
}

pub fn decode_minutes(payload: &[u8], protocol: Protocol) -> Result<(u32, Vec<Minute>)> {
    decode_page(payload, |r| {
        let (invalid, dropped, channel_changes, usb_retries) = (r.u8()?, r.u8()?, r.u8()?, r.u8()?);
        let noise = read_noise(r)?;
        let packets = r.u16()?;
        let (gaps, link_losses) = (r.u8()?, r.u8()?);
        let rssi = Rssi {
            min: r.i8()?,
            avg: r.i8()?,
            max: r.i8()?,
        };
        Ok(Minute {
            invalid,
            dropped,
            channel_changes,
            usb_retries,
            noise,
            device: SlotMinute {
                packets,
                gaps: protocol.counted(Counter::Gaps, gaps),
                link_losses: protocol.counted(Counter::LinkLosses, link_losses),
                rssi: present(rssi),
            },
        })
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventKind {
    Boot,
    Paired,
    LinkLost,
    Survey,
    Noise,
    UsbSuspend,
    UsbResume,
    Clear,
    LinkUp,
    LinkMoved,
    LinkGap,
    UnifyingPaired,
    PairingOpened,
    PairingClosed,
    PairingTimeout,
    Reconnect,
    Attack,
    BluetoothConnected,
    BluetoothDisconnected,
    BluetoothPaired,
    BluetoothRefused,
    BluetoothBondLost,
    BluetoothForgotten,
    Unknown(u8),
}

impl From<u8> for EventKind {
    fn from(v: u8) -> Self {
        match v {
            1 => Self::Boot,
            2 => Self::Paired,
            3 => Self::LinkLost,
            4 => Self::Survey,
            5 => Self::Noise,
            6 => Self::UsbSuspend,
            7 => Self::UsbResume,
            8 => Self::Clear,
            9 => Self::LinkUp,
            10 => Self::LinkMoved,
            11 => Self::LinkGap,
            12 => Self::UnifyingPaired,
            13 => Self::PairingOpened,
            14 => Self::PairingClosed,
            15 => Self::PairingTimeout,
            16 => Self::Reconnect,
            17 => Self::Attack,
            18 => Self::BluetoothConnected,
            19 => Self::BluetoothDisconnected,
            20 => Self::BluetoothPaired,
            21 => Self::BluetoothRefused,
            22 => Self::BluetoothBondLost,
            23 => Self::BluetoothForgotten,
            other => Self::Unknown(other),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Event {
    pub time_ms: u64,
    pub kind: EventKind,
    /// The slot of the device it is about.
    pub slot: Option<u8>,
    pub a: u8,
    pub b: u8,
    pub c: u8,
    pub value: u32,
    pub noise: Option<Noise>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NoiseChannel {
    pub channel: u8,
    pub loudest: i8,
    pub average: i8,
}

pub fn decode_noise(payload: &[u8]) -> Result<Vec<NoiseChannel>> {
    let mut r = Reader::new(payload);
    let first = r.u8()?;
    let count = r.u8()?;
    (0..count)
        .map(|i| {
            Ok(NoiseChannel {
                channel: first + i,
                loudest: r.i8()?,
                average: r.i8()?,
            })
        })
        .collect()
}

// Equal to the firmware's `NOISE_BUCKETS` and `NOISE_BUCKET_DB`.
pub const NOISE_BUCKETS: usize = 6;
const BUCKET_DB: i8 = 10;

/// The quietest reading `bucket` holds, or `None` for the bucket below `BUSY_DBM`.
pub fn bucket_floor(bucket: u8) -> Option<i8> {
    let above = i8::try_from(bucket.checked_sub(1)?).ok()?;
    BUSY_DBM.checked_add(above.checked_mul(BUCKET_DB)?)
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WatchChannel {
    pub channel: u8,
    /// Whether the channel has heard enough for the dongle to move to it.
    pub enough: bool,
    /// The share of readings loud enough to count at the keyboard's signal, in hundredths of a
    /// percent; meaningful only with a threshold and readings.
    pub counted: u16,
    pub buckets: [u32; NOISE_BUCKETS],
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

impl WatchChannel {
    pub fn readings(&self) -> u64 {
        self.buckets.iter().map(|&b| u64::from(b)).sum()
    }
}

pub fn decode_watch(payload: &[u8]) -> Result<Watch> {
    let mut r = Reader::new(payload);
    let home = r.u8()?;
    let threshold = r.u8()?;
    let paused = r.u8()?;
    let hold_left_ms = r.u32()?;
    let count = r.u8()?;
    let channels = (0..count)
        .map(|_| {
            let channel = r.u8()?;
            let enough = r.u8()? != 0;
            let counted = r.u16()?;
            let mut buckets = [0; NOISE_BUCKETS];
            for b in &mut buckets {
                *b = r.u32()?;
            }
            Ok(WatchChannel {
                channel,
                enough,
                counted,
                buckets,
            })
        })
        .collect::<Result<_>>()?;
    Ok(Watch {
        home: present(home),
        threshold: present(threshold),
        paused: paused != 0,
        hold_left_ms,
        channels,
    })
}

pub fn decode_events(payload: &[u8]) -> Result<(u32, Vec<Event>)> {
    decode_page(payload, |r| {
        Ok(Event {
            time_ms: r.u64()?,
            kind: r.u8()?.into(),
            slot: Some(r.u8()?).filter(|&slot| slot != NO_SLOT),
            a: r.u8()?,
            b: r.u8()?,
            c: r.u8()?,
            value: r.u32()?,
            noise: read_noise(r)?,
        })
    })
}

fn decode_page<T>(
    payload: &[u8],
    mut item: impl FnMut(&mut Reader) -> Result<T>,
) -> Result<(u32, Vec<T>)> {
    let mut r = Reader::new(payload);
    let first = r.u32()?;
    let count = r.u8()?;
    let items = (0..count).map(|_| item(&mut r)).collect::<Result<_>>()?;
    Ok((first, items))
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    #[test]
    fn only_the_esb_protocols_have_channels() {
        assert!(Protocol::Nuphy.has_channels());
        assert!(Protocol::Unifying.has_channels());
        assert!(!Protocol::Bluetooth.has_channels());
    }

    fn fixture_text(name: &str) -> String {
        let path = format!("{}/fixtures/{name}", env!("CARGO_MANIFEST_DIR"));
        std::fs::read_to_string(path).unwrap()
    }

    fn parse_hex(text: &str) -> Vec<u8> {
        text.split_whitespace()
            .map(|b| u8::from_str_radix(b, 16).unwrap())
            .collect()
    }

    const LINK_STATE_AT: usize = 16;
    const CHANNEL_AT: usize = LINK_STATE_AT + 1;
    const OWN_AT: usize = CHANNEL_AT + 1;
    const KEYBOARD_ID_AT: usize = OWN_AT + 6;
    pub(crate) const PROTOCOL_AT: usize = 69;
    const PAIRING_AT: usize = PROTOCOL_AT + 1;
    const KEYBOARD_AT: usize = PAIRING_AT + 1;
    const SLOTS_AT: usize = KEYBOARD_AT + 5;
    /// Where status.hex, with its one-slot table, holds the asked slot.
    const SLOT_AT: usize = SLOTS_AT + 3;
    /// The asked slot's fields before its name, the slot's number first.
    const SLOT_FIELDS_LEN: usize = 26;
    const NAME_AT: usize = SLOT_AT + SLOT_FIELDS_LEN;
    /// How far before a status reply's end the asked slot's number sits.
    pub(crate) const SLOT_FROM_END: usize = SLOT_FIELDS_LEN + NAME_MAX;

    pub(crate) fn fixture(name: &str) -> Vec<u8> {
        parse_hex(&fixture_text(name))
    }

    pub(crate) fn status_running(code: u8) -> Vec<u8> {
        let mut payload = fixture("status.hex");
        payload[PROTOCOL_AT] = code;
        if code == Protocol::Unifying as u8 {
            payload[LINK_STATE_AT] = 1;
            payload[CHANNEL_AT] = 62;
            payload[OWN_AT..KEYBOARD_ID_AT].fill(0);
        }
        if code == Protocol::Bluetooth as u8 {
            payload[LINK_STATE_AT] = 0;
            payload[SLOTS_AT + 2] = SLOT_STORED;
        }
        payload
    }

    pub(crate) fn status_of(protocol: Protocol) -> Status {
        read_status(&status_running(protocol as u8))
            .unwrap()
            .unwrap()
    }

    /// A Bluetooth dongle with a keyboard stored and a mouse connected, answering for the mouse.
    pub(crate) fn mouse_status() -> Status {
        read_status(&fixture("status-slot1.hex")).unwrap().unwrap()
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
                    no_passkey: false,
                },
                StoredDevice {
                    addr: [1, 0x5a, 0x3c, 0x11, 0xe0, 0xf2, 0xc8],
                    kind: DeviceKind::Mouse,
                    name: Some("MX Master 3S".into()),
                    slot: Some(1),
                    no_passkey: true,
                },
            ])
        );
    }

    #[test]
    fn a_stored_device_in_no_slot_reads_as_not_connected() {
        let mut payload = fixture("devices.hex");
        payload[1 + 9] = 0xff;
        let devices = read_devices(&payload).unwrap();
        assert_eq!(devices.first().map(|d| d.slot), Some(None));
    }

    #[test]
    fn a_device_list_cut_short_or_of_an_unknown_kind_is_refused() {
        let mut payload = fixture("devices.hex");
        payload[1 + 7] = 5;
        assert!(read_devices(&payload).is_err());
        let mut payload = fixture("devices.hex");
        payload.truncate(20);
        assert!(read_devices(&payload).is_err());
        assert_eq!(read_devices(&[0]).unwrap(), []);
    }

    #[test]
    fn a_bluetooth_status_reads_its_link_states() {
        let mut payload = status_running(Protocol::Bluetooth as u8);
        let link = |payload: &[u8]| decoded(payload).map(|s| s.link);
        assert_eq!(
            link(&payload),
            Some(Link::Bluetooth(BluetoothLink::NothingConnected))
        );
        payload[LINK_STATE_AT] = 1;
        assert_eq!(
            link(&payload),
            Some(Link::Bluetooth(BluetoothLink::Connected))
        );
        payload[LINK_STATE_AT] = 2;
        assert_eq!(
            link(&payload),
            Some(Link::Bluetooth(BluetoothLink::StartFailed))
        );
        payload[LINK_STATE_AT] = 3;
        assert!(read_status(&payload).is_err());
    }

    #[test]
    fn keyboard_channel_tables_match_fixtures() {
        assert_eq!(fixture("nuphy-fixed-channels.hex"), NUPHY_FIXED_CHANNELS);
        assert_eq!(fixture("unifying-channels.hex"), UNIFYING_CHANNELS);
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
        let mut payload = fixture("status.hex");
        payload[KEYBOARD_ID_AT..][..4].fill(0);
        let mut s = read_status(&payload).unwrap().unwrap();
        assert_eq!(nuphy(&mut s).keyboard_id, None);
    }

    #[test]
    fn the_reserved_signal_and_age_values_read_as_no_reading() {
        let mut payload = fixture("status.hex");
        payload[SLOT_AT + 1] = 0x80;
        payload[SLOT_AT + 2..SLOT_AT + 6].fill(0xff);
        let device = decoded(&payload).map(|s| s.device);
        assert_eq!(
            device.map(|d| (d.signal, d.last_packet_ms)),
            Some((None, None))
        );
    }

    #[test]
    fn request_carries_command_seq_and_arg() {
        let r = request(Command::History, 9, 0x1234_5678, &[]);
        assert_eq!(&r[..6], &[2, 9, 0x78, 0x56, 0x34, 0x12]);
        assert!(r[6..].iter().all(|&b| b == 0));
    }

    #[test]
    fn a_request_carries_the_slot_after_the_arg() {
        let expected = fixture("request.hex");
        let r = request(Command::History, 9, 1440, &[1]);
        assert_eq!(&r[..expected.len()], expected);
        assert!(r[expected.len()..].iter().all(|&b| b == 0));
    }

    #[test]
    fn status_fixture_decodes() {
        assert_eq!(
            decoded(&fixture("status.hex")),
            Some(Status {
                firmware_version: [1, 2, 3],
                uptime_ms: 0x01_0203_0405,
                reset_cause: 1,
                channel: 26,
                usb_live: true,
                usb_suspended: false,
                counters: Counters {
                    invalid: 1,
                    dropped: 2,
                    channel_changes: 3,
                    usb_retries: 5,
                    merges: 6,
                    surveys: Some(7),
                },
                minute_next: 1500,
                minute_oldest: 60,
                events_total: 600,
                events_oldest: 88,
                pairing_open: true,
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
    fn a_mouse_slot_status_decodes_with_the_slot_table() {
        assert_eq!(
            decoded(&fixture("status-slot1.hex")),
            Some(Status {
                firmware_version: [1, 2, 3],
                uptime_ms: 0x01_0203_0405,
                reset_cause: 1,
                channel: 0,
                usb_live: true,
                usb_suspended: false,
                counters: Counters {
                    invalid: 0,
                    dropped: 2,
                    channel_changes: 0,
                    usb_retries: 5,
                    merges: 6,
                    surveys: None,
                },
                minute_next: 1500,
                minute_oldest: 60,
                events_total: 600,
                events_oldest: 88,
                pairing_open: false,
                link: Link::Bluetooth(BluetoothLink::Connected),
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
                },
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
        let mut payload = fixture("status.hex");
        payload[SLOTS_AT + 1] = 7;
        assert!(read_status(&payload).is_err());
    }

    #[test]
    fn a_zero_pairing_byte_reads_as_closed() {
        let mut payload = fixture("status.hex");
        payload[PAIRING_AT] = 0;
        assert!(!read_status(&payload).unwrap().unwrap().pairing_open);
    }

    #[test]
    fn an_unknown_link_state_in_status_is_refused() {
        for protocol in Protocol::ALL {
            let mut payload = status_running(protocol as u8);
            payload[LINK_STATE_AT] = 9;
            assert!(read_status(&payload).is_err(), "{protocol:?}");
        }
    }

    #[test]
    fn a_unifying_dongle_has_a_keyboard_unless_its_link_state_is_not_paired() {
        let mut payload = status_running(Protocol::Unifying as u8);
        payload[LINK_STATE_AT] = 0;
        assert_eq!(
            read_status(&payload).unwrap().unwrap().link,
            Link::Unifying(None)
        );
        payload[LINK_STATE_AT] = 2;
        payload[KEYBOARD_ID_AT..][..4].fill(0);
        payload[KEYBOARD_AT..SLOTS_AT].fill(0);
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
        let mut payload = status_running(protocol as u8);
        payload[NAME_AT..].fill(0);
        payload[NAME_AT..][..name.len()].copy_from_slice(name);
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
    fn the_status_fixture_ends_with_the_slots_name() {
        assert_eq!(fixture("status.hex").len(), NAME_AT + NAME_MAX);
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

    #[test]
    fn noise_limits_match_fixture() {
        assert_eq!(
            fixture("noise-limits.hex"),
            [BUSY_DBM as u8, STRONG_DBM as u8]
        );
    }

    #[test]
    fn bucket_floors_match_fixture() {
        let floors: Vec<u8> = (1..NOISE_BUCKETS as u8)
            .map(|b| bucket_floor(b).unwrap() as u8)
            .collect();
        assert_eq!(fixture("bucket-floors.hex"), floors);
    }

    #[test]
    fn a_bucket_starts_ten_db_above_the_one_before() {
        assert_eq!(bucket_floor(0), None);
        assert_eq!(bucket_floor(1), Some(BUSY_DBM));
        assert_eq!(bucket_floor(2), Some(-75));
        assert_eq!(bucket_floor(5), Some(-45));
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
            refusal(3),
            "the dongle speaks diagnostic protocol 3, this tool 4; flash the firmware from this \
             tool's release with `spare-dongle flash`"
        );
    }

    #[test]
    fn a_newer_dongle_is_told_to_take_its_releases_tool() {
        assert_eq!(
            refusal(5),
            "the dongle speaks diagnostic protocol 5, this tool 4; install the spare-dongle tool \
             from the firmware's release"
        );
    }

    #[test]
    fn short_payload_is_refused() {
        assert!(read_status(&fixture("status.hex")[..40]).is_err());
    }

    #[test]
    fn error_part_parses() {
        let mut buf = [0u8; REPORT_LEN];
        buf[0] = 0x42 | ERROR_FLAG;
        buf[1] = 5;
        buf[3] = 1;
        buf[4] = 1;
        assert_eq!(
            parse_part(&buf),
            Some(Part::Error {
                command: 0x42,
                seq: 5,
                code: 1,
                detail: 0
            })
        );
    }

    #[test]
    fn an_error_part_carries_its_detail() {
        let mut buf = [0u8; REPORT_LEN];
        buf[..6].copy_from_slice(&[9 | ERROR_FLAG, 4, 0, 1, 5, 2]);
        assert_eq!(
            parse_part(&buf),
            Some(Part::Error {
                command: 9,
                seq: 4,
                code: 5,
                detail: 2
            })
        );
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
                        channel_changes: 0,
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
                        channel_changes: 12,
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
                        kind: EventKind::LinkLost,
                        slot: Some(0),
                        a: 0xb9,
                        b: 0,
                        c: 0,
                        value: 1200,
                        noise: Some(Noise {
                            peak: -50,
                            busy: 2500,
                            strong: 300
                        })
                    },
                    Event {
                        time_ms: 7000,
                        kind: EventKind::Survey,
                        slot: None,
                        a: 26,
                        b: 30,
                        c: 34,
                        value: 0,
                        noise: None
                    },
                ]
            ))
        );
    }

    #[test]
    fn command_numbers_match_fixture() {
        let all = [
            Command::Status,
            Command::History,
            Command::Events,
            Command::Noise,
            Command::Reboot,
            Command::Bootloader,
            Command::Clear,
            Command::ForceChannels,
            Command::Pair,
            Command::Protocol,
            Command::Move,
            Command::Watch,
            Command::Devices,
            Command::Forget,
        ];
        assert_eq!(fixture("commands.hex"), all.map(|c| c as u8));
    }

    #[test]
    fn error_codes_match_fixture() {
        let all = [
            ErrorCode::UnknownCommand,
            ErrorCode::Radio,
            ErrorCode::BadChannels,
            ErrorCode::Store,
            ErrorCode::WrongProtocol,
            ErrorCode::NotMovable,
            ErrorCode::KeyHeld,
            ErrorCode::ListFull,
            ErrorCode::NoSuchDevice,
        ];
        assert_eq!(fixture("error-codes.hex"), all.map(|c| c as u8));
        assert_eq!(all.map(|c| ErrorCode::from_code(c as u8)), all.map(Some));
        assert_eq!(ErrorCode::from_code(0), None);
    }

    #[test]
    fn forced_channels_go_in_the_low_bytes_of_the_arg() {
        assert_eq!(channels_arg(Some([62, 52, 44])), 0x002c_343e);
        assert_eq!(channels_arg(None), 0);
    }

    #[test]
    fn event_type_numbers_match_fixture() {
        let kinds: Vec<EventKind> = fixture("event-types.hex")
            .into_iter()
            .map(EventKind::from)
            .collect();
        assert_eq!(
            kinds,
            [
                EventKind::Boot,
                EventKind::Paired,
                EventKind::LinkLost,
                EventKind::Survey,
                EventKind::Noise,
                EventKind::UsbSuspend,
                EventKind::UsbResume,
                EventKind::Clear,
                EventKind::LinkUp,
                EventKind::LinkMoved,
                EventKind::LinkGap,
                EventKind::UnifyingPaired,
                EventKind::PairingOpened,
                EventKind::PairingClosed,
                EventKind::PairingTimeout,
                EventKind::Reconnect,
                EventKind::Attack,
                EventKind::BluetoothConnected,
                EventKind::BluetoothDisconnected,
                EventKind::BluetoothPaired,
                EventKind::BluetoothRefused,
                EventKind::BluetoothBondLost,
                EventKind::BluetoothForgotten,
            ]
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
        let mut payload = 0u32.to_le_bytes().to_vec();
        payload.push(1);
        payload.extend([0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0]);
        let (_, m) = decode_minutes(&payload, Protocol::Nuphy).unwrap();
        assert_eq!(
            m.first()
                .map(|m| (m.device.packets, m.device.rssi, m.noise)),
            Some((5, None, None))
        );
    }
}
