use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};

use crate::device::{self, dongles};
use crate::protocol::Action;

const ADAFRUIT: u16 = 0x239a;
const BOOTLOADER: u16 = 0x00b3;
const FACTORY_PROGRAM: u16 = 0x8029;
const POLL: Duration = Duration::from_millis(200);
const BOOTLOADER_WAIT: Duration = Duration::from_secs(10);
const BOOTLOADER_SETTLE: Duration = Duration::from_secs(3);
const ACCESS_RETRY: Duration = Duration::from_millis(100);
const ACCESS_WAIT: Duration = Duration::from_secs(2);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Dongle,
    Bootloader,
    Factory,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Kind {
    Dongle,
    Bootloader(String),
    Factory(String),
}

impl Kind {
    pub fn state(&self) -> State {
        match self {
            Kind::Dongle => State::Dongle,
            Kind::Bootloader(_) => State::Bootloader,
            Kind::Factory(_) => State::Factory,
        }
    }
}

/// A board as listed: its USB serial may be missing.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Found {
    pub serial: Option<String>,
    pub kind: Kind,
}

/// A board the tool can act on, followed across restarts by its USB serial.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Board {
    pub serial: String,
    pub kind: Kind,
}

fn port_state(vid: u16, pid: u16) -> Option<State> {
    match (vid, pid) {
        (ADAFRUIT, BOOTLOADER) => Some(State::Bootloader),
        (ADAFRUIT, FACTORY_PROGRAM) => Some(State::Factory),
        _ => None,
    }
}

/// macOS lists each USB port twice; the `tty.` side blocks on open until carrier detect.
fn usable_port(name: &str) -> bool {
    !name.starts_with("/dev/tty.")
}

/// Every board this tool acts on: the running dongle through its diagnostic interface, boards
/// in their bootloader or factory program through their serial ports.
pub fn find() -> Result<Vec<Found>> {
    let api = hidapi::HidApi::new().context("starting hidapi")?;
    let mut found: Vec<Found> = dongles(&api)
        .map(|d| Found {
            serial: d.serial_number().map(str::to_string),
            kind: Kind::Dongle,
        })
        .collect();
    for p in serialport::available_ports().context("listing serial ports")? {
        let serialport::SerialPortType::UsbPort(usb) = p.port_type else {
            continue;
        };
        if !usable_port(&p.port_name) {
            continue;
        }
        let kind = match port_state(usb.vid, usb.pid) {
            Some(State::Bootloader) => Kind::Bootloader(p.port_name),
            Some(State::Factory) => Kind::Factory(p.port_name),
            _ => continue,
        };
        found.push(Found {
            serial: usb.serial_number,
            kind,
        });
    }
    Ok(found)
}

pub fn pick(found: Vec<Found>, serial: Option<&str>) -> Result<Board> {
    let mut found: Vec<_> = found
        .into_iter()
        .filter(|f| serial.is_none_or(|s| f.serial.as_deref() == Some(s)))
        .collect();
    match found.len() {
        0 => bail!(
            "no board found: plug in an OpenDongle, a board in its bootloader, or a new board \
             running its factory program; any other board enters its bootloader when RST is \
             shorted to GND twice quickly"
        ),
        1 => {
            let Found { serial, kind } = found.remove(0);
            match serial {
                Some(serial) => Ok(Board { serial, kind }),
                None => bail!(
                    "the board at {} reports no USB serial number, so it can't be followed \
                     through its restart",
                    location(&kind)
                ),
            }
        }
        n => {
            let names: Vec<_> = found
                .iter()
                .map(|f| match &f.serial {
                    Some(s) => s.clone(),
                    None => format!("(no serial, {})", location(&f.kind)),
                })
                .collect();
            bail!(
                "{n} boards found; pick one with --serial: {}",
                names.join(", ")
            )
        }
    }
}

fn location(kind: &Kind) -> &str {
    match kind {
        Kind::Dongle => "the diagnostic interface",
        Kind::Bootloader(port) | Kind::Factory(port) => port,
    }
}

pub fn wait_for(serial: &str, state: State, timeout: Duration) -> Result<Board> {
    let deadline = Instant::now() + timeout;
    loop {
        let hit = find()?
            .into_iter()
            .find(|f| f.serial.as_deref() == Some(serial) && f.kind.state() == state);
        if let Some(f) = hit {
            return Ok(Board {
                serial: serial.to_string(),
                kind: f.kind,
            });
        }
        if Instant::now() > deadline {
            bail!("the board {serial} did not appear {state:?} within {timeout:?}");
        }
        std::thread::sleep(POLL);
    }
}

