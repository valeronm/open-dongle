use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail};

use crate::protocol::{
    Action, CHUNK, Command, ErrorCode, Event, Minute, NoiseChannel, Part, Protocol, REPORT_LEN,
    Reader, SlotStatus, Status, StoredDevice, VersionMismatch, Watch, channels_arg, decode_events,
    decode_minutes, decode_noise, decode_watch, parse_part, read_devices, read_status, request,
    shown_slots,
};

pub const VID: u16 = 0x2fe3;
pub const PID: u16 = 0x4e44;
pub const USAGE_PAGE: u16 = 0xff00;
/// Longer than the dongle's longest main-loop block, a 1.5 s radio survey.
pub const TIMEOUT: Duration = Duration::from_millis(2500);
pub const NOISE_TIMEOUT: Duration = Duration::from_secs(4);

/// A reply that carried an error code in place of a payload.
#[derive(Debug)]
pub struct Refused {
    pub command: Command,
    /// The raw byte when this tool does not know the code.
    pub code: Result<ErrorCode, u8>,
    /// The protocol the dongle runs, carried only by `ErrorCode::WrongProtocol`.
    pub running: Option<Protocol>,
}

impl std::fmt::Display for Refused {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "the dongle refused {:?}: ", self.command)?;
        match self.code {
            Ok(ErrorCode::WrongProtocol) => write!(
                f,
                "not available while the dongle runs {}",
                self.running.map_or("another protocol", Protocol::name)
            ),
            Ok(code) => f.write_str(code.name()),
            Err(code) => write!(f, "error {code}"),
        }
    }
}

impl std::error::Error for Refused {}

/// The HID device failed, which is how an unplugged dongle shows.
#[derive(Debug)]
pub struct Lost {
    pub doing: &'static str,
    pub cause: hidapi::HidError,
}

impl std::fmt::Display for Lost {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        f.write_str(self.doing)
    }
}

impl std::error::Error for Lost {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        Some(&self.cause)
    }
}

pub trait Transport {
    fn write(&mut self, report: &[u8; REPORT_LEN]) -> Result<(), Lost>;
    /// Returns false when nothing arrived within the timeout.
    fn read(&mut self, buf: &mut [u8; REPORT_LEN], timeout: Duration) -> Result<bool, Lost>;
}

pub struct Device<T: Transport> {
    transport: T,
    seq: u8,
}

impl<T: Transport> Device<T> {
    pub fn new(transport: T, seq: u8) -> Self {
        Self { transport, seq }
    }

    /// Sends one request and returns the joined payload of its reply, padding included.
    pub fn call(&mut self, command: Command, arg: u32, timeout: Duration) -> Result<Vec<u8>> {
        self.call_with(command, arg, &[], timeout)
    }

