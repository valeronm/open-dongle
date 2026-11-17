use std::io::Write;

use std::path::Path;
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use chrono::Local;

use crate::board::{self, State};
use crate::device::{self, Device, Refused, Transport};
use crate::format::{
    NOISE_QUESTION, action_question, channel_description, describe_event, flash_question, flashed,
    hex, last_packet, noise_level, paired, pairing_offer, reset_causes, state_name, status_label,
    unifying_kind, uptime, usb_state, version, wall_clock,
};
use crate::protocol::{
    Action, Counter, ErrorCode, Link, Minute, NOISE_BUCKETS, NoiseChannel, Protocol, Status, Watch,
};
use crate::{dfu, format, uf2};

pub fn status(s: &Status) -> Result<()> {
    for line in status_lines(s) {
        println!("{line}");
    }
    Ok(())
}

fn status_lines(s: &Status) -> Vec<String> {
    let c = s.counters;
    let name = s.link.keyboard_name().unwrap_or("unnamed");
    let (link, offer, keyboard) = match &s.link {
        Link::Unifying(unifying) => {
            let row = unifying.as_ref().map(|k| {
                let (label, kind) = match unifying_kind(k.kind) {
                    Some(kind) => (kind, String::new()),
                    None => ("device", format!(", type {:#04x}", k.kind)),
                };
                let value = format!(
                    "{name}, serial {}, WPID {}{kind}",
                    hex(&k.serial),
                    hex(&k.wpid)
                );
                (label, value)
            });
            (String::new(), String::new(), row)
        }
        Link::Nuphy(nuphy) => {
            let id = nuphy.keyboard_id.map(|id| format!(", ID {}", hex(&id)));
            let row = (nuphy.keyboard_name.is_some() || id.is_some())
                .then(|| ("keyboard", format!("{name}{}", id.unwrap_or_default())));
            (
                format!(" ({})", paired(nuphy.own)),
                format!(", {}", pairing_offer(nuphy.forced)),
                row,
            )
        }
        Link::Bluetooth(_) => {
            let row = s.link.keyboard_name().map(|n| ("keyboard", n.to_string()));
            (String::new(), String::new(), row)
        }
    };
    let channel_and_signal = s.protocol().has_channel_and_signal();
    let counters: Vec<String> = s
        .protocol()
        .counters()
        .iter()
        .filter_map(|&counter| Some(format!("{} {}", counter.name(), c.get(counter)?)))
        .collect();
    let mut rows = vec![
        ("firmware", version(s.firmware_version)),
        ("protocol", s.protocol().name().to_string()),
        (
            "link",
            if channel_and_signal {
                format!("{} on channel {}{link}", state_name(s), s.channel)
            } else {
                state_name(s).to_string()
            },
        ),
    ];
    rows.extend(keyboard);
    if channel_and_signal {
        rows.push((
            "signal",
            format!("{} dBm, {}", s.last_rssi, last_packet(s.last_rx_age_ms)),
        ));
    }
    rows.extend([
        (
            "pairing",
            format!("{}{offer}", if s.pairing_open { "open" } else { "closed" }),
        ),
        ("usb", usb_state(s.usb_live, s.usb_suspended).to_string()),
        (
            "uptime",
            format!(
                "{}, reset by {}",
                uptime(s.uptime_ms),
                reset_causes(s.reset_cause)
            ),
        ),
        ("counters", counters.join(" ")),
        (
            "history",
            format!(
                "{} minutes, {} events held",
                s.minute_next.saturating_sub(s.minute_oldest),
                s.events_total.saturating_sub(s.events_oldest)
            ),
        ),
    ]);
    rows.into_iter()
        .map(|(label, value)| status_label(label) + &value)
        .collect()
}

pub fn require_nuphy(s: &Status, what: &str) -> Result<()> {
    match s.protocol() {
        Protocol::Nuphy => Ok(()),
        other => bail!(
            "only NuPhy mode has {what}; the dongle runs {} (open-dongle protocol nuphy switches it)",
            other.name()
        ),
    }
}