/// Restarts `board` into its bootloader if it is not there yet, and returns the bootloader's
/// serial port.
pub fn into_bootloader(board: &Board) -> Result<String> {
    let port = match &board.kind {
        Kind::Bootloader(port) => port.clone(),
        Kind::Dongle => {
            device::open_unchecked(Some(&board.serial))?.action(Action::Bootloader)?;
            bootloader_port(
                &board.serial,
                "the dongle did not restart into its bootloader",
            )?
        }
        Kind::Factory(port) => {
            // The Arduino core restarts into the bootloader when its port is opened at 1200 baud.
            drop(open_port(port, 1200)?);
            bootloader_port(
                &board.serial,
                "the board did not restart into its bootloader; short RST to GND twice and run flash again",
            )?
        }
    };
    // The bootloader stops answering if its drive is read during an upload, and the host scans
    // the drive 1-2 s after the bootloader starts.
    std::thread::sleep(BOOTLOADER_SETTLE);
    Ok(port)
}

fn bootloader_port(serial: &str, failure: &'static str) -> Result<String> {
    match wait_for(serial, State::Bootloader, BOOTLOADER_WAIT)
        .context(failure)?
        .kind
    {
        Kind::Bootloader(port) => Ok(port),
        other => bail!(
            "the board {serial} appeared as {:?}, not in its bootloader",
            other.state()
        ),
    }
}

pub fn open_port(path: &str, baud: u32) -> Result<Box<dyn serialport::SerialPort>> {
    let deadline = Instant::now() + ACCESS_WAIT;
    loop {
        match serialport::new(path, baud)
            .timeout(Duration::from_millis(100))
            .open()
        {
            Ok(port) => return Ok(port),
            // udev grants access to a new port a moment after the port appears.
            Err(e)
                if e.kind() == serialport::ErrorKind::Io(std::io::ErrorKind::PermissionDenied) =>
            {
                if Instant::now() > deadline {
                    bail!(
                        "no access to {path}: install ~/.local/share/open-dongle/70-open-dongle.rules \
                         into /etc/udev/rules.d, or run \
                         sudo setfacl -m u:$USER:rw {path}"
                    );
                }
                std::thread::sleep(ACCESS_RETRY);
            }
            Err(e) => return Err(anyhow::Error::new(e).context(format!("opening {path}"))),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn found(kind: Kind, serial: Option<&str>) -> Found {
        Found {
            serial: serial.map(str::to_string),
            kind,
        }
    }

    #[test]
    fn only_the_bootloader_and_the_factory_program_are_taken_from_serial_ports() {
        assert_eq!(port_state(0x239a, 0x00b3), Some(State::Bootloader));
        assert_eq!(port_state(0x239a, 0x8029), Some(State::Factory));
        assert_eq!(port_state(0x239a, 0x0029), None);
        assert_eq!(port_state(0x2fe3, 0x4e44), None);
    }

    #[test]
    fn mac_dial_in_ports_are_skipped() {
        assert!(usable_port("/dev/ttyACM0"));
        assert!(usable_port("/dev/cu.usbmodem1101"));
        assert!(!usable_port("/dev/tty.usbmodem1101"));
    }

    #[test]
    fn kind_reports_its_state() {
        assert_eq!(Kind::Dongle.state(), State::Dongle);
        assert_eq!(
            Kind::Bootloader("/dev/ttyACM0".into()).state(),
            State::Bootloader
        );
        assert_eq!(Kind::Factory("/dev/ttyACM0".into()).state(), State::Factory);
    }

    #[test]
    fn one_board_is_picked() {
        let f = found(Kind::Bootloader("/dev/ttyACM0".into()), Some("AB"));
        let b = pick(vec![f.clone()], None).unwrap();
        assert_eq!(b.serial, "AB");
        assert_eq!(b.kind, f.kind);
    }

    #[test]
    fn no_board_names_the_double_reset() {
        let err = pick(vec![], None).unwrap_err();
        assert!(err.to_string().contains("RST"), "{err}");
    }

    #[test]
    fn a_serial_that_matches_nothing_finds_no_board() {
        let err = pick(vec![found(Kind::Dongle, Some("AA"))], Some("ZZ")).unwrap_err();
        assert!(err.to_string().contains("no board found"), "{err}");
    }

    #[test]
    fn a_board_without_a_serial_is_refused() {
        let err = pick(
            vec![found(Kind::Factory("/dev/ttyACM0".into()), None)],
            None,
        )
        .unwrap_err();
        assert!(err.to_string().contains("serial number"), "{err}");
        assert!(err.to_string().contains("/dev/ttyACM0"), "{err}");
    }

    #[test]
    fn several_boards_need_a_serial() {
        let list = vec![
            found(Kind::Dongle, Some("AA")),
            found(Kind::Factory("/dev/ttyACM1".into()), Some("BB")),
            found(Kind::Bootloader("/dev/ttyACM2".into()), None),
        ];
        let err = pick(list.clone(), None).unwrap_err();
        assert!(
            err.to_string()
                .contains("AA, BB, (no serial, /dev/ttyACM2)"),
            "{err}"
        );
        assert_eq!(pick(list, Some("BB")).unwrap().kind.state(), State::Factory);
    }
}
