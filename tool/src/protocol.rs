use anyhow::{Result, bail};

pub const REPORT_LEN: usize = 64;
pub const HEADER_LEN: usize = 4;
pub const CHUNK: usize = REPORT_LEN - HEADER_LEN;
pub const ERROR_FLAG: u8 = 0x80;
pub const VERSION: u8 = 2;

/// A status reply in a diagnostic protocol other than [`VERSION`].
#[derive(Debug)]
pub struct VersionMismatch {
    pub dongle: u8,
}

impl std::fmt::Display for VersionMismatch {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        let fix = if self.dongle < VERSION {
            "flash the firmware from this tool's release with `open-dongle flash`"
        } else {
            "install the open-dongle tool from the firmware's release"
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

    pub fn has_channel_and_signal(self) -> bool {
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

pub fn request(command: Command, seq: u8, arg: u32) -> [u8; REPORT_LEN] {
    let mut r = [0u8; REPORT_LEN];
    r[0] = command as u8;
    r[1] = seq;
    r[2..6].copy_from_slice(&arg.to_le_bytes());
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

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counters {
    pub packets: u32,
    pub invalid: u32,
    pub dropped: u32,
    pub channel_changes: u32,
    pub link_losses: Option<u32>,
    pub usb_retries: u32,
    pub gaps: Option<u32>,
    pub merges: u32,
    pub surveys: Option<u32>,
}

impl Counters {
    /// `None` for a counter the dongle's protocol does not count.
    pub fn get(&self, counter: Counter) -> Option<u32> {
        match counter {
            Counter::Packets => Some(self.packets),
            Counter::Invalid => Some(self.invalid),
            Counter::Dropped => Some(self.dropped),
            Counter::ChannelChanges => Some(self.channel_changes),
            Counter::LinkLosses => self.link_losses,
            Counter::Gaps => self.gaps,
            Counter::UsbRetries => Some(self.usb_retries),
            Counter::Merges => Some(self.merges),
            Counter::Surveys => self.surveys,
        }
    }
}

// Equal to the firmware's `DIAG_NAME_MAX`.
const NAME_MAX: usize = 24;

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
    /// Set once the keyboard has sent it since the dongle booted.
    pub keyboard_name: Option<String>,
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
    pub name: Option<String>,
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

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BluetoothLink {
    NoKeyboard,
    Connected { name: Option<String> },
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

    /// Printable ASCII only.
    pub fn keyboard_name(&self) -> Option<&str> {
        match self {
            Self::Nuphy(nuphy) => nuphy.keyboard_name.as_deref(),
            Self::Unifying(unifying) => unifying.as_ref()?.name.as_deref(),
            Self::Bluetooth(BluetoothLink::Connected { name }) => name.as_deref(),
            Self::Bluetooth(_) => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Status {
    pub firmware_version: [u8; 3],
    pub uptime_ms: u64,
    pub reset_cause: u32,
    pub channel: u8,
    pub last_rssi: i8,
    pub last_rx_age_ms: Option<u32>,
    pub usb_live: bool,
    pub usb_suspended: bool,
    pub counters: Counters,
    pub minute_next: u32,
    pub minute_oldest: u32,
    pub events_total: u32,
    pub events_oldest: u32,
    pub pairing_open: bool,
    pub link: Link,
}

/// The channels a NuPhy keyboard looks for its dongle on besides the three it gets at pairing.
pub const NUPHY_FIXED_CHANNELS: [u8; 7] = [16, 22, 44, 48, 58, 72, 78];

/// The channels a Unifying keyboard searches after a failed send, its pairing channels.
pub const UNIFYING_CHANNELS: [u8; 11] = [5, 8, 17, 32, 35, 41, 44, 62, 65, 71, 74];

impl Status {
    pub fn protocol(&self) -> Protocol {
        self.link.protocol()
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
    let last_rssi = r.i8()?;
    let age = r.u32()?;
    let usb = r.u8()?;
    let [
        packets,
        invalid,
        dropped,
        channel_changes,
        link_losses,
        usb_retries,
        gaps,
        merges,
        surveys,
    ] = [
        r.u32()?,
        r.u32()?,
        r.u32()?,
        r.u32()?,
        r.u32()?,
        r.u32()?,
        r.u32()?,
        r.u32()?,
        r.u32()?,
    ];
    let [minute_next, minute_oldest, events_total, events_oldest] =
        [r.u32()?, r.u32()?, r.u32()?, r.u32()?];
    let protocol = Protocol::read(&mut r)?;
    let counters = Counters {
        packets,
        invalid,
        dropped,
        channel_changes,
        link_losses: protocol.counted(Counter::LinkLosses, link_losses),
        usb_retries,
        gaps: protocol.counted(Counter::Gaps, gaps),
        merges,
        surveys: protocol.counted(Counter::Surveys, surveys),
    };
    let pairing_open = r.u8()? != 0;
    let link = read_link(&mut r, protocol, link_state, own, forced, keyboard_id)?;
    Ok(Ok(Status {
        firmware_version,
        uptime_ms,
        reset_cause,
        channel,
        last_rssi,
        last_rx_age_ms: (age != u32::MAX).then_some(age),
        usb_live: usb & 1 != 0,
        usb_suspended: usb & 2 != 0,
        counters,
        minute_next,
        minute_oldest,
        events_total,
        events_oldest,
        pairing_open,
        link,
    }))
}

fn unknown(what: &str) -> anyhow::Error {
    anyhow::anyhow!("the dongle reports a {what} this tool does not know")
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
    let name = r.bytes::<NAME_MAX>()?;
    let name: String = name
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
    let name = (!name.is_empty()).then_some(name);
    Ok(match protocol {
        Protocol::Bluetooth => Link::Bluetooth(match state {
            0 => BluetoothLink::NoKeyboard,
            1 => BluetoothLink::Connected { name },
            2 => BluetoothLink::StartFailed,
            _ => return Err(unknown("link state")),
        }),
        Protocol::Unifying => Link::Unifying(match state {
            0 => None,
            state => Some(UnifyingLink {
                wpid: [id[0], id[1]],
                name,
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
            // A NuPhy keyboard sends the name its dongle should show on USB.
            keyboard_name: name.map(|name| name.strip_suffix(" Dongle").unwrap_or(&name).into()),
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Minute {
    pub packets: u16,
    pub invalid: u8,
    pub dropped: u8,
    pub channel_changes: u8,
    pub link_losses: Option<u8>,
    pub usb_retries: u8,
    pub gaps: Option<u8>,
    pub rssi: Option<Rssi>,
    pub noise: Option<Noise>,
}

impl Minute {
    /// `None` for a counter the dongle keeps only as a total or its protocol does not count.
    pub fn get(&self, counter: Counter) -> Option<u16> {
        match counter {
            Counter::Packets => Some(self.packets),
            Counter::Invalid => Some(self.invalid.into()),
            Counter::Dropped => Some(self.dropped.into()),
            Counter::ChannelChanges => Some(self.channel_changes.into()),
            Counter::LinkLosses => self.link_losses.map(Into::into),
            Counter::UsbRetries => Some(self.usb_retries.into()),
            Counter::Gaps => self.gaps.map(Into::into),
            Counter::Merges | Counter::Surveys => None,
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
        let packets = r.u16()?;
        let (invalid, dropped, channel_changes, link_losses, usb_retries, gaps) =
            (r.u8()?, r.u8()?, r.u8()?, r.u8()?, r.u8()?, r.u8()?);
        let rssi = Rssi {
            min: r.i8()?,
            avg: r.i8()?,
            max: r.i8()?,
        };
        Ok(Minute {
            packets,
            invalid,
            dropped,
            channel_changes,
            link_losses: protocol.counted(Counter::LinkLosses, link_losses),
            usb_retries,
            gaps: protocol.counted(Counter::Gaps, gaps),
            rssi: present(rssi),
            noise: read_noise(r)?,
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
            other => Self::Unknown(other),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Event {
    pub time_ms: u64,
    pub kind: EventKind,
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
    fn only_the_esb_protocols_show_a_channel_and_signal() {
        assert!(Protocol::Nuphy.has_channel_and_signal());
        assert!(Protocol::Unifying.has_channel_and_signal());
        assert!(!Protocol::Bluetooth.has_channel_and_signal());
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
    pub(crate) const PROTOCOL_AT: usize = 86;
    const PAIRING_AT: usize = PROTOCOL_AT + 1;
    const KEYBOARD_AT: usize = PAIRING_AT + 1;
    const NAME_AT: usize = KEYBOARD_AT + 5;

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
        }
        payload
    }

    pub(crate) fn status_of(protocol: Protocol) -> Status {
        read_status(&status_running(protocol as u8))
            .unwrap()
            .unwrap()
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

    #[test]
    fn a_bluetooth_status_reads_its_link_states() {
        let mut payload = status_running(Protocol::Bluetooth as u8);
        payload[LINK_STATE_AT] = 1;
        payload[NAME_AT..].fill(0);
        payload[NAME_AT..][..7].copy_from_slice(b"MX Keys");
        let s = read_status(&payload).unwrap().unwrap();
        assert_eq!(
            s.link,
            Link::Bluetooth(BluetoothLink::Connected {
                name: Some("MX Keys".into())
            })
        );
        assert_eq!(s.link.keyboard_name(), Some("MX Keys"));
        payload[LINK_STATE_AT] = 2;
        let s = read_status(&payload).unwrap().unwrap();
        assert_eq!(s.link, Link::Bluetooth(BluetoothLink::StartFailed));
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
        assert_eq!(s.link, Link::Bluetooth(BluetoothLink::NoKeyboard));
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
    fn request_carries_command_seq_and_arg() {
        let r = request(Command::History, 9, 0x1234_5678);
        assert_eq!(&r[..6], &[2, 9, 0x78, 0x56, 0x34, 0x12]);
        assert!(r[6..].iter().all(|&b| b == 0));
    }

    #[test]
    fn status_fixture_decodes() {
        let s = read_status(&fixture("status.hex")).unwrap().unwrap();
        assert_eq!(s.firmware_version, [1, 2, 3]);
        assert_eq!(s.uptime_ms, 0x01_0203_0405);
        assert_eq!(s.reset_cause, 1);
        assert_eq!(s.channel, 26);
        assert_eq!(s.last_rssi, -71);
        assert_eq!(s.last_rx_age_ms, Some(12));
        assert!(s.usb_live && !s.usb_suspended);
        assert_eq!(
            s.counters,
            Counters {
                packets: 1000,
                invalid: 1,
                dropped: 2,
                channel_changes: 3,
                link_losses: Some(4),
                usb_retries: 5,
                gaps: Some(8),
                merges: 6,
                surveys: Some(7),
            }
        );
        assert_eq!((s.minute_next, s.minute_oldest), (1500, 60));
        assert_eq!((s.events_total, s.events_oldest), (600, 88));
        assert_eq!(s.protocol(), Protocol::Nuphy);
        assert!(s.pairing_open);
        assert_eq!(
            s.link,
            Link::Nuphy(NuphyLink {
                state: NuphyState::Connected,
                own: Some([26, 30, 34]),
                forced: Some([62, 52, 44]),
                keyboard_id: Some([0x70, 0x15, 0xa3, 0x29]),
                keyboard_name: Some("MX Keys".into())
            })
        );
        let unifying = status_of(Protocol::Unifying);
        assert_eq!(
            unifying.link,
            Link::Unifying(Some(UnifyingLink {
                wpid: [0x70, 0x15],
                name: Some("MX Keys".into()),
                serial: [0xb8, 0xbc, 0xd5, 0xb3],
                kind: 1,
                state: UnifyingState::Connected
            }))
        );
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
        payload[KEYBOARD_AT..].fill(0);
        assert_eq!(
            read_status(&payload).unwrap().unwrap().link,
            Link::Unifying(Some(UnifyingLink {
                wpid: [0, 0],
                name: None,
                serial: [0; 4],
                kind: 0,
                state: UnifyingState::Silent
            }))
        );
    }

    fn keyboard_named(protocol: Protocol, name: &[u8]) -> Link {
        let mut payload = status_running(protocol as u8);
        payload[NAME_AT..].fill(0);
        payload[NAME_AT..][..name.len()].copy_from_slice(name);
        read_status(&payload).unwrap().unwrap().link
    }

    #[test]
    fn a_keyboard_name_keeps_only_printable_ascii() {
        let keyboard = keyboard_named(Protocol::Unifying, b"MX\x1b[2J\xffKeys");
        assert_eq!(keyboard.keyboard_name(), Some("MX?[2J?Keys"));
    }

    #[test]
    fn a_keyboard_name_can_fill_its_field() {
        let keyboard = keyboard_named(Protocol::Nuphy, b"Twenty-four letters long");
        assert_eq!(keyboard.keyboard_name(), Some("Twenty-four letters long"));
    }

    #[test]
    fn only_a_nuphy_name_loses_its_dongle_suffix() {
        for (protocol, shown) in [
            (Protocol::Nuphy, "NuPhy Air75 V2"),
            (Protocol::Unifying, "NuPhy Air75 V2 Dongle"),
        ] {
            let keyboard = keyboard_named(protocol, b"NuPhy Air75 V2 Dongle");
            assert_eq!(keyboard.keyboard_name(), Some(shown));
        }
    }

    #[test]
    fn a_unifying_keyboard_without_a_name_has_none() {
        let keyboard = keyboard_named(Protocol::Unifying, b"");
        assert_eq!(keyboard.keyboard_name(), None);
    }

    #[test]
    fn the_status_fixture_ends_with_the_keyboard_name() {
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
            refusal(1),
            "the dongle speaks diagnostic protocol 1, this tool 2; flash the firmware from this \
             tool's release with `open-dongle flash`"
        );
    }

    #[test]
    fn a_newer_dongle_is_told_to_take_its_releases_tool() {
        assert_eq!(
            refusal(3),
            "the dongle speaks diagnostic protocol 3, this tool 2; install the open-dongle tool \
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
    fn minutes_fixture_decodes() {
        let (first, m) = decode_minutes(&fixture("minutes.hex"), Protocol::Nuphy).unwrap();
        assert_eq!(first, 1440);
        assert_eq!(m.len(), 2);
        assert_eq!(m[0].packets, 300);
        assert_eq!(m[0].usb_retries, 1);
        assert_eq!(m[0].gaps, Some(3));
        assert_eq!(
            m[0].rssi,
            Some(Rssi {
                min: -80,
                avg: -70,
                max: -60
            })
        );
        assert_eq!(
            m[0].noise,
            Some(Noise {
                peak: -41,
                busy: 102,
                strong: 5
            })
        );
        assert_eq!((m[1].channel_changes, m[1].link_losses), (12, Some(1)));
        assert_eq!(m[1].rssi, None);
        assert_eq!(m[1].noise, None);
    }

    #[test]
    fn events_fixture_decodes() {
        let (first, e) = decode_events(&fixture("events.hex")).unwrap();
        assert_eq!(first, 88);
        assert_eq!(
            e[0],
            Event {
                time_ms: 5000,
                kind: EventKind::LinkLost,
                a: 0xb9,
                b: 0,
                c: 0,
                value: 1200,
                noise: Some(Noise {
                    peak: -50,
                    busy: 2500,
                    strong: 300
                })
            }
        );
        assert_eq!(e[1].kind, EventKind::Paired);
        assert_eq!((e[1].a, e[1].b, e[1].c), (26, 30, 34));
        assert_eq!(e[1].noise, None);
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
            ]
        );
    }

    #[test]
    fn a_unifying_dongle_has_none_of_the_counters_only_nuphy_counts() {
        let c = status_of(Protocol::Unifying).counters;
        assert_eq!((c.link_losses, c.gaps, c.surveys), (None, None, None));
        assert_eq!(c.get(Counter::Merges), Some(6));
        let (_, m) = decode_minutes(&fixture("minutes.hex"), Protocol::Unifying).unwrap();
        assert_eq!((m[0].link_losses, m[0].gaps), (None, None));
        assert_eq!(m[0].get(Counter::UsbRetries), Some(1));
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
        payload.extend([5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        let (_, m) = decode_minutes(&payload, Protocol::Nuphy).unwrap();
        assert_eq!(m[0].packets, 5);
        assert_eq!(m[0].rssi, None);
        assert_eq!(m[0].noise, None);
    }
}
