use std::io::Write;

use std::path::Path;
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use chrono::Local;

use crate::board::{self, State};
use crate::device::{self, Device, Refused, Transport};
use crate::format::{
    NOISE_QUESTION, action_question, channel_description, describe_event, flash_question, flashed,
    hex, last_packet, latency, noise_level, paired, pairing_offer, reset_causes, state_name,
    status_label, unifying_kind, uptime, usb_state, version, wall_clock,
};
use crate::protocol::{
    Action, Counter, DeviceKind, ErrorCode, Link, Minute, NOISE_BUCKETS, NoiseChannel, Protocol,
    Slot, SlotMinute, SlotStatus, Status, StoredDevice, Watch, shown_slots,
};
use crate::{dfu, format, uf2};

/// Prints `s`'s link-wide lines and a section per shown slot, asking the dongle for the slots
/// past `s`'s own.
pub fn status<T: Transport>(dev: &mut Device<T>, s: &Status) -> Result<()> {
    for line in status_lines(s, &dev.shown(s)?) {
        println!("{line}");
    }
    Ok(())
}

fn status_lines(s: &Status, devices: &[SlotStatus]) -> Vec<String> {
    let (link, offer) = match &s.link {
        Link::Nuphy(nuphy) => (
            format!(" ({})", paired(nuphy.own)),
            format!(", {}", pairing_offer(nuphy.forced)),
        ),
        _ => (String::new(), String::new()),
    };
    let counters: Vec<String> = s
        .protocol()
        .counters()
        .iter()
        .filter_map(|&counter| Some(format!("{} {}", counter.name(), s.counters.get(counter)?)))
        .collect();
    let link = if s.protocol().has_channels() {
        format!("{} on channel {}{link}", state_name(s), s.channel)
    } else {
        state_name(s).to_string()
    };
    let rows = vec![
        ("firmware", version(s.firmware_version)),
        ("protocol", s.protocol().name().to_string()),
        ("link", link),
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
    ];
    let mut lines = labeled(rows);
    for d in devices {
        if let Some(slot) = s.slot(d.slot) {
            lines.push(String::new());
            lines.extend(labeled(slot_rows(s, slot, d)));
        }
    }
    lines
}

fn labeled(rows: Vec<(&str, String)>) -> Vec<String> {
    rows.into_iter()
        .map(|(label, value)| status_label(label) + &value)
        .collect()
}

fn slot_rows(s: &Status, slot: &Slot, d: &SlotStatus) -> Vec<(&'static str, String)> {
    let (label, details) = match &s.link {
        Link::Unifying(Some(k)) => {
            let (label, kind) = match unifying_kind(k.kind) {
                Some(kind) => (kind, String::new()),
                None => ("device", format!(", type {:#04x}", k.kind)),
            };
            let details = format!(", serial {}, WPID {}{kind}", hex(&k.serial), hex(&k.wpid));
            (label, details)
        }
        Link::Nuphy(nuphy) => (
            slot.kind.name(),
            nuphy
                .keyboard_id
                .map_or(String::new(), |id| format!(", ID {}", hex(&id))),
        ),
        _ => (slot.kind.name(), String::new()),
    };
    let device = if slot.stored || slot.connected {
        let state = if slot.connected {
            "connected"
        } else {
            "not connected"
        };
        let name = d.name.as_deref().unwrap_or("unnamed");
        format!("{name}{details}, {state}")
    } else {
        "not paired".into()
    };
    let mut rows = vec![(label, device)];
    if s.signal_shown(d) {
        let level = d.signal.map_or(String::new(), |dbm| format!("{dbm} dBm, "));
        rows.push((
            "signal",
            format!("{level}{}", last_packet(d.last_packet_ms)),
        ));
    }
    let counters: Vec<String> = s
        .protocol()
        .counters()
        .iter()
        .filter_map(|&counter| Some(format!("{} {}", counter.name(), d.get(counter)?)))
        .collect();
    rows.push(("counters", counters.join(" ")));
    if let Some(l) = s.shown_latency(d) {
        rows.push(("latency", latency(l)));
    }
    rows
}

