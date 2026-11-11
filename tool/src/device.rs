use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail};

use crate::protocol::{
    Action, CHUNK, Command, Event, Minute, NoiseChannel, Part, REPORT_LEN, Status, VersionMismatch,
    decode_events, decode_minutes, decode_noise, parse_part, read_status, request,
};

pub const VID: u16 = 0x2fe3;
pub const PID: u16 = 0x4e44;
pub const USAGE_PAGE: u16 = 0xff00;
/// Longer than the dongle's longest main-loop block, a 1.5 s radio survey.
pub const TIMEOUT: Duration = Duration::from_millis(2500);
pub const NOISE_TIMEOUT: Duration = Duration::from_secs(4);

pub trait Transport {
    fn write(&mut self, report: &[u8; REPORT_LEN]) -> Result<()>;
    /// Returns false when nothing arrived within the timeout.
    fn read(&mut self, buf: &mut [u8; REPORT_LEN], timeout: Duration) -> Result<bool>;
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
        self.seq = self.seq.wrapping_add(1);
        let seq = self.seq;
        self.transport.write(&request(command, seq, arg))?;
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
                Part::Error { code, .. } => bail!("the dongle refused {command:?}: error {code}"),
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

    pub fn status(&mut self) -> Result<Status> {
        Ok(self.read_status()??)
    }

    pub fn read_status(&mut self) -> Result<Result<Status, VersionMismatch>> {
        read_status(&self.call(Command::Status, 0, TIMEOUT)?)
    }

    /// Refuses a dongle that speaks a diagnostic protocol other than this tool's.
    pub fn checked(mut self) -> Result<(Self, Status)> {
        let status = self.status()?;
        Ok((self, status))
    }

    pub fn minutes(&mut self, from: u32) -> Result<(u32, Vec<Minute>)> {
        decode_minutes(&self.call(Command::History, from, TIMEOUT)?)
    }

    pub fn events(&mut self, from: u32) -> Result<(u32, Vec<Event>)> {
        decode_events(&self.call(Command::Events, from, TIMEOUT)?)
    }

    pub fn noise(&mut self) -> Result<Vec<NoiseChannel>> {
        decode_noise(&self.call(Command::Noise, 0, NOISE_TIMEOUT)?)
    }

    /// Returns the held minutes with their indexes, from `from` (or the oldest held) up to `end`.
    pub fn minutes_between(&mut self, from: u32, end: u32) -> Result<Vec<(u32, Minute)>> {
        collect_pages(from, end, |f| self.minutes(f))
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
    fn write(&mut self, report: &[u8; REPORT_LEN]) -> Result<()> {
        let mut out = [0u8; REPORT_LEN + 1];
        out[1..].copy_from_slice(report);
        self.0.write(&out).context("writing to the dongle")?;
        Ok(())
    }

    fn read(&mut self, buf: &mut [u8; REPORT_LEN], timeout: Duration) -> Result<bool> {
        let ms = i32::try_from(timeout.as_millis()).unwrap_or(i32::MAX);
        let n = self
            .0
            .read_timeout(buf, ms)
            .context("reading from the dongle")?;
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
        [] => bail!("no OpenDongle with a diagnostic interface found"),
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
        fn write(&mut self, report: &[u8; REPORT_LEN]) -> Result<()> {
            let replies = (self.respond)(report);
            self.pending.extend(replies);
            Ok(())
        }

        fn read(&mut self, buf: &mut [u8; REPORT_LEN], _timeout: Duration) -> Result<bool> {
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
        let (_, status) = dongle_in_protocol(1).checked().unwrap();
        assert_eq!(status.channel, 26);
    }

    #[test]
    fn a_dongle_in_another_protocol_is_refused_on_connect() {
        let err = dongle_in_protocol(2).checked().err().unwrap().to_string();
        assert!(err.starts_with("the dongle speaks diagnostic protocol 2, this tool 1;"));
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
                let n = 130u32.saturating_sub(from).min(60) as u8;
                let mut p = from.to_le_bytes().to_vec();
                p.push(n);
                for _ in 0..n {
                    p.extend([1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
                }
                parts(req[0], req[1], &p)
            }),
            0,
        );
        let m = dev.minutes_between(0, 130).unwrap();
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
        let (first, m) = dev.minutes(1440).unwrap();
        assert_eq!((first, m.len()), (1440, 2));
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
        assert_eq!(dev.status().unwrap().counters.surveys, 7);
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
        assert_eq!(dev.status().unwrap().link_state, 4);
    }

    #[test]
    fn reports_a_dongle_error() {
        let mut dev = Device::new(
            Fake::new(|req| {
                let mut r = [0u8; REPORT_LEN];
                r[..5].copy_from_slice(&[req[0] | crate::protocol::ERROR_FLAG, req[1], 0, 1, 1]);
                vec![r]
            }),
            0,
        );
        let err = dev.status().unwrap_err().to_string();
        assert!(err.contains("error 1"), "{err}");
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
}
