use chrono::{DateTime, Duration, Local};

use crate::protocol::{Action, Event, EventKind, Status};

pub const NOISE_QUESTION: &str =
    "The keyboard stops for about 1.5 s while the noise is measured. Measure?";

pub fn action_question(action: Action) -> &'static str {
    match action {
        Action::Reboot => "Reboot the dongle?",
        Action::Bootloader => "Reboot the dongle into its bootloader?",
        Action::Clear => "Clear the counters, history and events?",
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelRole {
    Current,
    Paired,
    Other,
}

pub fn channel_role(channel: u8, s: &Status) -> ChannelRole {
    if channel == s.channel {
        ChannelRole::Current
    } else if s.own.is_some_and(|o| o.contains(&channel)) {
        ChannelRole::Paired
    } else {
        ChannelRole::Other
    }
}

// Radio channel n is 2400 + n MHz; Wi-Fi channel k spans 2407 + 5k ± 10 MHz.
const WIFI_SPANS: [(u8, u8, u8); 3] = [(1, 2, 22), (6, 27, 47), (11, 52, 72)];
// BLE advertises on 2402, 2426 and 2480 MHz.
const BLE_ADVERTISING: [u8; 3] = [2, 26, 80];

pub fn wifi_channel(channel: u8) -> Option<u8> {
    WIFI_SPANS
        .iter()
        .find(|(_, from, to)| (*from..=*to).contains(&channel))
        .map(|(number, _, _)| *number)
}

pub fn is_ble_advertising(channel: u8) -> bool {
    BLE_ADVERTISING.contains(&channel)
}

pub fn band_label(channel: u8) -> String {
    let wifi = wifi_channel(channel).map(|k| format!("wifi {k}"));
    let ble = is_ble_advertising(channel).then(|| "ble adv".to_string());
    wifi.into_iter().chain(ble).collect::<Vec<_>>().join(", ")
}

pub fn noise_ruler(first: u8, count: u8) -> String {
    let mut cells = vec![' '; usize::from(count)];
    let mut put = |channel: u8, glyph: char| {
        if let Some(cell) = channel
            .checked_sub(first)
            .and_then(|i| cells.get_mut(usize::from(i)))
        {
            *cell = glyph;
        }
    };
    for (number, from, to) in WIFI_SPANS {
        for channel in from..=to {
            put(channel, '─');
        }
        put(from, '├');
        put(to, '┤');
        let label = number.to_string();
        let start = from + (to - from + 1 - label.len() as u8) / 2;
        for (i, digit) in label.chars().enumerate() {
            put(start + i as u8, digit);
        }
    }
    for channel in BLE_ADVERTISING {
        put(channel, '▲');
    }
    cells.into_iter().collect()
}

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
        EventKind::Clear => "history cleared".into(),
        EventKind::LinkUp if e.c == 1 => format!(
            "link restored after {} ms on channel {}, signal {} dBm",
            e.value, e.a, e.b as i8
        ),
        EventKind::LinkUp => format!(
            "link up {} ms after boot on channel {}, signal {} dBm",
            e.value, e.a, e.b as i8
        ),
        EventKind::Unknown(t) => format!("unknown event {t}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::TimeZone;

    #[test]
    fn describes_the_link_coming_back() {
        use crate::protocol::{Event, EventKind};
        let back = Event {
            time_ms: 0,
            kind: EventKind::LinkUp,
            a: 33,
            b: 0xc1,
            c: 1,
            value: 4200,
        };
        assert_eq!(
            describe_event(&back),
            "link restored after 4200 ms on channel 33, signal -63 dBm"
        );
        let first = Event {
            c: 0,
            value: 900,
            ..back
        };
        assert_eq!(
            describe_event(&first),
            "link up 900 ms after boot on channel 33, signal -63 dBm"
        );
    }

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
    fn band_label_names_wifi_and_ble_channels() {
        assert_eq!(band_label(2), "wifi 1, ble adv");
        assert_eq!(band_label(26), "ble adv");
        assert_eq!(band_label(37), "wifi 6");
        assert_eq!(band_label(24), "");
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

    #[test]
    fn wifi_channels_span_twenty_megahertz_around_their_centers() {
        let got: Vec<_> = [2, 22, 23, 27, 47, 52, 72, 73].map(wifi_channel).to_vec();
        assert_eq!(
            got,
            [
                Some(1),
                Some(1),
                None,
                Some(6),
                Some(6),
                Some(11),
                Some(11),
                None
            ]
        );
    }

    #[test]
    fn ble_advertising_sits_on_three_channels() {
        assert_eq!(
            [2, 26, 80, 3].map(is_ble_advertising),
            [true, true, true, false]
        );
    }

    #[test]
    fn the_ruler_marks_wifi_spans_and_ble_channels() {
        let ruler: Vec<char> = noise_ruler(2, 79).chars().collect();
        assert_eq!(ruler.len(), 79);
        let at = |channel: usize| ruler[channel - 2];
        assert_eq!([at(2), at(26), at(80)], ['▲', '▲', '▲']);
        assert_eq!([at(22), at(47)], ['┤', '┤']);
        assert_eq!(at(27), '├');
        let span = |from: usize, to: usize| ruler[from - 2..=to - 2].iter().collect::<String>();
        assert!(span(27, 47).contains('6'));
        assert!(span(52, 72).contains("11"));
    }
}
