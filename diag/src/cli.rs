use anyhow::Result;
use chrono::Local;

use crate::device::{Device, Transport};
use crate::format::{
    describe_event, last_packet, link_state_name, paired, reset_causes, uptime, usb_state,
    wall_clock,
};

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
