use std::io::Write;

use anyhow::Result;
use chrono::Local;

use crate::device::{Device, Transport};
use crate::format::{
    NOISE_QUESTION, action_question, channel_description, describe_event, last_packet,
    link_state_name, noise_level, paired, reset_causes, uptime, usb_state, wall_clock,
};
use crate::protocol::Action;

pub fn status<T: Transport>(dev: &mut Device<T>) -> Result<()> {
    let s = dev.status()?;
    let id = s
        .keyboard_id
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    let c = s.counters;
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