    fn call_with(
        &mut self,
        command: Command,
        arg: u32,
        data: &[u8],
        timeout: Duration,
    ) -> Result<Vec<u8>> {
        self.seq = self.seq.wrapping_add(1);
        let seq = self.seq;
        self.transport.write(&request(command, seq, arg, data))?;
        let deadline = Instant::now() + timeout;
        let mut payload = Vec::new();
        let mut received: Vec<bool> = Vec::new();
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                bail!("the dongle did not answer {command:?} in time");
            }
            let mut buf = [0u8; REPORT_LEN];
            if !self.transport.read(&mut buf, left)? {
                continue;
            }
            let Some(part) = parse_part(&buf).filter(|p| p.is_reply_to(command, seq)) else {
                continue;
            };
            let (index, count, chunk) = match part {
                Part::Error { code, detail, .. } => {
                    let code = ErrorCode::from_code(code).ok_or(code);
                    let running = (code == Ok(ErrorCode::WrongProtocol))
                        .then(|| Protocol::from_code(detail))
                        .flatten();
                    return Err(Refused {
                        command,
                        code,
                        running,
                    }
                    .into());
                }
                Part::Data {
                    index,
                    count,
                    chunk,
                    ..
                } if index < count => (index as usize, count as usize, chunk),
                Part::Data { .. } => continue,
            };
            if received.is_empty() {
                received = vec![false; count];
                payload = vec![0; count * CHUNK];
            }
            if received.len() != count {
                bail!("the dongle changed the part count of one reply");
            }
            payload[index * CHUNK..(index + 1) * CHUNK].copy_from_slice(chunk);
            received[index] = true;
            if received.iter().all(|&r| r) {
                return Ok(payload);
            }
        }
    }

    pub fn action(&mut self, action: Action) -> Result<()> {
        self.call(action.command(), 0, TIMEOUT).map(drop)
    }

    /// Slot 0's status, which carries the link-wide fields and the slot table.
    pub fn status(&mut self) -> Result<Status> {
        Ok(self.read_status()??)
    }

    pub fn read_status(&mut self) -> Result<Result<Status, VersionMismatch>> {
        read_status(&self.call(Command::Status, 0, TIMEOUT)?)
    }

    pub fn slot_status(&mut self, slot: u8) -> Result<SlotStatus> {
        let reply = self.call_with(Command::Status, 0, &[slot], TIMEOUT)?;
        Ok(read_status(&reply)??.device)
    }

    /// The status of each slot `shown_slots` picks from `s`'s slot table, reusing `s`'s own.
    pub fn shown(&mut self, s: &Status) -> Result<Vec<SlotStatus>> {
        shown_slots(&s.slots)
            .into_iter()
            .map(|slot| {
                if slot == s.device.slot {
                    Ok(s.device.clone())
                } else {
                    self.slot_status(slot)
                }
            })
            .collect()
    }

    /// Refuses a dongle that speaks a diagnostic protocol other than this tool's.
    pub fn checked(mut self) -> Result<(Self, Status)> {
        let status = self.status()?;
        Ok((self, status))
    }

    pub fn minutes(
        &mut self,
        from: u32,
        slot: u8,
        protocol: Protocol,
    ) -> Result<(u32, Vec<Minute>)> {
        decode_minutes(
            &self.call_with(Command::History, from, &[slot], TIMEOUT)?,
            protocol,
        )
    }

    pub fn events(&mut self, from: u32) -> Result<(u32, Vec<Event>)> {
        decode_events(&self.call(Command::Events, from, TIMEOUT)?)
    }

    /// `None` returns pairing to the survey.
    pub fn force_channels(&mut self, channels: Option<[u8; 3]>) -> Result<()> {
        self.call(Command::ForceChannels, channels_arg(channels), TIMEOUT)
            .map(drop)
    }

    /// Returns the channel the link was on.
    pub fn move_to(&mut self, channel: u8) -> Result<u8> {
        let reply = self.call(Command::Move, channel.into(), TIMEOUT)?;
        Reader::new(&reply).u8()
    }

    /// Returns the passkey to type on the keyboard when the dongle asks for one, which only
    /// Bluetooth does.
    pub fn pair(&mut self, no_passkey: bool) -> Result<Option<u32>> {
        let reply = self.call(Command::Pair, u32::from(no_passkey), TIMEOUT)?;
        let mut r = Reader::new(&reply);
        let asked = r.u8()? != 0;
        Ok(asked.then_some(r.u32()?))
    }

    /// Returns the protocol running when the dongle answered, and whether it now reboots.
    pub fn protocol(&mut self, set: Option<Protocol>) -> Result<(Protocol, bool)> {
        let reply = self.call(Command::Protocol, set.map_or(0, |p| p as u32), TIMEOUT)?;
        let mut r = Reader::new(&reply);
        let running = Protocol::read(&mut r)?;
        Ok((running, r.u8()? != 0))
    }

    pub fn devices(&mut self) -> Result<Vec<StoredDevice>> {
        read_devices(&self.call(Command::Devices, 0, TIMEOUT)?)
    }

    pub fn forget(&mut self, device: &StoredDevice) -> Result<()> {
        self.call_with(Command::Forget, 0, &device.addr, TIMEOUT)
            .map(drop)
    }

    pub fn noise(&mut self) -> Result<Vec<NoiseChannel>> {
        decode_noise(&self.call(Command::Noise, 0, NOISE_TIMEOUT)?)
    }

    pub fn watch(&mut self) -> Result<Watch> {
        decode_watch(&self.call(Command::Watch, 0, TIMEOUT)?)
    }

    /// Returns the slot's held minutes with their indexes, from `from` (or the oldest held) up to
    /// `end`.
    pub fn minutes_between(
        &mut self,
        from: u32,
        end: u32,
        slot: u8,
        protocol: Protocol,
    ) -> Result<Vec<(u32, Minute)>> {
        collect_pages(from, end, |f| self.minutes(f, slot, protocol))
    }

    /// Returns the held events with their indexes, from `from` (or the oldest held) up to `end`.
    pub fn events_between(&mut self, from: u32, end: u32) -> Result<Vec<(u32, Event)>> {
        collect_pages(from, end, |f| self.events(f))
    }
}

