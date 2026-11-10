use chrono::{DateTime, Duration, Local};

use crate::protocol::{Event, EventKind};

pub fn usb_state(live: bool, suspended: bool) -> &'static str {
    match (live, suspended) {
        (_, true) => "suspended",
        (true, false) => "live",
        (false, false) => "not configured",
    }
}

pub fn paired(own: Option<[u8; 3]>) -> String {
    own.map_or("never paired".into(), |o| {
        format!("paired {} {} {}", o[0], o[1], o[2])
    })
}

pub fn last_packet(age_ms: Option<u32>) -> String {
    age_ms.map_or("no packet since boot".into(), |ms| {
        format!("last packet {ms} ms ago")
    })
}

pub fn link_state_name(state: u8) -> &'static str {
    match state {
        0 => "searching",
        1 => "surveying",
        2 => "pairing",
        3 => "pairing confirmed",
        4 => "connected",
        _ => "unknown",
    }
}

const RESET_CAUSES: [&str; 17] = [
    "pin",
    "software",
    "brownout",
    "power-on",
    "watchdog",
    "debug",
    "security",
    "low-power wake",
    "cpu lockup",
    "parity",
    "pll",
    "clock",
    "hardware",
    "user",
    "temperature",
    "bootloader",
    "flash",
];

pub fn reset_causes(bits: u32) -> String {
    let names: Vec<_> = RESET_CAUSES
        .iter()
        .enumerate()
        .filter(|(i, _)| bits & (1 << i) != 0)
        .map(|(_, name)| *name)
        .collect();
    if names.is_empty() {
        "none".into()
    } else {
        names.join(", ")
    }
}

pub fn uptime(ms: u64) -> String {
    let s = ms / 1000;
    let (days, h, m, s) = (s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
    if days > 0 {
        format!("{days}d {h:02}:{m:02}:{s:02}")
    } else {
        format!("{h:02}:{m:02}:{s:02}")
    }
}

/// Converts a dongle uptime into the local clock time it happened at.
pub fn wall_clock(now: DateTime<Local>, uptime_now_ms: u64, at_ms: u64) -> DateTime<Local> {
    now - Duration::milliseconds(uptime_now_ms.saturating_sub(at_ms) as i64)
}

pub fn describe_event(e: &Event) -> String {
    match e.kind {
        EventKind::Boot => format!("booted, reset by {}", reset_causes(e.value)),
        EventKind::Paired if e.value == 0 => format!("paired on channels {} {} {}", e.a, e.b, e.c),
        EventKind::Paired => format!(
            "paired on channels {} {} {}, saving failed: {}",
            e.a, e.b, e.c, e.value as i32
        ),
        EventKind::LinkLost => {
            format!(
                "link lost after {} ms, last signal {} dBm",
                e.value, e.a as i8
            )
        }
        EventKind::Survey => format!("pairing survey picked {} {} {}", e.a, e.b, e.c),
        EventKind::Noise => "noise map".into(),
        EventKind::UsbSuspend => "usb suspended".into(),
        EventKind::UsbResume => "usb resumed".into(),
        EventKind::Forget => "pairing forgotten".into(),
        EventKind::Clear => "history cleared".into(),
        EventKind::Unknown(t) => format!("unknown event {t}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::TimeZone;

    #[test]
    fn describes_a_link_loss_and_a_pairing() {
        use crate::protocol::{Event, EventKind};
        let lost = Event {
            time_ms: 0,
            kind: EventKind::LinkLost,
            a: 0xb9,
            b: 0,
            c: 0,
            value: 1200,
        };
        assert_eq!(
            describe_event(&lost),
            "link lost after 1200 ms, last signal -71 dBm"
        );
        let paired = Event {
            time_ms: 0,
            kind: EventKind::Paired,
            a: 26,
            b: 30,
            c: 34,
            value: 0,
        };
        assert_eq!(describe_event(&paired), "paired on channels 26 30 34");
        let failed = Event {
            value: (-5i32) as u32,
            ..paired
        };
        assert_eq!(
            describe_event(&failed),
            "paired on channels 26 30 34, saving failed: -5"
        );
    }

    #[test]
    fn names_each_reset_cause_bit() {
        assert_eq!(reset_causes(0x11), "pin, watchdog");
        assert_eq!(reset_causes(0), "none");
    }

    #[test]
    fn uptime_shows_days_only_when_there_are_some() {
        assert_eq!(uptime(3_723_000), "01:02:03");
        assert_eq!(uptime(90_061_000), "1d 01:01:01");
    }

    #[test]
    fn wall_clock_counts_back_from_now() {
        let now = Local.with_ymd_and_hms(2026, 11, 10, 14, 0, 0).unwrap();
        assert_eq!(wall_clock(now, 70_000, 10_000), now - Duration::seconds(60));
    }
}