/// Opens the dongle for a command that only the NuPhy protocol has, such as forced channels.
pub fn open_nuphy(
    serial: Option<&str>,
    what: &str,
) -> Result<(Device<device::HidTransport>, Status)> {
    let (dev, s) = device::open(serial)?;
    require_nuphy(&s, what)?;
    Ok((dev, s))
}

/// Opens pairing and returns the line that says what to do at the keyboard.
pub fn pair<T: Transport>(
    dev: &mut Device<T>,
    protocol: Protocol,
    no_passkey: bool,
) -> Result<String> {
    if no_passkey && protocol != Protocol::Bluetooth {
        bail!(
            "--no-passkey applies only while the dongle runs Bluetooth; it runs {}",
            protocol.name()
        );
    }
    let passkey = dev.pair(no_passkey)?;
    Ok(format::pairing_open(protocol, passkey))
}

pub fn keyboards<T: Transport>(dev: &mut Device<T>) -> Result<()> {
    for line in format::keyboard_lines(&dev.keyboards()?) {
        println!("{line}");
    }
    Ok(())
}

/// `place` is the keyboard's number in the list `keyboards` prints, from 1.
pub fn forget<T: Transport>(dev: &mut Device<T>, place: usize, yes: bool) -> Result<()> {
    let list = dev.keyboards()?;
    let Some(keyboard) = place.checked_sub(1).and_then(|i| list.get(i)) else {
        bail!(
            "there is no keyboard {place}; `open-dongle keyboards` lists the {} stored",
            list.len()
        );
    };
    if confirm(&format::forget_question(keyboard), yes)? {
        dev.forget(keyboard)?;
        println!("forgotten");
    }
    Ok(())
}

/// Moves the link and returns the line that reports it, with the dongle's refusals explained.
pub fn move_link<T: Transport>(dev: &mut Device<T>, s: &Status, channel: u8) -> Result<String> {
    let from = dev.move_to(channel).map_err(|err| {
        match err.downcast_ref::<Refused>().map(|r| r.code) {
            Some(Ok(ErrorCode::BadChannels)) => anyhow!(format::move_refused(s)),
            _ => err,
        }
    })?;
    Ok(format::moved(from, channel))
}

pub fn protocol(serial: Option<&str>, set: Option<Protocol>) -> Result<()> {
    let (mut dev, before) = device::open(serial)?;
    let Some(want) = set else {
        println!("the dongle runs {}", before.protocol().name());
        return Ok(());
    };
    let (running, rebooting) = dev.protocol(Some(want))?;
    if !rebooting {
        println!("the dongle already runs {}", running.name());
        return Ok(());
    }
    drop(dev);
    let deadline = std::time::Instant::now() + RETURN_WAIT;
    std::thread::sleep(Duration::from_secs(1));
    let s = loop {
        std::thread::sleep(Duration::from_millis(300));
        if let Ok(Ok(s)) = device::open_unchecked(serial).and_then(|mut d| d.read_status())
            && s.uptime_ms < before.uptime_ms
        {
            break s;
        }
        if std::time::Instant::now() > deadline {
            bail!("the dongle did not come back within {RETURN_WAIT:?}; replug it");
        }
    };
    if s.protocol() != want {
        bail!("the dongle came back running {}", s.protocol().name());
    }
    status(&s)
}

fn minute_line(protocol: Protocol, m: &Minute) -> String {
    let counters: Vec<String> = protocol
        .counters()
        .iter()
        .filter_map(|&counter| {
            let width = if counter == Counter::Packets { 5 } else { 3 };
            m.get(counter)
                .map(|n| format!("{} {n:width$}", counter.name()))
        })
        .collect();
    let signal = m
        .rssi
        .map_or("-".into(), |r| format!("{}/{}/{}", r.min, r.avg, r.max));
    format!(
        "{}  signal {signal}  {}",
        counters.join("  "),
        format::noise(m.noise)
    )
}