fn collect_pages<T>(
    mut from: u32,
    end: u32,
    mut fetch: impl FnMut(u32) -> Result<(u32, Vec<T>)>,
) -> Result<Vec<(u32, T)>> {
    let mut out = Vec::new();
    while from < end {
        let (first, page) = fetch(from)?;
        let next = first.saturating_add(page.len() as u32);
        if next <= from {
            break;
        }
        out.extend((first..).zip(page));
        from = next;
    }
    Ok(out)
}

pub struct HidTransport(hidapi::HidDevice);

impl Transport for HidTransport {
    fn write(&mut self, report: &[u8; REPORT_LEN]) -> Result<(), Lost> {
        let mut out = [0u8; REPORT_LEN + 1];
        out[1..].copy_from_slice(report);
        self.0.write(&out).map_err(|cause| Lost {
            doing: "writing to the dongle",
            cause,
        })?;
        Ok(())
    }

    fn read(&mut self, buf: &mut [u8; REPORT_LEN], timeout: Duration) -> Result<bool, Lost> {
        let ms = i32::try_from(timeout.as_millis()).unwrap_or(i32::MAX);
        let n = self.0.read_timeout(buf, ms).map_err(|cause| Lost {
            doing: "reading from the dongle",
            cause,
        })?;
        Ok(n == REPORT_LEN)
    }
}

pub fn dongles(api: &hidapi::HidApi) -> impl Iterator<Item = &hidapi::DeviceInfo> {
    api.device_list()
        .filter(|d| d.vendor_id() == VID && d.product_id() == PID && d.usage_page() == USAGE_PAGE)
}

pub fn open(serial: Option<&str>) -> Result<(Device<HidTransport>, Status)> {
    open_unchecked(serial)?.checked()
}

