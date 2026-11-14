use std::io::Write;

use std::path::Path;
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use chrono::Local;

use crate::board::{self, State};
use crate::device::{self, Device, Refused, Transport};
use crate::format::{
    NOISE_QUESTION, action_question, channel_description, describe_event, flash_question, flashed,
    keyboard_id, last_packet, noise_level, paired, pairing_offer, reset_causes, state_name, uptime,
    usb_state, version, wall_clock, wpid,
};
use crate::protocol::{Action, Counter, ErrorCode, Minute, Protocol, Status};
use crate::{dfu, format, uf2};

pub fn status(s: &Status) -> Result<()> {
    for line in status_lines(s) {
        println!("{line}");
    }
    Ok(())
}

fn status_lines(s: &Status) -> Vec<String> {
    let c = s.counters;
    let nuphy = s.protocol == Protocol::Nuphy;
    let mut lines = vec![
        format!("firmware  {}", version(s.firmware_version)),
        format!("protocol  {}", s.protocol.name()),
    ];
    let keyboard = if nuphy {
        format!(
            " ({}), keyboard {}",
            paired(s.own),
            keyboard_id(s.keyboard_id)
        )
    } else {
        format!(", keyboard {}", wpid(s.keyboard_id))
    };
    lines.push(format!(
        "link      {} on channel {}{keyboard}",
        state_name(s),
        s.channel
    ));
    lines.push(format!(
        "signal    {} dBm, {}",
        s.last_rssi,
        last_packet(s.last_rx_age_ms)
    ));
    let offer = if nuphy {
        format!(", {}", pairing_offer(s.forced))
    } else {
        String::new()
    };
    lines.push(format!(
        "pairing   {}{offer}",
        if s.pairing_open { "open" } else { "closed" }
    ));
    lines.push(format!(
        "usb       {}",
        usb_state(s.usb_live, s.usb_suspended)
    ));
    lines.push(format!(
        "uptime    {}, reset by {}",
        uptime(s.uptime_ms),
        reset_causes(s.reset_cause)
    ));
    let counters: Vec<String> = s
        .protocol
        .counters()
        .iter()
        .map(|&counter| format!("{} {}", counter.name(), c.get(counter)))
        .collect();
    lines.push(format!("counters  {}", counters.join(" ")));
    lines.push(format!(
        "history   {} minutes, {} events held",
        s.minute_next.saturating_sub(s.minute_oldest),
        s.events_total.saturating_sub(s.events_oldest)
    ));
    lines
}

pub fn require_nuphy(s: &Status, what: &str) -> Result<()> {
    match s.protocol {
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

pub fn pair<T: Transport>(dev: &mut Device<T>, protocol: Protocol) -> Result<()> {
    dev.pair()?;
    println!("{}", format::pairing_open(protocol));
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
        println!("the dongle runs {}", before.protocol.name());
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
    if s.protocol != want {
        bail!("the dongle came back running {}", s.protocol.name());
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
    for (index, m) in dev.minutes_between(from, s.minute_next)? {
        let at = wall_clock(now, s.uptime_ms, u64::from(index) * 60_000);
        println!(
            "{}  {}",
            at.format("%Y-%m-%d %H:%M:%S"),
            minute_line(s.protocol, &m)
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
            describe_event(&e, s.protocol)
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
    let rows: Vec<_> = dev
        .noise()?
        .into_iter()
        .map(|c| (channel_description(c.channel, s), c))
        .collect();
    let width = rows
        .iter()
        .map(|(d, _)| d.chars().count())
        .max()
        .unwrap_or(0);
    for (description, c) in rows {
        let bar = "#".repeat(usize::from(noise_level(c.loudest)) / 2);
        println!(
            "{:3}  {description:<width$}  {:4} dBm  avg {:4}  {bar}",
            c.channel, c.loudest, c.average
        );
    }
    Ok(())
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
    let mut bootloader = board::open_port(&port, 115_200)?;
    bootloader
        .write_data_terminal_ready(true)
        .context("raising DTR")?;
    let mut shown = None;
    dfu::upload(
        &mut bootloader,
        &image,
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
    .context(
        "the upload broke off; the board stays in its bootloader until it is unplugged and \
         plugged back in, then run flash again",
    )?;
    println!();
    drop(bootloader);
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::device::tests::{Fake, error_part, parts};
    use crate::protocol::{Command, Protocol};

    fn strings(values: &[&str]) -> Vec<String> {
        values.iter().map(|v| v.to_string()).collect()
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

    fn status_in(protocol: Protocol) -> Status {
        let mut s = crate::protocol::read_status(&crate::protocol::tests::fixture("status.hex"))
            .unwrap()
            .unwrap();
        s.protocol = protocol;
        s.own = Some([26, 30, 34]);
        s
    }

    #[test]
    fn a_refused_channel_names_the_keyboards_channels() {
        let unifying = status_in(Protocol::Unifying);
        let err = move_link(&mut dongle_refusing_with(Command::Move, 3), &unifying, 6)
            .unwrap_err()
            .to_string();
        assert!(
            err.ends_with("give one of 5 8 17 32 35 41 44 62 65 71 74"),
            "{err}"
        );
        let nuphy = status_in(Protocol::Nuphy);
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
            &status_in(Protocol::Unifying),
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
            &status_in(Protocol::Nuphy),
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
        let mut s = crate::protocol::read_status(&crate::protocol::tests::fixture("status.hex"))
            .unwrap()
            .unwrap();
        assert!(require_nuphy(&s, "forced channels").is_ok());
        s.protocol = Protocol::Unifying;
        let err = require_nuphy(&s, "forced channels")
            .unwrap_err()
            .to_string();
        assert_eq!(
            err,
            "only NuPhy mode has forced channels; the dongle runs Unifying (open-dongle protocol nuphy switches it)"
        );
    }

    #[test]
    fn prints_a_nuphy_status() {
        let s = crate::protocol::read_status(&crate::protocol::tests::fixture("status.hex"))
            .unwrap()
            .unwrap();
        assert_eq!(
            status_lines(&s),
            [
                "firmware  1.2.3",
                "protocol  NuPhy",
                "link      connected on channel 26 (paired 26 30 34), keyboard 7015a329",
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
        let mut s = crate::protocol::read_status(&crate::protocol::tests::fixture("status.hex"))
            .unwrap()
            .unwrap();
        s.pairing_open = false;
        assert!(
            status_lines(&s)
                .contains(&"pairing   closed, offers channels 62 52 44, set by hand".to_string())
        );
    }

    #[test]
    fn prints_a_unifying_status() {
        let payload = crate::protocol::tests::status_running(Protocol::Unifying as u8);
        let mut s = crate::protocol::read_status(&payload).unwrap().unwrap();
        s.link_state = 1;
        assert_eq!(
            status_lines(&s),
            [
                "firmware  1.2.3",
                "protocol  Unifying",
                "link      connected on channel 26, keyboard 7015",
                "signal    -71 dBm, last packet 12 ms ago",
                "pairing   open",
                "usb       live",
                "uptime    50d 02:25:19, reset by pin",
                "counters  packets 1000 invalid 1 dropped 2 channels 3 usb-retries 5 merges 6",
                "history   1440 minutes, 512 events held",
            ]
        );
    }

    fn minute() -> Minute {
        Minute {
            packets: 120,
            invalid: 1,
            dropped: 0,
            channel_changes: 2,
            link_losses: 3,
            usb_retries: 5,
            gaps: 4,
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
