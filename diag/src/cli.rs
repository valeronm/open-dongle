use anyhow::Result;

use crate::device::{Device, Transport};
use crate::format::{last_packet, link_state_name, paired, reset_causes, uptime, usb_state};

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
