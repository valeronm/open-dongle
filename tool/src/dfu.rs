use std::io::{ErrorKind, Read, Write};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};

const START: u32 = 3;
const INIT: u32 = 1;
const DATA: u32 = 4;
const STOP: u32 = 5;
const MODE_APPLICATION: u32 = 4;
const DATA_SIZE: usize = 512;
const SLIP_END: u8 = 0xC0;
const SLIP_ESC: u8 = 0xDB;
const SLIP_ESC_END: u8 = 0xDC;
const SLIP_ESC_ESC: u8 = 0xDD;
const HCI_PACKET_TYPE: u8 = 14;
const DATA_INTEGRITY_CHECK: u8 = 1 << 6;
const RELIABLE_PACKET: u8 = 1 << 7;
const PAGE_SIZE: usize = 4096;
const PACKETS_PER_PAGE: usize = PAGE_SIZE / DATA_SIZE;
const PAGE_ERASE: Duration = Duration::from_micros(89_700);
const PAGE_WRITE: Duration = Duration::from_micros(102_400);
const ACK_TIMEOUT: Duration = Duration::from_secs(1);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Seq(u8);

impl Seq {
    /// nrfutil numbers the first packet of an upload 1.
    const FIRST: Seq = Seq(1);

    fn next(self) -> Seq {
        Seq((self.0 + 1) % 8)
    }
}

fn crc16(data: &[u8]) -> u16 {
    data.iter().fold(0xffff, |crc: u16, &b| {
        let mut crc = crc.swap_bytes() ^ u16::from(b);
        crc ^= (crc & 0xff) >> 4;
        crc ^= crc << 12;
        crc ^ ((crc & 0xff) << 5)
    })
}

/// One SLIP-framed packet as the Adafruit bootloader expects it: a reliable packet with a
/// data integrity check, acknowledging `seq + 1`.
fn frame(seq: Seq, payload: &[u8]) -> Vec<u8> {
    let len = payload.len();
    let mut packet = vec![
        seq.0 | seq.next().0 << 3 | DATA_INTEGRITY_CHECK | RELIABLE_PACKET,
        HCI_PACKET_TYPE | ((len & 0x0f) as u8) << 4,
        ((len >> 4) & 0xff) as u8,
    ];
    let sum = packet.iter().fold(0u8, |s, &b| s.wrapping_add(b));
    packet.push(sum.wrapping_neg());
    packet.extend_from_slice(payload);
    let crc = crc16(&packet);
    packet.extend_from_slice(&crc.to_le_bytes());
    let mut out = vec![SLIP_END];
    for b in packet {
        match b {
            SLIP_END => out.extend([SLIP_ESC, SLIP_ESC_END]),
            SLIP_ESC => out.extend([SLIP_ESC, SLIP_ESC_ESC]),
            b => out.push(b),
        }
    }
    out.push(SLIP_END);
    out
}

fn init_header(image: &[u8]) -> Vec<u8> {
    [
        0x0052u16.to_le_bytes().as_slice(),
        0xffffu16.to_le_bytes().as_slice(),
        0xffff_ffffu32.to_le_bytes().as_slice(),
        1u16.to_le_bytes().as_slice(),
        0xfffeu16.to_le_bytes().as_slice(),
        crc16(image).to_le_bytes().as_slice(),
    ]
    .concat()
}

/// The payloads of one application upload, in order: start, init header, data, stop.
fn payloads(image: &[u8]) -> Vec<Vec<u8>> {
    let words = |ws: &[u32]| ws.iter().flat_map(|w| w.to_le_bytes()).collect::<Vec<u8>>();
    let mut out = vec![
        words(&[START, MODE_APPLICATION, 0, 0, image.len() as u32]),
        [words(&[INIT]), init_header(image), vec![0, 0]].concat(),
    ];
    out.extend(
        image
            .chunks(DATA_SIZE)
            .map(|c| [words(&[DATA]), c.to_vec()].concat()),
    );
    out.push(words(&[STOP]));
    out
}

fn erase_time(len: usize) -> Duration {
    (PAGE_ERASE * (len / PAGE_SIZE + 1) as u32).max(Duration::from_millis(500))
}

fn read_ack<P: Read>(port: &mut P) -> Result<u8> {
    let deadline = Instant::now() + ACK_TIMEOUT;
    let mut frame = Vec::new();
    let mut open = false;
    let mut byte = [0u8; 1];
    loop {
        if Instant::now() > deadline {
            bail!("the bootloader did not answer within {ACK_TIMEOUT:?}");
        }
        match port.read(&mut byte) {
            Ok(0) => {}
            Ok(_) if byte[0] == SLIP_END => {
                if !frame.is_empty() {
                    break;
                }
                open = true;
            }
            Ok(_) if open => frame.push(byte[0]),
            Ok(_) => {}
            Err(e)
                if matches!(
                    e.kind(),
                    ErrorKind::TimedOut | ErrorKind::WouldBlock | ErrorKind::Interrupted
                ) => {}
            Err(e) => return Err(e).context("reading from the bootloader"),
        }
    }
    let first = match frame.as_slice() {
        [SLIP_ESC, SLIP_ESC_END, ..] => SLIP_END,
        [SLIP_ESC, SLIP_ESC_ESC, ..] => SLIP_ESC,
        [b, ..] => *b,
        [] => bail!("the bootloader sent an empty acknowledgment"),
    };
    Ok((first >> 3) & 7)
}