pub fn history<T: Transport>(dev: &mut Device<T>, s: &Status, minutes: u32) -> Result<()> {
    let now = Local::now();
    let from = s.minute_next.saturating_sub(minutes);
    for (index, m) in dev.minutes_between(from, s.minute_next, s.protocol())? {
        let at = wall_clock(now, s.uptime_ms, u64::from(index) * 60_000);
        println!(
            "{}  {}",
            at.format("%Y-%m-%d %H:%M:%S"),
            minute_line(s.protocol(), &m)
        );
    }
    Ok(())
}

pub fn events<T: Transport>(dev: &mut Device<T>, s: &Status, count: u32) -> Result<()> {
    let now = Local::now();
    let from = s.events_total.saturating_sub(count);
    for (_, e) in dev.events_between(from, s.events_total)? {
        let at = wall_clock(now, s.uptime_ms, e.time_ms);
        println!(
            "{}  {}",
            at.format("%Y-%m-%d %H:%M:%S%.3f"),
            describe_event(&e, &s.link)
        );
    }
    Ok(())
}

/// Reads `auto` as `None` and three channel numbers as `Some`.
pub fn parse_channels(values: &[String]) -> Result<Option<[u8; 3]>> {
    if let [only] = values
        && only == "auto"
    {
        return Ok(None);
    }
    let channels: Vec<u8> = values
        .iter()
        .map(|v| v.parse::<u8>())
        .collect::<Result<_, _>>()
        .context("channels are numbers from 2 to 80")?;
    let channels: [u8; 3] = channels
        .try_into()
        .map_err(|_| anyhow!("give three channels, or auto"))?;
    Ok(Some(channels))
}

pub fn channels<T: Transport>(dev: &mut Device<T>, channels: Option<[u8; 3]>) -> Result<()> {
    dev.force_channels(channels).map_err(|err| {
        match err.downcast_ref::<Refused>().map(|r| r.code) {
            Some(Ok(ErrorCode::BadChannels)) => anyhow!(
                "the dongle refused the channels: give three different channels from 2 to 80, \
                 none of them a pairing channel (2, 50, 80)"
            ),
            Some(Ok(ErrorCode::Store)) => {
                anyhow!("the dongle could not save the channels; nothing changed")
            }
            _ => err,
        }
    })?;
    match channels {
        Some([a, b, c]) => println!(
            "every pairing now offers channels {a} {b} {c}; hold Fn+4 on the keyboard to pair onto them"
        ),
        None => println!("pairing picks its channels by survey again"),
    }
    Ok(())
}

/// Returns true when `yes` is set or the user answers y.
pub fn confirm(question: &str, yes: bool) -> Result<bool> {
    if yes {
        return Ok(true);
    }
    print!("{question} [y/N] ");
    std::io::stdout().flush()?;
    let mut line = String::new();
    std::io::stdin().read_line(&mut line)?;
    Ok(matches!(line.trim(), "y" | "Y" | "yes"))
}

pub fn noise<T: Transport>(dev: &mut Device<T>, s: &Status, yes: bool) -> Result<()> {
    if !confirm(NOISE_QUESTION, yes)? {
        return Ok(());
    }
    for line in noise_lines(&dev.noise()?, s) {
        println!("{line}");
    }
    Ok(())
}

fn noise_lines(noise: &[NoiseChannel], s: &Status) -> Vec<String> {
    let reachable = s.keyboard_channels();
    let rows: Vec<_> = noise
        .iter()
        .map(|c| {
            let mut description = channel_description(c.channel, s);
            if reachable.contains(&c.channel) {
                if !description.is_empty() {
                    description.push_str(", ");
                }
                description.push_str("move target");
            }
            (description, c)
        })
        .collect();
    let width = rows
        .iter()
        .map(|(d, _)| d.chars().count())
        .max()
        .unwrap_or(0);
    rows.into_iter()
        .map(|(description, c)| {
            let bar = "#".repeat(usize::from(noise_level(c.loudest)) / 2);
            format!(
                "{:3}  {description:<width$}  {:4} dBm  avg {:4}  {bar}",
                c.channel, c.loudest, c.average
            )
        })
        .collect()
}

pub fn moves<T: Transport>(dev: &mut Device<T>, s: &Status) -> Result<()> {
    for line in moves_lines(&dev.watch()?, s) {
        println!("{line}");
    }
    Ok(())
}

