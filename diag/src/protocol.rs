use anyhow::{Result, bail};

pub const REPORT_LEN: usize = 64;
pub const HEADER_LEN: usize = 4;
pub const CHUNK: usize = REPORT_LEN - HEADER_LEN;
pub const ERROR_FLAG: u8 = 0x80;
pub const VERSION: u8 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[expect(dead_code)]
pub enum Command {
    Status = 1,
    History,
    Events,
    Noise,
    Reboot,
    Bootloader,
    Forget,
    Clear,
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

    #[expect(dead_code)]
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
    pub merges: u32,
    pub surveys: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Status {
    pub uptime_ms: u64,
    pub reset_cause: u32,
    pub link_state: u8,
    pub channel: u8,
    pub own: Option<[u8; 3]>,
    pub keyboard_id: [u8; 4],
    pub last_rssi: i8,
    pub last_rx_age_ms: Option<u32>,
    pub usb_live: bool,
    pub usb_suspended: bool,
    pub counters: Counters,
    pub minute_next: u32,
    pub minute_oldest: u32,
    pub events_total: u32,
    pub events_oldest: u32,
}

pub fn decode_status(payload: &[u8]) -> Result<Status> {
    let mut r = Reader::new(payload);
    let version = r.u8()?;
    if version != VERSION {
        bail!("the dongle speaks diagnostic protocol {version}, this tool {VERSION}");
    }
    let uptime_ms = r.u64()?;
    let reset_cause = r.u32()?;
    let link_state = r.u8()?;
    let channel = r.u8()?;
    let own = r.bytes::<3>()?;
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
        merges: r.u32()?,
        surveys: r.u32()?,
    };
    Ok(Status {
        uptime_ms,
        reset_cause,
        link_state,
        channel,
        own: (own != [0; 3]).then_some(own),
        keyboard_id,
        last_rssi,
        last_rx_age_ms: (age != u32::MAX).then_some(age),
        usb_live: usb & 1 != 0,
        usb_suspended: usb & 2 != 0,
        counters,
        minute_next: r.u32()?,
        minute_oldest: r.u32()?,
        events_total: r.u32()?,
        events_oldest: r.u32()?,
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub(crate) fn fixture(name: &str) -> Vec<u8> {
        let path = format!("{}/fixtures/{name}", env!("CARGO_MANIFEST_DIR"));
        std::fs::read_to_string(path)
            .unwrap()
            .split_whitespace()
            .map(|b| u8::from_str_radix(b, 16).unwrap())
            .collect()
    }

    #[test]
    fn request_carries_command_seq_and_arg() {
        let r = request(Command::History, 9, 0x1234_5678);
        assert_eq!(&r[..6], &[2, 9, 0x78, 0x56, 0x34, 0x12]);
        assert!(r[6..].iter().all(|&b| b == 0));
    }

    #[test]
    fn status_fixture_decodes() {
        let s = decode_status(&fixture("status.hex")).unwrap();
        assert_eq!(s.uptime_ms, 0x01_0203_0405);
        assert_eq!(s.reset_cause, 1);
        assert_eq!(s.link_state, 4);
        assert_eq!(s.channel, 26);
        assert_eq!(s.own, Some([26, 30, 34]));
        assert_eq!(s.keyboard_id, [0x70, 0x15, 0xa3, 0x29]);
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
                merges: 6,
                surveys: 7
            }
        );
        assert_eq!((s.minute_next, s.minute_oldest), (1500, 60));
        assert_eq!((s.events_total, s.events_oldest), (600, 88));
    }

    #[test]
    fn another_protocol_version_is_refused() {
        let mut payload = fixture("status.hex");
        payload[0] = 2;
        assert!(decode_status(&payload).is_err());
    }

    #[test]
    fn short_payload_is_refused() {
        assert!(decode_status(&fixture("status.hex")[..40]).is_err());
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
                code: 1
            })
        );
    }
}