/// Skips the protocol check, for the commands every protocol version keeps.
pub fn open_unchecked(serial: Option<&str>) -> Result<Device<HidTransport>> {
    let api = hidapi::HidApi::new().context("starting hidapi")?;
    let found: Vec<_> = dongles(&api)
        .filter(|d| serial.is_none_or(|s| d.serial_number() == Some(s)))
        .collect();
    let info = match found.as_slice() {
        [] => bail!("no {} with a diagnostic interface found", crate::PRODUCT),
        [one] => *one,
        many => {
            let serials: Vec<_> = many.iter().filter_map(|d| d.serial_number()).collect();
            bail!(
                "{} dongles found; pick one with --serial: {}",
                many.len(),
                serials.join(", ")
            );
        }
    };
    let device = info.open_device(&api).context("opening the dongle")?;
    // A reply left over from an earlier run must not match this run's first sequence number.
    let seq = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.subsec_nanos() as u8);
    Ok(Device::new(HidTransport(device), seq))
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::protocol::HEADER_LEN;
    use crate::protocol::tests::{fixture, status_in_protocol};
    use std::collections::VecDeque;

    type Responder = Box<dyn FnMut(&[u8; REPORT_LEN]) -> Vec<[u8; REPORT_LEN]>>;

    /// Answers each request through `respond`, which returns the reports to queue for reading.
    pub(crate) struct Fake {
        pub(crate) respond: Responder,
        pending: VecDeque<[u8; REPORT_LEN]>,
    }

    impl Fake {
        pub(crate) fn new(
            respond: impl FnMut(&[u8; REPORT_LEN]) -> Vec<[u8; REPORT_LEN]> + 'static,
        ) -> Self {
            Self {
                respond: Box::new(respond),
                pending: VecDeque::new(),
            }
        }
    }

    impl Transport for Fake {
        fn write(&mut self, report: &[u8; REPORT_LEN]) -> Result<(), Lost> {
            let replies = (self.respond)(report);
            self.pending.extend(replies);
            Ok(())
        }

        fn read(&mut self, buf: &mut [u8; REPORT_LEN], _timeout: Duration) -> Result<bool, Lost> {
            match self.pending.pop_front() {
                Some(report) => {
                    *buf = report;
                    Ok(true)
                }
                None => Ok(false),
            }
        }
    }

    pub(crate) fn parts(command: u8, seq: u8, payload: &[u8]) -> Vec<[u8; REPORT_LEN]> {
        let count = payload.len().div_ceil(CHUNK).max(1);
        (0..count)
            .map(|i| {
                let mut r = [0u8; REPORT_LEN];
                r[..HEADER_LEN].copy_from_slice(&[command, seq, i as u8, count as u8]);
                let chunk = payload.chunks(CHUNK).nth(i).unwrap_or(&[]);
                r[HEADER_LEN..HEADER_LEN + chunk.len()].copy_from_slice(chunk);
                r
            })
            .collect()
    }

    pub(crate) fn error_part(command: u8, seq: u8, code: u8) -> [u8; REPORT_LEN] {
        let mut r = [0u8; REPORT_LEN];
        r[..5].copy_from_slice(&[command | crate::protocol::ERROR_FLAG, seq, 0, 1, code]);
        r
    }

    #[test]
    fn an_action_succeeds_on_an_empty_reply() {
        let mut dev = Device::new(Fake::new(|req| parts(req[0], req[1], &[])), 0);
        dev.action(Action::Clear).unwrap();
    }

    fn dongle_in_protocol(version: u8) -> Device<Fake> {
        Device::new(
            Fake::new(move |req| parts(req[0], req[1], &status_in_protocol(version))),
            0,
        )
    }

    #[test]
    fn a_dongle_in_this_protocol_connects_with_its_status() {
        let (_, status) = dongle_in_protocol(4).checked().unwrap();
        assert_eq!(status.channel, 26);
    }

    #[test]
    fn a_dongle_in_another_protocol_is_refused_on_connect() {
        let err = dongle_in_protocol(3).checked().err().unwrap().to_string();
        assert!(err.starts_with("the dongle speaks diagnostic protocol 3, this tool 4;"));
    }

    #[test]
    fn joins_a_two_part_status() {
        let mut dev = Device::new(
            Fake::new(|req| parts(req[0], req[1], &fixture("status.hex"))),
            0,
        );
        assert_eq!(dev.status().unwrap().channel, 26);
    }

    #[test]
    fn minutes_between_pages_until_the_end() {
        let mut dev = Device::new(
            Fake::new(|req| {
                let from = u32::from_le_bytes(req[2..6].try_into().unwrap());
                let n = 130u32.saturating_sub(from).min(40) as u8;
                let mut p = from.to_le_bytes().to_vec();
                p.push(n);
                for _ in 0..n {
                    p.extend([0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
                }
                parts(req[0], req[1], &p)
            }),
            0,
        );
        let m = dev.minutes_between(0, 130, 0, Protocol::Nuphy).unwrap();
        assert_eq!(m.len(), 130);
        assert_eq!((m[0].0, m[129].0), (0, 129));
    }

    #[test]
    fn asks_for_minutes_from_the_given_index() {
        let mut dev = Device::new(
            Fake::new(|req| {
                assert_eq!(req[0], Command::History as u8);
                assert_eq!(u32::from_le_bytes(req[2..6].try_into().unwrap()), 1440);
                parts(req[0], req[1], &fixture("minutes.hex"))
            }),
            0,
        );
        let (first, m) = dev.minutes(1440, 0, Protocol::Nuphy).unwrap();
        assert_eq!((first, m.len()), (1440, 2));
    }

    #[test]
    fn asks_for_a_slots_minutes_as_the_request_fixture_has_it() {
        let sent = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let seen = sent.clone();
        let mut dev = Device::new(
            Fake::new(move |req| {
                *seen.borrow_mut() = req.to_vec();
                parts(req[0], req[1], &fixture("minutes.hex"))
            }),
            8,
        );
        dev.minutes(1440, 1, Protocol::Bluetooth).unwrap();
        let expected = fixture("request.hex");
        let sent = sent.borrow();
        assert_eq!(&sent[..expected.len()], expected);
        assert!(sent[expected.len()..].iter().all(|&b| b == 0));
    }

    #[test]
    fn asks_for_a_slots_status_by_its_number() {
        let mut dev = Device::new(
            Fake::new(|req| {
                let reply = if req[6] == 1 {
                    "status-slot1.hex"
                } else {
                    "status.hex"
                };
                parts(req[0], req[1], &fixture(reply))
            }),
            0,
        );
        let device = dev.slot_status(1).unwrap();
        assert_eq!(
            (device.slot, device.name.as_deref()),
            (1, Some("MX Master 3S"))
        );
    }

    /// Answers status requests from status-slot1.hex as the slot asked, recording each slot asked.
    fn bluetooth_dongle(asked: std::rc::Rc<std::cell::RefCell<Vec<u8>>>) -> Device<Fake> {
        Device::new(
            Fake::new(move |req| {
                assert_eq!(req[0], Command::Status as u8);
                asked.borrow_mut().push(req[6]);
                let mut reply = fixture("status-slot1.hex");
                let slot_at = reply.len() - 42;
                reply[slot_at] = req[6];
                parts(req[0], req[1], &reply)
            }),
            0,
        )
    }

    /// Slot 0's status from a dongle whose slot table is `stored` and `connected` per slot.
    fn slot_0_status(stored: [bool; 2], connected: [bool; 2]) -> Status {
        let mut s = crate::protocol::tests::mouse_status();
        s.device.slot = 0;
        for (i, slot) in s.slots.iter_mut().enumerate() {
            slot.stored = stored[i];
            slot.connected = connected[i];
        }
        s
    }

    fn shown_from(s: &Status) -> (Vec<u8>, Vec<u8>) {
        let asked = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let mut dev = bluetooth_dongle(asked.clone());
        let shown = dev.shown(s).unwrap();
        let slots = shown.iter().map(|d| d.slot).collect();
        (slots, asked.take())
    }

    #[test]
    fn the_shown_slots_past_the_first_reply_are_asked_for() {
        let both = slot_0_status([true, true], [false, true]);
        assert_eq!(shown_from(&both), (vec![0, 1], vec![1]));
        let keyboard = slot_0_status([true, false], [true, false]);
        assert_eq!(shown_from(&keyboard), (vec![0], vec![]));
        let mouse = slot_0_status([false, true], [false, true]);
        assert_eq!(shown_from(&mouse), (vec![1], vec![1]));
    }

    #[test]
    fn parts_out_of_order_still_join() {
        let mut dev = Device::new(
            Fake::new(|req| {
                let mut p = parts(req[0], req[1], &fixture("status.hex"));
                p.reverse();
                p
            }),
            0,
        );
        assert_eq!(dev.status().unwrap().counters.surveys, Some(7));
    }

    #[test]
    fn skips_a_stale_reply_to_an_earlier_request() {
        let mut dev = Device::new(
            Fake::new(|req| {
                let mut p = parts(req[0], req[1].wrapping_sub(1), &[0xee; 10]);
                p.extend(parts(req[0], req[1], &fixture("status.hex")));
                p
            }),
            0,
        );
        assert_eq!(dev.status().unwrap().channel, 26);
    }

    #[test]
    fn reports_a_dongle_error() {
        let mut dev = Device::new(Fake::new(|req| vec![error_part(req[0], req[1], 1)]), 0);
        let err = dev.status().unwrap_err().to_string();
        assert_eq!(err, "the dongle refused Status: unknown command");
    }

    #[test]
    fn an_unknown_error_code_is_reported_as_a_number() {
        let mut dev = Device::new(Fake::new(|req| vec![error_part(req[0], req[1], 99)]), 0);
        let err = dev.status().unwrap_err().to_string();
        assert_eq!(err, "the dongle refused Status: error 99");
    }

    #[test]
    fn times_out_when_nothing_answers() {
        let mut dev = Device::new(Fake::new(|_| Vec::new()), 0);
        let err = dev
            .call(Command::Status, 0, Duration::from_millis(20))
            .unwrap_err()
            .to_string();
        assert!(err.contains("did not answer"), "{err}");
    }

    #[test]
    fn setting_a_protocol_sends_its_number_and_reads_the_reboot_flag() {
        let mut dev = Device::new(
            Fake::new(|req| {
                assert_eq!(req[0], Command::Protocol as u8);
                assert_eq!(req[2], 2);
                parts(req[0], req[1], &[1, 1])
            }),
            0,
        );
        assert_eq!(
            dev.protocol(Some(Protocol::Unifying)).unwrap(),
            (Protocol::Nuphy, true)
        );
    }

    #[test]
    fn pair_returns_the_passkey_a_bluetooth_dongle_chose() {
        let mut dev = Device::new(
            Fake::new(|req| parts(req[0], req[1], &[1, 0x40, 0xe2, 0x01, 0x00])),
            0,
        );
        assert_eq!(dev.pair(false).unwrap(), Some(123_456));
        let mut dev = Device::new(Fake::new(|req| parts(req[0], req[1], &[1, 0, 0, 0, 0])), 0);
        assert_eq!(dev.pair(false).unwrap(), Some(0));
        let mut dev = Device::new(Fake::new(|req| parts(req[0], req[1], &[])), 0);
        assert_eq!(dev.pair(false).unwrap(), None);
    }

    #[test]
    fn pairing_without_a_passkey_is_asked_for_in_the_request() {
        let asked = std::rc::Rc::new(std::cell::Cell::new(None));
        let seen = asked.clone();
        let mut dev = Device::new(
            Fake::new(move |req| {
                seen.set(Some(req[2]));
                parts(req[0], req[1], &[])
            }),
            0,
        );
        dev.pair(true).unwrap();
        assert_eq!(asked.get(), Some(1));
        dev.pair(false).unwrap();
        assert_eq!(asked.get(), Some(0));
    }

    #[test]
    fn a_wrong_protocol_refusal_names_the_running_one() {
        let mut dev = Device::new(
            Fake::new(|req| {
                let mut e = error_part(req[0], req[1], 5);
                e[5] = 2;
                vec![e]
            }),
            0,
        );
        let err = dev.pair(false).unwrap_err().to_string();
        assert!(
            err.contains("not available while the dongle runs Unifying"),
            "{err}"
        );
    }
}