fn moves_lines(w: &Watch, s: &Status) -> Vec<String> {
    let rows: Vec<_> = w
        .channels
        .iter()
        .map(|c| {
            (
                format::described(c.channel, format::watch_role(c.channel, w, s)),
                c,
            )
        })
        .collect();
    let width = rows
        .iter()
        .map(|(d, _)| d.chars().count())
        .max()
        .unwrap_or(0);
    let labels = (0..NOISE_BUCKETS as u8).map(|b| format!("{:>6}", format::bucket_label(b)));
    let header = format!(
        "{:3}  {:width$}  {:>8}  {:>7}  {}",
        "",
        "",
        "readings",
        "counted",
        labels.collect::<Vec<_>>().join(" ")
    );
    let lines = rows.into_iter().map(|(description, c)| {
        let counted = format::counted_share(w.counted(c));
        let readings = c.readings();
        let spread: Vec<_> = c
            .buckets
            .iter()
            .map(|&b| format!("{:>6}", tenths(b, readings)))
            .collect();
        let line = format!(
            "{:3}  {description:<width$}  {readings:>8}  {counted:>7}  {}",
            c.channel,
            spread.join(" ")
        );
        if w.too_few(c) {
            format!("{line}  too few readings to move to")
        } else {
            line
        }
    });
    [format::watch_summary(w), header]
        .into_iter()
        .chain(lines)
        .collect()
}

fn tenths(part: u32, whole: u64) -> String {
    let t = (u64::from(part) * 2000 + whole) / (2 * whole.max(1));
    format!("{}.{}%", t / 10, t % 10)
}

pub fn action<T: Transport>(dev: &mut Device<T>, action: Action, yes: bool) -> Result<()> {
    if confirm(action_question(action), yes)? {
        dev.action(action)?;
        let done = match action {
            Action::Reboot => "rebooting; the keyboard reconnects by itself",
            Action::Bootloader => "in the bootloader; flash it or replug it",
            Action::Clear => "cleared",
        };
        println!("{done}");
    }
    Ok(())
}

const RETURN_WAIT: Duration = Duration::from_secs(10);

/// Checks `file` before any board is touched, then restarts the board into its bootloader if
/// needed, uploads, and waits for the firmware to report back.
pub fn flash(file: &Path, serial: Option<&str>, yes: bool) -> Result<()> {
    let bytes = std::fs::read(file).with_context(|| format!("reading {}", file.display()))?;
    let image =
        uf2::image(&bytes).with_context(|| format!("{} is not usable firmware", file.display()))?;
    let board = board::pick(board::find()?, serial)?;
    let name = file.file_name().map_or_else(
        || file.display().to_string(),
        |n| n.to_string_lossy().into_owned(),
    );
    if !confirm(
        &flash_question(board.kind.state(), &board.serial, &name),
        yes,
    )? {
        return Ok(());
    }
    let port = board::into_bootloader(&board)?;
    let uploaded = match upload(&port, &image) {
        // A dongle's watchdog cannot be stopped and keeps counting in the bootloader, which
        // does not feed it while it erases; after that restart no watchdog runs.
        Err(e) if e.is::<dfu::LostWhileErasing>() => {
            println!(
                "the firmware's watchdog restarted the bootloader while it erased; uploading again"
            );
            upload(&board::back_in_bootloader(&board.serial)?, &image)
        }
        other => other,
    };
    uploaded.context(
        "the upload broke off; the board stays in its bootloader until it is unplugged and \
         plugged back in, then run flash again",
    )?;
    println!();
    board::wait_for(&board.serial, State::Dongle, RETURN_WAIT).context(
        "the board did not come back after flashing; if a replug doesn't bring it back, this \
         firmware can't run on it",
    )?;
    println!(
        "{}",
        flashed(device::open_unchecked(Some(&board.serial))?.read_status()?)
    );
    Ok(())
}

