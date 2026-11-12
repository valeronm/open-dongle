use anyhow::Result;
use chrono::{DateTime, Duration, Local};

use crate::board::State;
use crate::protocol::{
    Action, BUSY_DBM, Event, EventKind, Noise, STRONG_DBM, Status, VersionMismatch,
};

pub const NOISE_QUESTION: &str =
    "The keyboard stops for about 1.5 s while the noise is measured. Measure?";

pub fn action_question(action: Action) -> &'static str {
    match action {
        Action::Reboot => "Reboot the dongle?",
        Action::Bootloader => "Reboot the dongle into its bootloader?",
        Action::Clear => "Clear the counters, history and events?",
    }
}

pub fn flash_question(state: State, serial: &str, file: &str) -> String {
    let state = match state {
        State::Dongle => "running OpenDongle",
        State::Bootloader => "in its bootloader",
        State::Factory => "running its factory program",
    };
    format!("Flash {file} onto board {serial}, {state}?")
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelRole {
    Current,
    Paired,
    Other,
}

impl ChannelRole {
    fn label(self) -> Option<&'static str> {
        match self {
            ChannelRole::Current => Some("current"),
            ChannelRole::Paired => Some("paired"),
            ChannelRole::Other => None,
        }
    }
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

fn wifi_channel(channel: u8) -> Option<u8> {
    WIFI_SPANS
        .iter()
        .find(|(_, from, to)| (*from..=*to).contains(&channel))
        .map(|(number, _, _)| *number)
}

fn is_ble_advertising(channel: u8) -> bool {
    BLE_ADVERTISING.contains(&channel)
}

pub fn channel_description(channel: u8, s: &Status) -> String {
    let wifi = wifi_channel(channel).map(|k| format!("wifi {k}"));
    let ble = is_ble_advertising(channel).then(|| "ble adv".to_string());
    let role = channel_role(channel, s).label().map(str::to_string);
    wifi.into_iter()
        .chain(ble)
        .chain(role)
        .collect::<Vec<_>>()
        .join(", ")
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

/// dBm above the -100 dBm floor, clamped at 0.
pub fn noise_level(loudest: i8) -> u8 {
    u8::try_from(i16::from(loudest) + 100).unwrap_or(0)
}

pub fn usb_state(live: bool, suspended: bool) -> &'static str {
    match (live, suspended) {
        (_, true) => "suspended",
        (true, false) => "live",
        (false, false) => "not configured",
    }
}

pub fn pairing_offer(forced: Option<[u8; 3]>) -> String {
    forced.map_or("picks channels by survey".into(), |f| {
        format!("offers channels {} {} {}, set by hand", f[0], f[1], f[2])
    })
}

pub fn paired(own: Option<[u8; 3]>) -> String {
    own.map_or("never paired".into(), |o| {
        format!("paired {} {} {}", o[0], o[1], o[2])
    })
}

pub fn duration(ms: u32) -> String {
    let s = ms / 1000;
    match ms {
        0..1_000 => format!("{ms} ms"),
        1_000..60_000 => format!("{s} s"),
        60_000..3_600_000 => format!("{} min {} s", s / 60, s % 60),
        _ => format!("{} h {} min", s / 3600, s / 60 % 60),
    }
}

pub fn last_packet(age_ms: Option<u32>) -> String {
    age_ms.map_or("no packet since boot".into(), |ms| {
        format!("last packet {} ago", duration(ms))
    })
}

pub fn link_state_name(state: u8) -> &'static str {
    match state {
        0 => "searching",
        1 => "surveying",
        2 => "pairing",
        3 => "pairing confirmed",
        4 => "connected",
        5 => "moving home",
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

pub fn version(v: [u8; 3]) -> String {
    format!("{}.{}.{}", v[0], v[1], v[2])
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

/// Firmware in a protocol this tool cannot read still counts as flashed.
pub fn flashed(status: Result<Status, VersionMismatch>) -> String {
    match status {
        Ok(s) => format!(
            "flashed; OpenDongle {} running, up {}",
            version(s.firmware_version),
            uptime(s.uptime_ms)
        ),
        Err(m) => format!("flashed; {m}"),
    }
}

fn share(hundredths: u16) -> String {
    format!("{}.{:02}%", hundredths / 100, hundredths % 100)
}

pub fn noise(n: Option<Noise>) -> String {
    n.map_or("noise -".into(), |n| {
        format!(
            "noise {} dBm, {} at {BUSY_DBM}, {} at {STRONG_DBM}",
            n.peak,
            share(n.busy),
            share(n.strong)
        )
    })
}

pub fn describe_event(e: &Event) -> String {
    match e.kind {
        EventKind::Boot => format!("booted, reset by {}", reset_causes(e.value)),
        EventKind::Paired if e.value == 0 => format!("paired on channels {} {} {}", e.a, e.b, e.c),
        EventKind::Paired => format!(
            "paired on channels {} {} {}, saving failed: {}",
            e.a, e.b, e.c, e.value as i32
        ),
        EventKind::LinkLost => format!(
            "link lost after {}, last signal {} dBm, {}",
            duration(e.value),
            e.a as i8,
            noise(e.noise)
        ),
        EventKind::Survey => format!("pairing survey picked {} {} {}", e.a, e.b, e.c),
        EventKind::Noise => "noise map".into(),
        EventKind::UsbSuspend => "usb suspended".into(),
        EventKind::UsbResume => "usb resumed".into(),
        EventKind::Clear => "history cleared".into(),
        EventKind::LinkUp if e.c == 1 => format!(
            "link restored after {} on channel {}, signal {} dBm",
            duration(e.value),
            e.a,
            e.b as i8
        ),
        EventKind::LinkUp => format!(
            "link up {} after boot on channel {}, signal {} dBm",
            duration(e.value),
            e.a,
            e.b as i8
        ),
        EventKind::LinkMoved => format!("moved from channel {} to channel {}", e.a, e.b),
        EventKind::LinkGap => format!(
            "gap of {} on channel {}, signal {} dBm before, {} dBm after, {}",
            duration(e.value),
            e.a,
            e.b as i8,
            e.c as i8,
            noise(e.noise)
        ),
        EventKind::Unknown(t) => format!("unknown event {t}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{
        read_status,
        tests::{fixture, status_in_protocol},
    };
    use chrono::TimeZone;

    #[test]
    fn the_flash_question_names_the_file_the_board_and_its_state() {
        let q = flash_question(State::Factory, "55D8CCC8B3E3E5D1", "zephyr.uf2");
        assert_eq!(
            q,
            "Flash zephyr.uf2 onto board 55D8CCC8B3E3E5D1, running its factory program?"
        );
    }

    #[test]
    fn a_flash_ends_on_the_running_version() {
        let line = flashed(read_status(&fixture("status.hex")).unwrap());
        assert!(line.starts_with("flashed; OpenDongle 1.2.3 running, up "));
    }

    #[test]
    fn a_flash_of_firmware_in_a_newer_protocol_says_to_update_the_tool() {
        assert_eq!(
            flashed(read_status(&status_in_protocol(3)).unwrap()),
            "flashed; the dongle speaks diagnostic protocol 3, this tool 2; install the \
             open-dongle tool from the firmware's release"
        );
    }

    #[test]
    fn names_the_forced_channels_or_the_survey() {
        assert_eq!(
            pairing_offer(Some([62, 52, 44])),
            "offers channels 62 52 44, set by hand"
        );
        assert_eq!(pairing_offer(None), "picks channels by survey");
    }

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
            noise: None,
        };
        assert_eq!(
            describe_event(&back),
            "link restored after 4 s on channel 33, signal -63 dBm"
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
    fn describes_the_move_back_to_the_own_channel() {
        use crate::protocol::{Event, EventKind};
        let moved = Event {
            time_ms: 0,
            kind: EventKind::LinkMoved,
            a: 2,
            b: 70,
            c: 0,
            value: 0,
            noise: None,
        };
        assert_eq!(describe_event(&moved), "moved from channel 2 to channel 70");
    }

    #[test]
    fn noise_prints_its_peak_and_shares() {
        let n = Some(Noise {
            peak: -27,
            busy: 102,
            strong: 5,
        });
        assert_eq!(noise(n), "noise -27 dBm, 1.02% at -85, 0.05% at -60");
        assert_eq!(noise(None), "noise -");
    }

    #[test]
    fn describes_a_gap_with_the_signal_on_both_sides() {
        use crate::protocol::{Event, EventKind};
        let gap = Event {
            time_ms: 0,
            kind: EventKind::LinkGap,
            a: 38,
            b: 0xae,
            c: 0xb1,
            value: 340,
            noise: None,
        };
        assert_eq!(
            describe_event(&gap),
            "gap of 340 ms on channel 38, signal -82 dBm before, -79 dBm after, noise -"
        );
        let noisy = Event {
            noise: Some(Noise {
                peak: -50,
                busy: 2500,
                strong: 300,
            }),
            ..gap
        };
        assert_eq!(
            describe_event(&noisy),
            "gap of 340 ms on channel 38, signal -82 dBm before, -79 dBm after, \
             noise -50 dBm, 25.00% at -85, 3.00% at -60"
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
            noise: None,
        };
        assert_eq!(
            describe_event(&lost),
            "link lost after 1 s, last signal -71 dBm, noise -"
        );
        let noisy = Event {
            noise: Some(Noise {
                peak: -50,
                busy: 2500,
                strong: 300,
            }),
            ..lost
        };
        assert_eq!(
            describe_event(&noisy),
            "link lost after 1 s, last signal -71 dBm, noise -50 dBm, 25.00% at -85, 3.00% at -60"
        );
        let paired = Event {
            time_ms: 0,
            kind: EventKind::Paired,
            a: 26,
            b: 30,
            c: 34,
            value: 0,
            noise: None,
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
    fn channel_description_names_band_and_role() {
        let s = read_status(&fixture("status.hex")).unwrap().unwrap();
        assert_eq!(channel_description(2, &s), "wifi 1, ble adv");
        assert_eq!(channel_description(26, &s), "ble adv, current");
        assert_eq!(channel_description(30, &s), "wifi 6, paired");
        assert_eq!(channel_description(24, &s), "");
    }

    #[test]
    fn noise_level_counts_up_from_the_floor() {
        assert_eq!(noise_level(-100), 0);
        assert_eq!(noise_level(-110), 0);
        assert_eq!(noise_level(-50), 50);
    }

    #[test]
    fn durations_switch_units_past_each_threshold() {
        assert_eq!(duration(840), "840 ms");
        assert_eq!(duration(12_400), "12 s");
        assert_eq!(duration(269_817), "4 min 29 s");
        assert_eq!(duration(3_900_000), "1 h 5 min");
    }

    #[test]
    fn last_packet_reads_in_the_largest_fitting_units() {
        assert_eq!(last_packet(Some(269_817)), "last packet 4 min 29 s ago");
        assert_eq!(last_packet(None), "no packet since boot");
    }

    #[test]
    fn names_each_reset_cause_bit() {
        assert_eq!(reset_causes(0x11), "pin, watchdog");
        assert_eq!(reset_causes(0), "none");
    }

    #[test]
    fn a_version_prints_dotted() {
        assert_eq!(version([0, 12, 3]), "0.12.3");
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
