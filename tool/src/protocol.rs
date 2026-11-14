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
            Self::NotMovable => "it moves the link only with a keyboard paired and pairing closed",
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
}

impl Protocol {
    pub fn from_code(code: u8) -> Option<Self> {
        match code {
            1 => Some(Self::Nuphy),
            2 => Some(Self::Unifying),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Nuphy => "NuPhy",
            Self::Unifying => "Unifying",
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
    pub link_losses: u32,
    pub usb_retries: u32,
    pub gaps: u32,
    pub merges: u32,
    pub surveys: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Status {
    pub firmware_version: [u8; 3],
    pub uptime_ms: u64,
    pub reset_cause: u32,
    pub link_state: u8,
    pub channel: u8,
    pub own: Option<[u8; 3]>,
    /// The channels every pairing offers; `None` while pairing surveys.
    pub forced: Option<[u8; 3]>,
    /// Set only from a pairing request since the dongle booted.
    pub keyboard_id: Option<[u8; 4]>,
    pub last_rssi: i8,
    pub last_rx_age_ms: Option<u32>,
    pub usb_live: bool,
    pub usb_suspended: bool,
    pub counters: Counters,
    pub minute_next: u32,
    pub minute_oldest: u32,
    pub events_total: u32,
    pub events_oldest: u32,
    pub protocol: Protocol,
    pub pairing_open: bool,
}

/// The channels a NuPhy keyboard looks for its dongle on besides the three it gets at pairing.
pub const NUPHY_FIXED_CHANNELS: [u8; 7] = [16, 22, 44, 48, 58, 72, 78];

/// The channels a Unifying keyboard searches after a failed send, its pairing channels.
pub const UNIFYING_CHANNELS: [u8; 11] = [5, 8, 17, 32, 35, 41, 44, 62, 65, 71, 74];

impl Status {
    /// The channels a move can send the link to, ascending.
    pub fn keyboard_channels(&self) -> Vec<u8> {
        let mut channels: Vec<u8> = match self.protocol {
            Protocol::Unifying => UNIFYING_CHANNELS.to_vec(),
            Protocol::Nuphy => NUPHY_FIXED_CHANNELS
                .into_iter()
                .chain(self.own.into_iter().flatten())
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
    let counters = Counters {
        packets: r.u32()?,
        invalid: r.u32()?,
        dropped: r.u32()?,
        channel_changes: r.u32()?,
        link_losses: r.u32()?,
        usb_retries: r.u32()?,
        gaps: r.u32()?,
        merges: r.u32()?,
        surveys: r.u32()?,
    };
    Ok(Ok(Status {
        firmware_version,
        uptime_ms,
        reset_cause,
        link_state,
        channel,
        own: (own != [0; 3]).then_some(own),
        forced: (forced != [0; 3]).then_some(forced),
        keyboard_id: (keyboard_id != [0; 4]).then_some(keyboard_id),
        last_rssi,
        last_rx_age_ms: (age != u32::MAX).then_some(age),
        usb_live: usb & 1 != 0,
        usb_suspended: usb & 2 != 0,
        counters,
        minute_next: r.u32()?,
        minute_oldest: r.u32()?,
        events_total: r.u32()?,
        events_oldest: r.u32()?,
        protocol: Protocol::from_code(r.u8()?).ok_or_else(|| {
            anyhow::anyhow!("the dongle reports a protocol this tool does not know")
        })?,
        pairing_open: r.u8()? != 0,
    }))
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
    pub link_losses: u8,
    pub usb_retries: u8,
    pub gaps: u8,
    pub rssi: Option<Rssi>,
    pub noise: Option<Noise>,
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

pub fn decode_minutes(payload: &[u8]) -> Result<(u32, Vec<Minute>)> {
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
            link_losses,
            usb_retries,
            gaps,
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

    pub(crate) fn status_running(code: u8) -> Vec<u8> {
        let mut payload = fixture("status.hex");
        let at = payload.len() - 2;
        payload[at] = code;
        payload
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
    fn keyboard_channel_tables_match_fixtures() {
        assert_eq!(fixture("nuphy-fixed-channels.hex"), NUPHY_FIXED_CHANNELS);
        assert_eq!(fixture("unifying-channels.hex"), UNIFYING_CHANNELS);
    }

    #[test]
    fn a_keyboard_listens_on_its_protocols_channels() {
        let mut s = read_status(&fixture("status.hex")).unwrap().unwrap();
        s.own = Some([30, 26, 44]);
        assert_eq!(s.keyboard_channels(), [16, 22, 26, 30, 44, 48, 58, 72, 78]);
        s.own = None;
        assert_eq!(s.keyboard_channels(), NUPHY_FIXED_CHANNELS);
        s.protocol = Protocol::Unifying;
        assert_eq!(s.keyboard_channels(), UNIFYING_CHANNELS);
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
        payload[24..28].fill(0);
        let s = read_status(&payload).unwrap().unwrap();
        assert_eq!(s.keyboard_id, None);
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
        assert_eq!(s.link_state, 4);
        assert_eq!(s.channel, 26);
        assert_eq!(s.own, Some([26, 30, 34]));
        assert_eq!(s.forced, Some([62, 52, 44]));
        assert_eq!(s.keyboard_id, Some([0x70, 0x15, 0xa3, 0x29]));
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
                link_losses: 4,
                usb_retries: 5,
                gaps: 8,
                merges: 6,
                surveys: 7
            }
        );
        assert_eq!((s.minute_next, s.minute_oldest), (1500, 60));
        assert_eq!((s.events_total, s.events_oldest), (600, 88));
        assert_eq!(s.protocol, Protocol::Nuphy);
        assert!(s.pairing_open);
    }

    #[test]
    fn a_zero_pairing_byte_reads_as_closed() {
        let mut payload = fixture("status.hex");
        *payload.last_mut().unwrap() = 0;
        assert!(!read_status(&payload).unwrap().unwrap().pairing_open);
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
        let (first, m) = decode_minutes(&fixture("minutes.hex")).unwrap();
        assert_eq!(first, 1440);
        assert_eq!(m.len(), 2);
        assert_eq!(m[0].packets, 300);
        assert_eq!(m[0].usb_retries, 1);
        assert_eq!(m[0].gaps, 3);
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
        assert_eq!((m[1].channel_changes, m[1].link_losses), (12, 1));
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
    fn padding_after_the_count_is_ignored() {
        let mut payload = fixture("minutes.hex");
        payload.resize(CHUNK, 0);
        assert_eq!(decode_minutes(&payload).unwrap().1.len(), 2);
    }

    #[test]
    fn a_minute_of_other_devices_packets_has_no_signal() {
        let mut payload = 0u32.to_le_bytes().to_vec();
        payload.push(1);
        payload.extend([5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        let (_, m) = decode_minutes(&payload).unwrap();
        assert_eq!(m[0].packets, 5);
        assert_eq!(m[0].rssi, None);
        assert_eq!(m[0].noise, None);
    }
}