fn upload(port: &str, image: &[u8]) -> Result<()> {
    let mut bootloader = board::open_port(port, 115_200)?;
    bootloader
        .write_data_terminal_ready(true)
        .context("raising DTR")?;
    let mut shown = None;
    dfu::upload(
        &mut bootloader,
        image,
        &mut std::thread::sleep,
        &mut |done, total| {
            let percent = done * 100 / total;
            if shown != Some(percent) {
                shown = Some(percent);
                print!("\ruploading {percent:3}%");
                let _ = std::io::stdout().flush();
            }
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::device::tests::{Fake, error_part, parts};
    use crate::protocol::tests::{nuphy, status_of, unifying};
    use crate::protocol::{BluetoothLink, Command, Link, Protocol};

    fn strings(values: &[&str]) -> Vec<String> {
        values.iter().map(|v| v.to_string()).collect()
    }

    #[test]
    fn a_bluetooth_status_shows_no_channel_or_signal() {
        let lines = status_lines(&status_of(Protocol::Bluetooth));
        let link = lines.iter().find(|l| l.starts_with("link")).unwrap();
        assert!(link.ends_with("no keyboard"), "{link}");
        assert!(!lines.iter().any(|l| l.starts_with("signal")));
        assert!(
            lines
                .iter()
                .any(|l| l.starts_with("protocol") && l.ends_with("Bluetooth"))
        );
    }

    #[test]
    fn a_connected_bluetooth_keyboard_is_named() {
        let mut s = status_of(Protocol::Bluetooth);
        s.link = Link::Bluetooth(BluetoothLink::Connected {
            name: Some("MX Keys".into()),
        });
        let lines = status_lines(&s);
        let link = lines.iter().find(|l| l.starts_with("link")).unwrap();
        assert!(link.ends_with("connected"), "{link}");
        let keyboard = lines.iter().find(|l| l.starts_with("keyboard")).unwrap();
        assert!(keyboard.ends_with("MX Keys"), "{keyboard}");
    }

    #[test]
    fn a_failed_bluetooth_start_is_reported() {
        let mut s = status_of(Protocol::Bluetooth);
        s.link = Link::Bluetooth(BluetoothLink::StartFailed);
        let lines = status_lines(&s);
        let link = lines.iter().find(|l| l.starts_with("link")).unwrap();
        assert!(link.ends_with("Bluetooth failed to start"), "{link}");
    }

    #[test]
    fn reads_auto_or_three_channels() {
        assert_eq!(parse_channels(&strings(&["auto"])).unwrap(), None);
        assert_eq!(
            parse_channels(&strings(&["62", "52", "44"])).unwrap(),
            Some([62, 52, 44])
        );
        assert!(parse_channels(&strings(&["62", "52"])).is_err());
        assert!(parse_channels(&strings(&["62", "52", "x"])).is_err());
        assert!(parse_channels(&strings(&["62", "52", "300"])).is_err());
        assert!(parse_channels(&strings(&["auto", "52", "44"])).is_err());
    }

    #[test]
    fn noise_labels_the_channels_a_move_can_reach() {
        let s = status_of(Protocol::Nuphy);
        let noise = [16, 17].map(|channel| NoiseChannel {
            channel,
            loudest: -100,
            average: -100,
        });
        assert_eq!(
            noise_lines(&noise, &s),
            [
                " 16  wifi 1, move target  -100 dBm  avg -100  ",
                " 17  wifi 1               -100 dBm  avg -100  ",
            ]
        );
    }

    #[test]
    fn moves_list_each_channel_with_its_counted_share_and_spread() {
        let mut s = status_of(Protocol::Nuphy);
        s.channel = 41;
        let w =
            crate::protocol::decode_watch(&crate::protocol::tests::fixture("watch.hex")).unwrap();
        assert_eq!(
            moves_lines(&w, &s),
            [
                "noise at -75 dBm or louder counts; next move in 30 s at the earliest",
                "                      readings  counted    <-85    -85    -75    -65    -55    -45",
                "  2  wifi 1, ble adv        60    0.00%  100.0%   0.0%   0.0%   0.0%   0.0%   0.0%",
                " 41  wifi 6, current      1236    2.91%   80.9%  16.2%   2.4%   0.4%   0.0%   0.1%",
                " 62  wifi 11                30    0.00%   33.3%  66.7%   0.0%   0.0%   0.0%   0.0%  too few readings to move to",
            ]
        );
    }

    #[test]
    fn moves_mark_the_home_they_report_as_current() {
        let mut s = status_of(Protocol::Nuphy);
        s.channel = 2;
        let w =
            crate::protocol::decode_watch(&crate::protocol::tests::fixture("watch.hex")).unwrap();
        let lines = moves_lines(&w, &s);
        assert!(lines[2].starts_with("  2  wifi 1, ble adv "), "{lines:?}");
        assert!(lines[3].starts_with(" 41  wifi 6, current "), "{lines:?}");
    }

    #[test]
    fn moves_without_a_keyboard_packet_count_nothing() {
        let s = status_of(Protocol::Nuphy);
        let w = Watch {
            home: None,
            threshold: None,
            paused: false,
            hold_left_ms: 0,
            channels: Vec::new(),
        };
        assert_eq!(
            moves_lines(&w, &s)[0],
            "no keyboard link, so the dongle makes no moves"
        );
        let w = Watch {
            home: Some(41),
            ..w
        };
        assert_eq!(
            moves_lines(&w, &s)[0],
            "no recent keyboard packet, so no noise counts"
        );
    }

    fn dongle_refusing_with(command: Command, code: u8) -> Device<Fake> {
        Device::new(
            Fake::new(move |req| {
                assert_eq!(req[0], command as u8);
                vec![error_part(req[0], req[1], code)]
            }),
            0,
        )
    }

    #[test]
    fn forgetting_a_place_outside_the_list_sends_nothing() {
        let list = crate::protocol::tests::fixture("keyboards.hex");
        let mut dev = Device::new(
            Fake::new(move |req| {
                assert_eq!(req[0], Command::Keyboards as u8);
                parts(req[0], req[1], &list)
            }),
            0,
        );
        for place in [0, 3] {
            let err = forget(&mut dev, place, true).unwrap_err().to_string();
            assert!(err.contains("lists the 2 stored"), "{err}");
        }
    }

    #[test]
    fn forgetting_sends_the_address_of_the_keyboard_at_that_place() {
        let list = crate::protocol::tests::fixture("keyboards.hex");
        let sent = std::rc::Rc::new(std::cell::Cell::new(None));
        let seen = sent.clone();
        let mut dev = Device::new(
            Fake::new(move |req| {
                if req[0] == Command::Keyboards as u8 {
                    return parts(req[0], req[1], &list);
                }
                seen.set(Some((req[0], <[u8; 7]>::try_from(&req[6..13]).unwrap())));
                parts(req[0], req[1], &[])
            }),
            0,
        );
        forget(&mut dev, 2, true).unwrap();
        assert_eq!(
            sent.get(),
            Some((
                Command::Forget as u8,
                [1, 0x24, 0xa8, 0x4b, 0x3c, 0x7d, 0xd6]
            ))
        );
    }

    #[test]
    fn pairing_without_a_passkey_is_refused_outside_bluetooth() {
        let mut dev = Device::new(Fake::new(|_| panic!("nothing should be sent")), 0);
        let err = pair(&mut dev, Protocol::Unifying, true)
            .unwrap_err()
            .to_string();
        assert!(
            err.contains("only while the dongle runs Bluetooth"),
            "{err}"
        );
    }

    #[test]
    fn the_pairing_line_carries_the_passkey_the_dongle_sent() {
        let reply = |req: &[u8; 64]| parts(req[0], req[1], &[1, 42, 0, 0, 0]);
        let mut dev = Device::new(Fake::new(reply), 0);
        let line = pair(&mut dev, Protocol::Bluetooth, false).unwrap();
        assert!(line.contains("000042"), "{line}");
    }

    #[test]
    fn forcing_sends_the_channels_to_the_dongle() {
        let sent = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let log = sent.clone();
        let mut dev = Device::new(
            Fake::new(move |req| {
                log.borrow_mut().push(req[..6].to_vec());
                parts(req[0], req[1], &[])
            }),
            0,
        );
        channels(&mut dev, Some([62, 52, 44])).unwrap();
        assert_eq!(
            *sent.borrow(),
            [vec![Command::ForceChannels as u8, 1, 62, 52, 44, 0]]
        );
    }

    #[test]
    fn refused_channels_name_the_rule() {
        let err = channels(
            &mut dongle_refusing_with(Command::ForceChannels, 3),
            Some([62, 50, 44]),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("pairing channel"), "{err}");
    }

    #[test]
    fn a_failed_save_says_nothing_changed() {
        let err = channels(&mut dongle_refusing_with(Command::ForceChannels, 4), None)
            .unwrap_err()
            .to_string();
        assert!(err.contains("nothing changed"), "{err}");
    }

    #[test]
    fn a_move_sends_the_channel_and_reads_where_the_link_was() {
        let sent = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let log = sent.clone();
        let mut dev = Device::new(
            Fake::new(move |req| {
                log.borrow_mut().push(req[..6].to_vec());
                parts(req[0], req[1], &[62])
            }),
            0,
        );
        assert_eq!(dev.move_to(5).unwrap(), 62);
        assert_eq!(*sent.borrow(), [vec![Command::Move as u8, 1, 5, 0, 0, 0]]);
    }

    #[test]
    fn a_refused_channel_names_the_keyboards_channels() {
        let unifying = status_of(Protocol::Unifying);
        let err = move_link(&mut dongle_refusing_with(Command::Move, 3), &unifying, 6)
            .unwrap_err()
            .to_string();
        assert!(
            err.ends_with("give one of 5 8 17 32 35 41 44 62 65 71 74"),
            "{err}"
        );
        let nuphy = status_of(Protocol::Nuphy);
        let err = move_link(&mut dongle_refusing_with(Command::Move, 3), &nuphy, 6)
            .unwrap_err()
            .to_string();
        assert!(
            err.ends_with("give one of 16 22 26 30 34 44 48 58 72 78"),
            "{err}"
        );
    }

    #[test]
    fn a_refused_move_says_what_it_needs() {
        let err = move_link(
            &mut dongle_refusing_with(Command::Move, 6),
            &status_of(Protocol::Unifying),
            5,
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("pairing closed"), "{err}");
    }

    #[test]
    fn a_move_refused_for_a_held_key_says_so() {
        let err = move_link(
            &mut dongle_refusing_with(Command::Move, 7),
            &status_of(Protocol::Nuphy),
            44,
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("release every key"), "{err}");
    }

    #[test]
    fn a_bad_file_stops_before_any_board_is_looked_for() {
        let path =
            std::env::temp_dir().join(format!("open-dongle-not-uf2-{}.bin", std::process::id()));
        std::fs::write(&path, [0u8; 100]).unwrap();
        let err = flash(&path, Some("no-such-board"), true).unwrap_err();
        std::fs::remove_file(&path).unwrap();
        assert!(format!("{err:#}").contains("UF2"), "{err:#}");
    }

    #[test]
    fn nuphy_only_commands_name_the_running_protocol() {
        assert!(require_nuphy(&status_of(Protocol::Nuphy), "forced channels").is_ok());
        let err = require_nuphy(&status_of(Protocol::Unifying), "forced channels")
            .unwrap_err()
            .to_string();
        assert_eq!(
            err,
            "only NuPhy mode has forced channels; the dongle runs Unifying (open-dongle protocol nuphy switches it)"
        );
    }

    #[test]
    fn prints_a_nuphy_status() {
        let s = status_of(Protocol::Nuphy);
        assert_eq!(
            status_lines(&s),
            [
                "firmware  1.2.3",
                "protocol  NuPhy",
                "link      connected on channel 26 (paired 26 30 34)",
                "keyboard  MX Keys, ID 7015a329",
                "signal    -71 dBm, last packet 12 ms ago",
                "pairing   open, offers channels 62 52 44, set by hand",
                "usb       live",
                "uptime    50d 02:25:19, reset by pin",
                "counters  packets 1000 invalid 1 dropped 2 channels 3 losses 4 gaps 8 usb-retries 5 merges 6 surveys 7",
                "history   1440 minutes, 512 events held",
            ]
        );
    }

    #[test]
    fn prints_closed_pairing_in_a_nuphy_status() {
        let mut s = status_of(Protocol::Nuphy);
        s.pairing_open = false;
        assert!(
            status_lines(&s)
                .contains(&"pairing   closed, offers channels 62 52 44, set by hand".to_string())
        );
    }

    #[test]
    fn prints_a_unifying_status() {
        let s = status_of(Protocol::Unifying);
        assert_eq!(
            status_lines(&s),
            [
                "firmware  1.2.3",
                "protocol  Unifying",
                "link      connected on channel 62",
                "keyboard  MX Keys, serial b8bcd5b3, WPID 7015",
                "signal    -71 dBm, last packet 12 ms ago",
                "pairing   open",
                "usb       live",
                "uptime    50d 02:25:19, reset by pin",
                "counters  packets 1000 invalid 1 dropped 2 channels 3 usb-retries 5 merges 6",
                "history   1440 minutes, 512 events held",
            ]
        );
    }

    #[test]
    fn a_unifying_status_with_no_keyboard_paired_has_no_keyboard_line() {
        let mut s = status_of(Protocol::Unifying);
        s.link = Link::Unifying(None);
        let lines = status_lines(&s);
        assert!(!lines.iter().any(|l| l.starts_with("keyboard")));
        assert_eq!(lines[2], "link      not paired on channel 62");
    }

    #[test]
    fn a_nuphy_keyboard_line_holds_what_is_known_of_the_keyboard() {
        let id = [0x70, 0x15, 0xa3, 0x29];
        for (id, name, line) in [
            (Some(id), None, Some("keyboard  unnamed, ID 7015a329")),
            (
                None,
                Some("NuPhy Air75 V2"),
                Some("keyboard  NuPhy Air75 V2"),
            ),
            (None, None, None),
        ] {
            let mut s = status_of(Protocol::Nuphy);
            nuphy(&mut s).keyboard_id = id;
            nuphy(&mut s).keyboard_name = name.map(String::from);
            let lines = status_lines(&s);
            let keyboard = lines.iter().find(|l| l.starts_with("keyboard"));
            assert_eq!(keyboard.map(String::as_str), line);
        }
    }

    #[test]
    fn a_unifying_device_line_is_labeled_with_its_type() {
        for (kind, line) in [
            (2, "mouse     MX Keys, serial b8bcd5b3, WPID 7015"),
            (4, "presenter MX Keys, serial b8bcd5b3, WPID 7015"),
            (
                0x7f,
                "device    MX Keys, serial b8bcd5b3, WPID 7015, type 0x7f",
            ),
        ] {
            let mut s = status_of(Protocol::Unifying);
            unifying(&mut s).kind = kind;
            assert_eq!(status_lines(&s)[3], line);
        }
    }

    #[test]
    fn a_keyboard_without_a_name_prints_as_unnamed() {
        let mut s = status_of(Protocol::Unifying);
        unifying(&mut s).name = None;
        assert!(
            status_lines(&s).contains(&"keyboard  unnamed, serial b8bcd5b3, WPID 7015".to_string())
        );
    }

    fn minute() -> Minute {
        Minute {
            packets: 120,
            invalid: 1,
            dropped: 0,
            channel_changes: 2,
            link_losses: Some(3),
            usb_retries: 5,
            gaps: Some(4),
            rssi: None,
            noise: None,
        }
    }

    #[test]
    fn a_history_line_lists_the_counters_the_protocol_counts() {
        assert_eq!(
            minute_line(Protocol::Nuphy, &minute()),
            "packets   120  invalid   1  dropped   0  channels   2  losses   3  gaps   4  usb-retries   5  signal -  noise -"
        );
        assert_eq!(
            minute_line(Protocol::Unifying, &minute()),
            "packets   120  invalid   1  dropped   0  channels   2  usb-retries   5  signal -  noise -"
        );
    }
}
