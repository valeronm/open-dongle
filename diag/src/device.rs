use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail};

use crate::protocol::{
    CHUNK, Command, Part, REPORT_LEN, Status, decode_status, parse_part, request,
};

pub const VID: u16 = 0x2fe3;
pub const PID: u16 = 0x4e44;
pub const USAGE_PAGE: u16 = 0xff00;
/// Longer than the dongle's longest main-loop block, a 1.5 s radio survey.
pub const TIMEOUT: Duration = Duration::from_millis(2500);

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

    pub fn status(&mut self) -> Result<Status> {
        decode_status(&self.call(Command::Status, 0, TIMEOUT)?)
    }
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

pub fn open(serial: Option<&str>) -> Result<Device<HidTransport>> {
    let api = hidapi::HidApi::new().context("starting hidapi")?;
    let found: Vec<_> = api
        .device_list()
        .filter(|d| d.vendor_id() == VID && d.product_id() == PID && d.usage_page() == USAGE_PAGE)
        .filter(|d| serial.is_none_or(|s| d.serial_number() == Some(s)))
        .collect();
    let info = match found.as_slice() {
        [] => bail!("no nuphy-dongle with a diagnostic interface found"),
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
    use crate::protocol::tests::fixture;
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
    fn joins_a_two_part_status() {
        let mut dev = Device::new(
            Fake::new(|req| parts(req[0], req[1], &fixture("status.hex"))),
            0,
        );
        assert_eq!(dev.status().unwrap().channel, 26);
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
