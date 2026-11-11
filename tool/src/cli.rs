use std::io::Write;

use std::path::Path;
use std::time::Duration;

use anyhow::{Context, Result};
use chrono::Local;

use crate::board::{self, State};
use crate::device::{self, Device, Transport};
use crate::format::{
    NOISE_QUESTION, action_question, channel_description, describe_event, flash_question,
    last_packet, link_state_name, noise_level, paired, reset_causes, uptime, usb_state, version,
    wall_clock,
};
use crate::protocol::Action;
use crate::{dfu, uf2};

pub fn status<T: Transport>(dev: &mut Device<T>) -> Result<()> {
    let s = dev.status()?;
    let id = s
        .keyboard_id
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    let c = s.counters;
    println!("firmware  {}", version(s.firmware_version));
    println!(
        "link      {} on channel {} ({}), keyboard {id}",
        link_state_name(s.link_state),
        s.channel,
        paired(s.own)
    );
    println!(
        "signal    {} dBm, {}",
        s.last_rssi,
        last_packet(s.last_rx_age_ms)
    );
    println!("usb       {}", usb_state(s.usb_live, s.usb_suspended));
    println!(
        "uptime    {}, reset by {}",
        uptime(s.uptime_ms),
        reset_causes(s.reset_cause)
    );
    println!(
        "counters  packets {} invalid {} dropped {} channels {} losses {} usb-retries {} merges {} surveys {}",
        c.packets,
        c.invalid,
        c.dropped,
        c.channel_changes,
        c.link_losses,
        c.usb_retries,
        c.merges,
        c.surveys
    );
    println!(
        "history   {} minutes, {} events held",
        s.minute_next.saturating_sub(s.minute_oldest),
        s.events_total.saturating_sub(s.events_oldest)
    );
    Ok(())
}

pub fn history<T: Transport>(dev: &mut Device<T>, minutes: u32) -> Result<()> {
    let s = dev.status()?;
    let now = Local::now();
    let from = s.minute_next.saturating_sub(minutes);
    for (index, m) in dev.minutes_between(from, s.minute_next)? {
        let at = wall_clock(now, s.uptime_ms, u64::from(index) * 60_000);
        let signal = m
            .rssi
            .map_or("-".into(), |r| format!("{}/{}/{}", r.min, r.avg, r.max));
        println!(
            "{}  packets {:5}  invalid {:3}  dropped {:3}  channels {:3}  losses {:3}  usb-retries {:3}  signal {signal}",
            at.format("%Y-%m-%d %H:%M"),
            m.packets,
            m.invalid,
            m.dropped,
            m.channel_changes,
            m.link_losses,
            m.usb_retries
        );
    }
    Ok(())
}

pub fn events<T: Transport>(dev: &mut Device<T>, count: u32) -> Result<()> {
    let s = dev.status()?;
    let now = Local::now();
    let from = s.events_total.saturating_sub(count);
    for (_, e) in dev.events_between(from, s.events_total)? {
        let at = wall_clock(now, s.uptime_ms, e.time_ms);
        println!(
            "{}  {}",
            at.format("%Y-%m-%d %H:%M:%S%.3f"),
            describe_event(&e)
        );
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

pub fn noise<T: Transport>(dev: &mut Device<T>, yes: bool) -> Result<()> {
    if !confirm(NOISE_QUESTION, yes)? {
        return Ok(());
    }
    let s = dev.status()?;
    let rows: Vec<_> = dev
        .noise()?
        .into_iter()
        .map(|c| (channel_description(c.channel, &s), c))
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
    let status = device::open(Some(&board.serial))?.status()?;
    println!(
        "flashed; OpenDongle {} running, up {}",
        version(status.firmware_version),
        uptime(status.uptime_ms)
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_bad_file_stops_before_any_board_is_looked_for() {
        let path =
            std::env::temp_dir().join(format!("open-dongle-not-uf2-{}.bin", std::process::id()));
        std::fs::write(&path, [0u8; 100]).unwrap();
        let err = flash(&path, Some("no-such-board"), true).unwrap_err();
        std::fs::remove_file(&path).unwrap();
        assert!(format!("{err:#}").contains("UF2"), "{err:#}");
    }
}