pub fn require_nuphy(s: &Status, what: &str) -> Result<()> {
    match s.protocol() {
        Protocol::Nuphy => Ok(()),
        other => bail!(
            "only NuPhy mode has {what}; the dongle runs {} ({} protocol nuphy switches it)",
            other.name(),
            crate::TOOL
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

pub fn devices<T: Transport>(dev: &mut Device<T>, protocol: Protocol) -> Result<()> {
    for line in format::device_lines(&dev.devices()?, protocol) {
        println!("{line}");
    }
    Ok(())
}

/// Empty outside Bluetooth, the only protocol whose events name a stored device by its address.
pub fn stored_devices<T: Transport>(
    dev: &mut Device<T>,
    protocol: Protocol,
) -> Result<Vec<StoredDevice>> {
    if protocol == Protocol::Bluetooth {
        dev.devices()
    } else {
        Ok(Vec::new())
    }
}

/// `place` is the device's number in the list `devices` prints, from 1.
pub fn forget<T: Transport>(dev: &mut Device<T>, place: usize, yes: bool) -> Result<()> {
    let list = dev.devices()?;
    let Some(device) = format::device_at(&list, place) else {
        bail!(
            "there is no device {place}; `{} devices` lists the {} stored",
            crate::TOOL,
            list.len()
        );
    };
    if confirm(&format::forget_question(device), yes)? {
        dev.forget(device)?;
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
    let (mut dev, s) = loop {
        std::thread::sleep(Duration::from_millis(300));
        if let Ok(mut d) = device::open_unchecked(serial)
            && let Ok(Ok(s)) = d.read_status()
            && s.uptime_ms < before.uptime_ms
        {
            break (d, s);
        }
        if std::time::Instant::now() > deadline {
            bail!("the dongle did not come back within {RETURN_WAIT:?}; replug it");
        }
    };
    if s.protocol() != want {
        bail!("the dongle came back running {}", s.protocol().name());
    }
    status(&mut dev, &s)
}

/// `slots` holds each shown slot's part of the minute; the noise, of variable width, goes last.
fn minute_line(protocol: Protocol, m: &Minute, slots: &[(DeviceKind, SlotMinute)]) -> String {
    let columns = |get: &dyn Fn(Counter) -> Option<u16>| -> Vec<String> {
        protocol
            .counters()
            .iter()
            .filter_map(|&counter| {
                let width = if counter == Counter::Packets { 5 } else { 3 };
                get(counter).map(|n| format!("{} {n:width$}", counter.name()))
            })
            .collect()
    };
    let devices = slots.iter().map(|(kind, d)| {
        let signal = d
            .rssi
            .map_or("-".into(), |r| format!("{}/{}/{}", r.min, r.avg, r.max));
        let counters = columns(&|counter| d.get(counter));
        format!("{}: {}  signal {signal}", kind.name(), counters.join("  "))
    });
    columns(&|counter| m.get(counter))
        .into_iter()
        .chain(devices)
        .chain([format::noise(m.noise)])
        .collect::<Vec<_>>()
        .join("  ")
}

pub fn history<T: Transport>(dev: &mut Device<T>, s: &Status, minutes: u32) -> Result<()> {
    let now = Local::now();
    for (index, line) in history_rows(dev, s, minutes)? {
        let at = wall_clock(now, s.uptime_ms, u64::from(index) * 60_000);
        println!("{}  {line}", at.format("%Y-%m-%d %H:%M:%S"));
    }
    Ok(())
}

/// The last `minutes` minutes by index, with the link-wide fields from the first shown slot's.
fn history_rows<T: Transport>(
    dev: &mut Device<T>,
    s: &Status,
    minutes: u32,
) -> Result<Vec<(u32, String)>> {
    let from = s.minute_next.saturating_sub(minutes);
    let protocol = s.protocol();
    let mut slots = Vec::new();
    for slot in shown_slots(&s.slots) {
        if let Some(kind) = s.slot(slot).map(|k| k.kind) {
            slots.push((
                kind,
                dev.minutes_between(from, s.minute_next, slot, protocol)?,
            ));
        }
    }
    let Some((_, first)) = slots.first() else {
        return Ok(Vec::new());
    };
    Ok(first
        .iter()
        .map(|(index, m)| {
            let parts: Vec<(DeviceKind, SlotMinute)> = slots
                .iter()
                .filter_map(|(kind, minutes)| {
                    let (_, minute) = minutes.iter().find(|(i, _)| i == index)?;
                    Some((*kind, minute.device))
                })
                .collect();
            (*index, minute_line(protocol, m, &parts))
        })
        .collect())
}

pub fn events<T: Transport>(dev: &mut Device<T>, s: &Status, count: u32) -> Result<()> {
    let now = Local::now();
    let from = s.events_total.saturating_sub(count);
    let stored = stored_devices(dev, s.protocol())?;
    for (_, e) in dev.events_between(from, s.events_total)? {
        let at = wall_clock(now, s.uptime_ms, e.time_ms);
        println!(
            "{}  {}",
            at.format("%Y-%m-%d %H:%M:%S%.3f"),
            describe_event(&e, s, &stored)
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
    use crate::protocol::tests::{mouse_status, nuphy, status_of, unifying};
    use crate::protocol::{BluetoothLink, Command, Link, Protocol};

    fn strings(values: &[&str]) -> Vec<String> {
        values.iter().map(|v| v.to_string()).collect()
    }

    /// The lines `status` prints for a dongle whose shown slots are all `s`'s own.
    fn lines(s: &Status) -> Vec<String> {
        status_lines(s, std::slice::from_ref(&s.device))
    }

    #[test]
    fn a_bluetooth_status_shows_no_channel_or_signal() {
        let mut s = status_of(Protocol::Bluetooth);
        s.device.signal = None;
        let lines = lines(&s);
        let link = lines.iter().find(|l| l.starts_with("link")).unwrap();
        assert!(link.ends_with("nothing connected"), "{link}");
        assert!(!lines.iter().any(|l| l.starts_with("signal")));
        assert!(
            lines
                .iter()
                .any(|l| l.starts_with("protocol") && l.ends_with("Bluetooth"))
        );
    }

    #[test]
    fn a_connected_bluetooth_keyboard_shows_its_signal_and_last_packet() {
        let mut s = status_of(Protocol::Bluetooth);
        s.link = Link::Bluetooth(BluetoothLink::Connected);
        s.device.signal = Some(-58);
        s.device.last_packet_ms = Some(400);
        let lines = lines(&s);
        let signal = lines.iter().find(|l| l.starts_with("signal")).unwrap();
        assert!(
            signal.ends_with(" -58 dBm, last packet 400 ms ago"),
            "{signal}"
        );
        let link = lines.iter().find(|l| l.starts_with("link")).unwrap();
        assert!(!link.contains("channel"), "{link}");
    }

    #[test]
    fn a_nuphy_status_with_no_reading_shows_no_signal_level() {
        let mut s = status_of(Protocol::Nuphy);
        s.device.signal = None;
        s.device.last_packet_ms = None;
        let lines = lines(&s);
        let signal = lines.iter().find(|l| l.starts_with("signal")).unwrap();
        assert!(signal.ends_with(" no packet since boot"), "{signal}");
        assert!(!signal.contains("dBm"), "{signal}");
    }

    #[test]
    fn a_failed_bluetooth_start_is_reported() {
        let mut s = status_of(Protocol::Bluetooth);
        s.link = Link::Bluetooth(BluetoothLink::StartFailed);
        let lines = lines(&s);
        let link = lines.iter().find(|l| l.starts_with("link")).unwrap();
        assert!(link.ends_with("Bluetooth failed to start"), "{link}");
    }

    #[test]
    fn a_device_not_connected_shows_no_latency() {
        let s = status_of(Protocol::Bluetooth);
        assert!(s.device.latency.is_some());
        let lines = lines(&s);
        assert!(
            lines.iter().any(|l| l.contains("not connected")),
            "{lines:#?}"
        );
        assert!(
            !lines.iter().any(|l| l.starts_with("latency")),
            "{lines:#?}"
        );
    }

    #[test]
    fn a_bluetooth_status_has_a_section_for_each_shown_slot() {
        let s = mouse_status();
        let keyboard = SlotStatus {
            slot: 0,
            name: Some("MX Keys".into()),
            signal: None,
            last_packet_ms: None,
            packets: 10,
            gaps: None,
            link_losses: None,
            latency: None,
        };
        assert_eq!(
            status_lines(&s, &[keyboard, s.device.clone()]),
            [
                "firmware  1.2.3",
                "protocol  Bluetooth",
                "link      connected",
                "pairing   closed",
                "usb       live",
                "uptime    50d 02:25:19, reset by pin",
                "counters  dropped 2 usb-retries 5 merges 6",
                "history   1440 minutes, 512 events held",
                "",
                "keyboard  MX Keys, not connected",
                "counters  packets 10",
                "",
                "mouse     MX Master 3S, connected",
                "signal    -60 dBm, last packet 250 ms ago",
                "counters  packets 2000",
            ]
        );
    }

    #[test]
    fn a_slot_with_nothing_paired_says_so() {
        let mut s = status_of(Protocol::Bluetooth);
        s.slots[0].stored = false;
        s.device.name = None;
        s.device.signal = None;
        let lines = lines(&s);
        assert!(
            lines.contains(&"keyboard  not paired".to_string()),
            "{lines:#?}"
        );
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
        let list = crate::protocol::tests::fixture("devices.hex");
        let mut dev = Device::new(
            Fake::new(move |req| {
                assert_eq!(req[0], Command::Devices as u8);
                parts(req[0], req[1], &list)
            }),
            0,
        );
        for place in [0, 3] {
            let err = forget(&mut dev, place, true).unwrap_err().to_string();
            assert_eq!(
                err,
                format!("there is no device {place}; `spare-dongle devices` lists the 2 stored")
            );
        }
    }

    #[test]
    fn forgetting_sends_the_address_of_the_keyboard_at_that_place() {
        let list = crate::protocol::tests::fixture("devices.hex");
        let sent = std::rc::Rc::new(std::cell::Cell::new(None));
        let seen = sent.clone();
        let mut dev = Device::new(
            Fake::new(move |req| {
                if req[0] == Command::Devices as u8 {
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
                [1, 0x5a, 0x3c, 0x11, 0xe0, 0xf2, 0xc8]
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
            std::env::temp_dir().join(format!("spare-dongle-not-uf2-{}.bin", std::process::id()));
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
            "only NuPhy mode has forced channels; the dongle runs Unifying (spare-dongle protocol nuphy switches it)"
        );
    }

    #[test]
    fn prints_a_nuphy_status() {
        let s = status_of(Protocol::Nuphy);
        assert_eq!(
            lines(&s),
            [
                "firmware  1.2.3",
                "protocol  NuPhy",
                "link      connected on channel 26 (paired 26 30 34)",
                "pairing   open, offers channels 62 52 44, set by hand",
                "usb       live",
                "uptime    50d 02:25:19, reset by pin",
                "counters  invalid 1 dropped 2 channels 3 usb-retries 5 merges 6 surveys 7",
                "history   1440 minutes, 512 events held",
                "",
                "keyboard  MX Keys, ID 7015a329, connected",
                "signal    -71 dBm, last packet 12 ms ago",
                "counters  packets 1000 losses 4 gaps 8",
                "latency   0.9 ms average, 4.2 ms max",
            ]
        );
    }

    #[test]
    fn prints_closed_pairing_in_a_nuphy_status() {
        let mut s = status_of(Protocol::Nuphy);
        s.pairing_open = false;
        assert!(
            lines(&s)
                .contains(&"pairing   closed, offers channels 62 52 44, set by hand".to_string())
        );
    }

    #[test]
    fn prints_a_unifying_status() {
        let s = status_of(Protocol::Unifying);
        assert_eq!(
            lines(&s),
            [
                "firmware  1.2.3",
                "protocol  Unifying",
                "link      connected on channel 62",
                "pairing   open",
                "usb       live",
                "uptime    50d 02:25:19, reset by pin",
                "counters  invalid 1 dropped 2 channels 3 usb-retries 5 merges 6",
                "history   1440 minutes, 512 events held",
                "",
                "keyboard  MX Keys, serial b8bcd5b3, WPID 7015, connected",
                "signal    -71 dBm, last packet 12 ms ago",
                "counters  packets 1000",
                "latency   0.9 ms average, 4.2 ms max",
            ]
        );
    }

    #[test]
    fn a_unifying_status_with_no_keyboard_paired_says_its_slot_is_not_paired() {
        let mut s = status_of(Protocol::Unifying);
        s.link = Link::Unifying(None);
        s.slots[0].stored = false;
        s.slots[0].connected = false;
        let lines = lines(&s);
        assert!(
            lines.contains(&"keyboard  not paired".to_string()),
            "{lines:#?}"
        );
        assert_eq!(lines[2], "link      not paired on channel 62");
    }

    #[test]
    fn a_nuphy_keyboard_line_holds_what_is_known_of_the_keyboard() {
        let id = [0x70, 0x15, 0xa3, 0x29];
        for (id, name, line) in [
            (Some(id), None, "keyboard  unnamed, ID 7015a329, connected"),
            (
                None,
                Some("NuPhy Air75 V2"),
                "keyboard  NuPhy Air75 V2, connected",
            ),
            (None, None, "keyboard  unnamed, connected"),
        ] {
            let mut s = status_of(Protocol::Nuphy);
            nuphy(&mut s).keyboard_id = id;
            s.device.name = name.map(String::from);
            let lines = lines(&s);
            assert!(lines.contains(&line.to_string()), "{lines:#?}");
        }
    }

    #[test]
    fn a_unifying_device_line_is_labeled_with_its_type() {
        for (kind, line) in [
            (
                2,
                "mouse     MX Keys, serial b8bcd5b3, WPID 7015, connected",
            ),
            (
                4,
                "presenter MX Keys, serial b8bcd5b3, WPID 7015, connected",
            ),
            (
                0x7f,
                "device    MX Keys, serial b8bcd5b3, WPID 7015, type 0x7f, connected",
            ),
        ] {
            let mut s = status_of(Protocol::Unifying);
            unifying(&mut s).kind = kind;
            let lines = lines(&s);
            assert!(lines.contains(&line.to_string()), "{lines:#?}");
        }
    }

    #[test]
    fn a_keyboard_without_a_name_prints_as_unnamed() {
        let mut s = status_of(Protocol::Unifying);
        s.device.name = None;
        assert!(
            lines(&s)
                .contains(&"keyboard  unnamed, serial b8bcd5b3, WPID 7015, connected".to_string())
        );
    }

    fn minute() -> Minute {
        Minute {
            invalid: 1,
            dropped: 0,
            channel_changes: 2,
            usb_retries: 5,
            noise: None,
            device: SlotMinute {
                packets: 120,
                gaps: Some(4),
                link_losses: Some(3),
                rssi: None,
            },
        }
    }

    #[test]
    fn a_history_line_lists_the_link_wide_counters_then_each_slots() {
        let keyboard = (DeviceKind::Keyboard, minute().device);
        assert_eq!(
            minute_line(Protocol::Nuphy, &minute(), &[keyboard]),
            "invalid   1  dropped   0  channels   2  usb-retries   5  keyboard: packets   120  losses   3  gaps   4  signal -  noise -"
        );
        assert_eq!(
            minute_line(Protocol::Unifying, &minute(), &[keyboard]),
            "invalid   1  dropped   0  channels   2  usb-retries   5  keyboard: packets   120  signal -  noise -"
        );
        let mouse = SlotMinute {
            packets: 30,
            gaps: None,
            link_losses: None,
            rssi: Some(crate::protocol::Rssi {
                min: -60,
                avg: -55,
                max: -50,
            }),
        };
        assert_eq!(
            minute_line(
                Protocol::Bluetooth,
                &minute(),
                &[keyboard, (DeviceKind::Mouse, mouse)]
            ),
            "dropped   0  usb-retries   5  keyboard: packets   120  signal -  mouse: packets    30  signal -60/-55/-50  noise -"
        );
    }

    #[test]
    fn history_asks_for_each_shown_slots_minutes() {
        let mut s = mouse_status();
        s.device.slot = 0;
        s.minute_next = 1442;
        s.minute_oldest = 0;
        let mut dev = Device::new(
            Fake::new(|req| {
                assert_eq!(req[0], Command::History as u8);
                let from = u32::from_le_bytes(req[2..6].try_into().unwrap());
                let packets = if req[6] == 1 { 9 } else { 7 };
                let mut p = from.to_le_bytes().to_vec();
                let n = 1442 - from;
                p.push(n as u8);
                for _ in 0..n {
                    p.extend([0, 0, 0, 0, 0, 0, 0, 0, 0, packets, 0, 0, 0, 0, 0, 0]);
                }
                parts(req[0], req[1], &p)
            }),
            0,
        );
        let row = "dropped   0  usb-retries   0  keyboard: packets     7  signal -  mouse: packets     9  signal -  noise -";
        assert_eq!(
            history_rows(&mut dev, &s, 2).unwrap(),
            [(1440, row.to_string()), (1441, row.to_string())]
        );
    }
}