/// Sends `image` as an application upload and returns once the bootloader has had time to
/// activate it; `wait` sleeps for the flash erase and write times nrfutil allows.
pub fn upload<P: Read + Write>(
    port: &mut P,
    image: &[u8],
    wait: &mut dyn FnMut(Duration),
    progress: &mut dyn FnMut(usize, usize),
) -> Result<()> {
    let payloads = payloads(image);
    let data = 2..payloads.len() - 1;
    let mut seq = Seq::FIRST;
    for (i, payload) in payloads.iter().enumerate() {
        port.write_all(&frame(seq, payload))
            .context("writing to the bootloader")?;
        let ack = read_ack(port)?;
        let due = seq.next();
        if ack != due.0 {
            bail!(
                "the bootloader acknowledged packet {ack} where {} was due",
                due.0
            );
        }
        seq = due;
        if i == 0 {
            wait(erase_time(image.len()));
        }
        if data.contains(&i) {
            let n = i - data.start;
            progress(n + 1, data.len());
            if n % PACKETS_PER_PAGE == 0 {
                wait(PAGE_WRITE);
            }
            if n + 1 == data.len() {
                wait(PAGE_WRITE);
            }
        }
    }
    wait(PAGE_ERASE + PAGE_WRITE);
    Ok(())
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::protocol::tests::fixture_lines;

    pub(crate) fn test_image() -> Vec<u8> {
        (0..1100u32).map(|i| ((i * 7 + 3) & 0xff) as u8).collect()
    }

    #[test]
    fn crc_matches_nrfutil() {
        assert_eq!(crc16(&test_image()), 0x9c17);
    }

    #[test]
    fn the_sequence_wraps_from_7_to_0() {
        assert_eq!(Seq(7).next(), Seq(0));
        assert_eq!(frame(Seq(7), &[])[1], 0xC7);
    }

    #[test]
    fn frames_match_nrfutil() {
        let expected = fixture_lines("dfu-frames.hex");
        let mut seq = Seq::FIRST;
        let frames: Vec<_> = payloads(&test_image())
            .iter()
            .map(|p| {
                let framed = frame(seq, p);
                seq = seq.next();
                framed
            })
            .collect();
        assert_eq!(frames, expected);
    }

    use std::collections::VecDeque;
    use std::io;

    struct FakeBootloader {
        sent: Vec<u8>,
        replies: VecDeque<u8>,
        ack: fn(u8) -> Option<u8>,
        leading_delimiter: bool,
        idle: io::ErrorKind,
    }

    impl FakeBootloader {
        fn new(ack: fn(u8) -> Option<u8>) -> Self {
            Self {
                sent: Vec::new(),
                replies: VecDeque::new(),
                ack,
                leading_delimiter: false,
                idle: io::ErrorKind::TimedOut,
            }
        }
    }

    impl io::Write for FakeBootloader {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            self.sent.extend_from_slice(buf);
            if let Some(ack) = (self.ack)(buf[1] & 7) {
                if self.leading_delimiter {
                    self.replies.push_back(SLIP_END);
                }
                self.replies.extend(frame(Seq((ack + 7) % 8), &[]));
            }
            Ok(buf.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    impl io::Read for FakeBootloader {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            match self.replies.pop_front() {
                Some(b) => {
                    buf[0] = b;
                    Ok(1)
                }
                None => Err(self.idle.into()),
            }
        }
    }

    #[test]
    fn upload_sends_nrfutils_frames_and_waits_for_the_flash() {
        let mut port = FakeBootloader::new(|seq| Some((seq + 1) % 8));
        let mut waits = Vec::new();
        let mut progress = Vec::new();
        upload(
            &mut port,
            &test_image(),
            &mut |d| waits.push(d),
            &mut |n, t| progress.push((n, t)),
        )
        .unwrap();
        assert_eq!(port.sent, fixture_lines("dfu-frames.hex").concat());
        assert_eq!(
            waits,
            [
                Duration::from_millis(500),
                PAGE_WRITE,
                PAGE_WRITE,
                PAGE_ERASE + PAGE_WRITE
            ]
        );
        assert_eq!(progress, [(1, 3), (2, 3), (3, 3)]);
    }

    #[test]
    fn a_wrong_acknowledgment_stops_the_upload() {
        let mut port = FakeBootloader::new(Some);
        let err = upload(&mut port, &test_image(), &mut |_| {}, &mut |_, _| {}).unwrap_err();
        assert!(err.to_string().contains("acknowledged"), "{err}");
    }

    #[test]
    fn a_silent_bootloader_stops_the_upload() {
        let mut port = FakeBootloader::new(|_| None);
        let err = upload(&mut port, &test_image(), &mut |_| {}, &mut |_, _| {}).unwrap_err();
        assert!(err.to_string().contains("did not answer"), "{err}");
    }

    #[test]
    fn a_silent_bootloader_that_would_block_stops_the_upload() {
        let mut port = FakeBootloader::new(|_| None);
        port.idle = io::ErrorKind::WouldBlock;
        let err = upload(&mut port, &test_image(), &mut |_| {}, &mut |_, _| {}).unwrap_err();
        assert!(err.to_string().contains("did not answer"), "{err}");
    }

    #[test]
    fn a_doubled_frame_delimiter_is_skipped() {
        let mut port = FakeBootloader::new(|seq| Some((seq + 1) % 8));
        port.leading_delimiter = true;
        upload(&mut port, &test_image(), &mut |_| {}, &mut |_, _| {}).unwrap();
    }
}
