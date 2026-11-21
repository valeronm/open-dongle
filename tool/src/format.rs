use anyhow::Result;
use chrono::{DateTime, Duration, Local};

use crate::board::State;
use crate::protocol::ProtocolInfo as _;
use crate::protocol::{
    Action, AwaitedPasskey, BUSY_DBM, Battery, BluetoothLink, ChargeState, Detail, DeviceKind,
    Divert, EVENT_ADDR_LEN, Event, Forgotten, Latency, Link, Moved, Noise, NuphyState, Pairing,
    Protocol, Refused, STRONG_DBM, Slot, SoftDevice, Status, StoredDevice, UnifyingState, Up,
    VersionMismatch, Watch, Wheel, bucket_floor,
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

fn address(most_significant_first: impl Iterator<Item = u8>) -> String {
    let bytes: Vec<String> = most_significant_first.map(|b| format!("{b:02X}")).collect();
    bytes.join(":")
}

fn bluetooth_address(d: &StoredDevice) -> String {
    address(d.addr[1..].iter().rev().copied())
}

fn device_address(d: &StoredDevice, protocol: Protocol) -> String {
    match protocol {
        Protocol::Bluetooth => bluetooth_address(d),
        Protocol::Nuphy => format!("ID {}", hex(&d.addr[..4])),
        Protocol::Unifying => format!("serial {}, WPID {}", hex(&d.addr[2..6]), hex(&d.addr[..2])),
    }
}

fn device_name(d: &StoredDevice) -> &str {
    d.name.as_deref().unwrap_or("unnamed")
}

/// The device `device_lines` numbers `place`, counting from 1.
pub fn device_at(list: &[StoredDevice], place: usize) -> Option<&StoredDevice> {
    list.get(place.checked_sub(1)?)
}

#[derive(Debug)]
pub struct DeviceLine {
    columns: String,
    slot: Option<u8>,
    pairing: Option<Pairing>,
}

impl DeviceLine {
    /// The line's text in pieces, the pairing's words carrying their pairing.
    pub fn parts(&self) -> Vec<(String, Option<Pairing>)> {
        let mut parts = vec![(self.columns.clone(), None)];
        if let Some(slot) = self.slot {
            parts.push((format!("  slot {slot}"), None));
        }
        if let Some(p) = self.pairing {
            let separator = if self.slot.is_some() { ", " } else { "  " };
            parts.extend([(separator.into(), None), (pairing(p), Some(p))]);
        }
        parts
    }
}

impl std::fmt::Display for DeviceLine {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        self.parts()
            .iter()
            .try_for_each(|(text, _)| f.write_str(text))
    }
}

/// One numbered line per stored device, in the order `forget` counts them.
pub fn device_lines(list: &[StoredDevice], protocol: Protocol) -> Vec<DeviceLine> {
    if list.is_empty() {
        return vec![DeviceLine {
            columns: "no device is stored".into(),
            slot: None,
            pairing: None,
        }];
    }
    let widest =
        |column: fn(&StoredDevice) -> &str| list.iter().map(|d| column(d).len()).max().unwrap_or(0);
    let (kinds, names) = (widest(|d| d.kind.name()), widest(device_name));
    list.iter()
        .enumerate()
        .map(|(i, d)| DeviceLine {
            columns: format!(
                "{}  {:kinds$}  {:names$}  {}",
                i + 1,
                d.kind.name(),
                device_name(d),
                device_address(d, protocol),
            ),
            slot: d.slot,
            pairing: d.pairing,
        })
        .collect()
}

pub fn forget_question(d: &StoredDevice, protocol: Protocol) -> String {
    format!(
        "Forget {} ({})? It has to be paired again to be used.",
        device_name(d),
        device_address(d, protocol)
    )
}

pub fn switch_question(to: Protocol) -> String {
    format!(
        "Switch to {}? The dongle reboots and history starts over.",
        to.name()
    )
}

pub fn pairing_open(protocol: Protocol, passkey_asked: bool) -> String {
    let how: String = match (protocol, passkey_asked) {
        (Protocol::Nuphy, _) => "hold Fn+4 on the keyboard for 3 s".into(),
        (Protocol::Unifying, _) => {
            "switch the keyboard off and on, after holding an Easy-Switch key until it blinks fast if it has them".into()
        }
        (Protocol::Bluetooth, true) => {
            "put the keyboard in Bluetooth pairing mode".into()
        }
        (Protocol::Bluetooth, false) => {
            "put the keyboard or mouse in Bluetooth pairing mode".into()
        }
    };
    format!("within 30 s, {how}")
}

pub fn passkey_digits(passkey: u32) -> String {
    format!("{passkey:06}")
}

/// The keyboard's advertised name, or "keyboard" where it gave none.
pub fn waiting_keyboard(a: &AwaitedPasskey) -> &str {
    a.name.as_deref().unwrap_or("keyboard")
}

/// The words asking for the passkey a keyboard waits for: those before the digits, the digits, and
/// those after them.
pub fn passkey_prompt(a: &AwaitedPasskey) -> [String; 3] {
    [
        "type".into(),
        passkey_digits(a.passkey),
        format!("on the {} and press Enter", waiting_keyboard(a)),
    ]
}

pub fn waits_for_passkey(who: &str) -> String {
    format!("{who} waits for the passkey")
}

pub const PAIRING_CLOSED_UNPAIRED: &str = "pairing closed with no keyboard paired";

pub fn moved(from: u8, to: u8) -> String {
    if from == to {
        format!("the link is already on channel {to}")
    } else {
        format!("moved from channel {from} to channel {to}; the keyboard follows on its next send")
    }
}

pub fn move_refused(s: &Status) -> String {
    let channels: Vec<String> = s.keyboard_channels().iter().map(u8::to_string).collect();
    format!(
        "the keyboard does not use that channel: give one of {}",
        channels.join(" ")
    )
}

pub fn flash_question(state: State, serial: &str, file: &str) -> String {
    let state = match state {
        State::Dongle => format!("running {}", crate::PRODUCT),
        State::Bootloader => "in its bootloader".into(),
        State::Factory => "running its factory program".into(),
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
    pub fn label(self) -> Option<&'static str> {
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
    } else if matches!(&s.link, Link::Nuphy(n) if n.own.is_some_and(|o| o.contains(&channel))) {
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

/// The status's channel leaves home during a pairing sweep.
pub fn watch_role(channel: u8, w: &Watch, s: &Status) -> ChannelRole {
    match channel_role(channel, s) {
        _ if Some(channel) == w.home => ChannelRole::Current,
        ChannelRole::Current => ChannelRole::Other,
        role => role,
    }
}

pub fn channel_description(channel: u8, s: &Status) -> String {
    described(channel, channel_role(channel, s))
}

pub fn described(channel: u8, role: ChannelRole) -> String {
    let wifi = wifi_channel(channel).map(|k| format!("wifi {k}"));
    let ble = is_ble_advertising(channel).then(|| "ble adv".to_string());
    let role = role.label().map(str::to_string);
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

const STATUS_LABEL_WIDTH: usize = 10;

pub fn status_label(label: &str) -> String {
    format!("{:<STATUS_LABEL_WIDTH$}", format!("{label} "))
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn state_name(s: &Status) -> &'static str {
    match &s.link {
        Link::Nuphy(nuphy) => match nuphy.state {
            NuphyState::Idle => "searching",
            NuphyState::Survey => "surveying",
            NuphyState::Pairing => "pairing",
            NuphyState::Confirmed => "pairing confirmed",
            NuphyState::Connected => "connected",
            NuphyState::MovingHome => "moving home",
        },
        Link::Unifying(None) => "not paired",
        Link::Unifying(Some(unifying)) => match unifying.state {
            UnifyingState::Connected => "connected",
            UnifyingState::Silent => "silent",
        },
        Link::Bluetooth(BluetoothLink::NoneReady) => "nothing connected",
        Link::Bluetooth(BluetoothLink::Ready) => "connected",
        Link::Bluetooth(BluetoothLink::StartFailed) => "Bluetooth failed to start",
    }
}

/// The slot's state; under NuPhy and Unifying, whose one slot is the link, the link's.
pub fn slot_state(s: &Status, slot: &Slot) -> &'static str {
    match bluetooth_slot_health(s, slot) {
        Some(LinkHealth::Connected) => "connected",
        Some(LinkHealth::Waiting) => "not connected",
        Some(_) => "not paired",
        None => state_name(s),
    }
}

pub fn slot_health(s: &Status, slot: &Slot) -> LinkHealth {
    bluetooth_slot_health(s, slot).unwrap_or_else(|| link_health(s))
}

/// `None` where the link's state is the slot's: under NuPhy and Unifying, and while Bluetooth
/// failed to start.
fn bluetooth_slot_health(s: &Status, slot: &Slot) -> Option<LinkHealth> {
    match s.link {
        Link::Bluetooth(BluetoothLink::StartFailed) | Link::Nuphy(_) | Link::Unifying(_) => None,
        Link::Bluetooth(_) if slot.connected => Some(LinkHealth::Connected),
        Link::Bluetooth(_) if slot.stored => Some(LinkHealth::Waiting),
        Link::Bluetooth(_) => Some(LinkHealth::NotPaired),
    }
}

#[derive(Clone, Copy)]
pub enum LinkHealth {
    Connected,
    Pairing,
    Waiting,
    NotPaired,
}

pub fn link_health(s: &Status) -> LinkHealth {
    match &s.link {
        Link::Nuphy(nuphy) => match nuphy.state {
            NuphyState::Connected => LinkHealth::Connected,
            NuphyState::Survey | NuphyState::Pairing | NuphyState::Confirmed => LinkHealth::Pairing,
            NuphyState::Idle | NuphyState::MovingHome => LinkHealth::Waiting,
        },
        Link::Unifying(None) => LinkHealth::NotPaired,
        Link::Unifying(Some(unifying)) => match unifying.state {
            UnifyingState::Connected => LinkHealth::Connected,
            UnifyingState::Silent => LinkHealth::Waiting,
        },
        Link::Bluetooth(BluetoothLink::Ready) => LinkHealth::Connected,
        Link::Bluetooth(_) => LinkHealth::NotPaired,
    }
}

pub fn unifying_kind(kind: u8) -> Option<&'static str> {
    Some(match kind {
        0x01 => "keyboard",
        0x02 => "mouse",
        0x03 => "numpad",
        0x04 => "presenter",
        0x07 => "remote",
        0x08 => "trackball",
        0x09 => "touchpad",
        0x0a => "tablet",
        0x0b => "gamepad",
        0x0c => "joystick",
        _ => return None,
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

pub fn pairing(p: Pairing) -> String {
    let method = if p.secure { "secure" } else { "legacy" };
    let passkey = if p.passkey { "passkey" } else { "no passkey" };
    format!("{method}, {passkey}")
}

pub fn battery(b: Battery) -> String {
    let name = |c| match c {
        ChargeState::Discharging => "discharging",
        ChargeState::Charging => "charging",
        ChargeState::Charged => "charged",
        ChargeState::Fault => "battery fault",
    };
    match b {
        Battery::Level(level, None | Some(ChargeState::Discharging)) => format!("{level}%"),
        Battery::Level(level, Some(c)) => format!("{level}%, {}", name(c)),
        Battery::Charge(c) => name(c).into(),
    }
}

/// In milliseconds to a tenth, rounded half up.
pub fn latency(l: Option<Latency>) -> String {
    let Some(l) = l else {
        return "no report yet".into();
    };
    let tenths = |us: u32| {
        let t = (u64::from(us) + 50) / 100;
        format!("{}.{} ms", t / 10, t % 10)
    };
    format!("{} average, {} max", tenths(l.average_us), tenths(l.max_us))
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

pub fn version<T: std::fmt::Display>([major, minor, patch]: [T; 3]) -> String {
    format!("{major}.{minor}.{patch}")
}

/// The firmware's version, then the bootloader's and the SoftDevice's, each with its name.
pub fn firmware(s: &Status) -> [String; 3] {
    let bootloader = s.bootloader_version.map_or("unknown".into(), version);
    let softdevice = match s.softdevice {
        SoftDevice::Absent => "no SoftDevice".into(),
        SoftDevice::Unnamed { fwid } => format!("SoftDevice FWID {fwid:#06x}"),
        SoftDevice::Named { id, version } => {
            let version = version.map_or(String::new(), |v| format!(" {}", self::version(v)));
            format!("SoftDevice S{id}{version}")
        }
    };
    [
        version(s.firmware_version),
        format!("bootloader {bootloader}"),
        softdevice,
    ]
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
            "flashed; {} {} running, up {}",
            crate::PRODUCT,
            version(s.firmware_version),
            uptime(s.uptime_ms)
        ),
        Err(m) => format!("flashed; {m}"),
    }
}

pub fn bucket_label(bucket: u8) -> String {
    bucket_floor(bucket).map_or(format!("<{BUSY_DBM}"), |dbm| dbm.to_string())
}

pub fn watch_summary(w: &Watch) -> String {
    let counted = match (w.home, w.threshold) {
        (None, _) => return "no keyboard link, so the dongle makes no moves".into(),
        (Some(_), None) => "no recent keyboard packet, so no noise counts".into(),
        (Some(_), Some(t)) => format!("noise at {} dBm or louder counts", bucket_label(t)),
    };
    if w.paused {
        format!("moves paused; {counted}")
    } else if w.hold_left_ms == 0 {
        counted
    } else {
        format!(
            "{counted}; next move in {} at the earliest",
            duration(w.hold_left_ms)
        )
    }
}

pub fn counted_share(hundredths: Option<u16>) -> String {
    hundredths.map_or("-".into(), share)
}

pub fn share(hundredths: u16) -> String {
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

fn since_input(time_ms: u64, seconds: u16, input: &str) -> String {
    match seconds {
        // The firmware reports u16::MAX both for no input since boot and for a last input that
        // many seconds or more ago.
        u16::MAX if time_ms < u64::from(u16::MAX) * 1000 => format!("no {input} since boot"),
        u16::MAX => format!("no {input} for over 18 h"),
        s => format!("{} after the last {input}", duration(u32::from(s) * 1000)),
    }
}

fn saving(err: i32) -> String {
    match err {
        0 => String::new(),
        err => format!(", saving failed: {err}"),
    }
}

fn event_address(addr: &[u8; EVENT_ADDR_LEN]) -> String {
    address(addr.iter().rev().copied())
}

/// A forgotten device is known only by its address.
fn event_device(addr: &[u8; EVENT_ADDR_LEN], stored: &[StoredDevice]) -> String {
    stored
        .iter()
        .find(|d| d.addr[1..] == addr[..])
        .and_then(|d| d.name.clone())
        .unwrap_or_else(|| event_address(addr))
}

fn event_kind(e: &Event, s: &Status) -> &'static str {
    e.slot.map_or("device", |slot| s.kind_name(slot))
}

/// `s` is the status the event's slot is looked up in.
pub fn describe_event(e: &Event, s: &Status, stored: &[StoredDevice]) -> String {
    let protocol = s.protocol();
    let kind = event_kind(e, s);
    let device = |addr: &[u8; EVENT_ADDR_LEN]| event_device(addr, stored);
    match e.detail {
        Ok(Detail::Boot { reset_cause }) => {
            format!("booted, reset by {}", reset_causes(reset_cause))
        }
        Ok(Detail::Paired {
            own: [a, b, c],
            store_err,
        }) => format!("paired on channels {a} {b} {c}{}", saving(store_err)),
        Ok(Detail::LinkLost {
            rssi,
            since_input_s,
            gap_ms,
        }) => format!(
            "{} after {}, last signal {rssi} dBm, {}, {}",
            match protocol {
                Protocol::Nuphy => "link lost",
                Protocol::Unifying => "keyboard silent",
                Protocol::Bluetooth => "keyboard disconnected",
            },
            duration(gap_ms),
            since_input(e.time_ms, since_input_s, "key"),
            noise(e.noise)
        ),
        Ok(Detail::Survey { picked: [a, b, c] }) => format!("pairing survey picked {a} {b} {c}"),
        Ok(Detail::Noise) => "noise map".into(),
        Ok(Detail::UsbSuspend) => "usb suspended".into(),
        Ok(Detail::UsbResume) => "usb resumed".into(),
        Ok(Detail::Clear) => "history cleared".into(),
        Ok(Detail::LinkUp {
            channel,
            rssi,
            up,
            down_ms,
        }) => match Up::from_code(up) {
            Some(Up::Woke) => format!(
                "keyboard woke after {} on channel {channel}, signal {rssi} dBm",
                duration(down_ms)
            ),
            Some(Up::Again) => format!(
                "link restored after {} on channel {channel}, signal {rssi} dBm",
                duration(down_ms)
            ),
            _ => format!(
                "link up {} after boot on channel {channel}, signal {rssi} dBm",
                duration(down_ms)
            ),
        },
        Ok(Detail::LinkMoved {
            from,
            to,
            reason,
            home_share,
            to_share,
        }) => match Moved::from_code(reason) {
            Some(Moved::ForNoise) => format!(
                "moved from channel {from} to channel {to} for less noise: {} on {from}, {} on {to}",
                share(home_share),
                share(to_share)
            ),
            Some(Moved::OnRequest) => {
                format!("moved from channel {from} to channel {to} on request")
            }
            _ => format!("moved from channel {from} to channel {to}"),
        },
        Ok(Detail::LinkGap {
            channel,
            rssi_before,
            rssi_after,
            gap_ms,
        }) => format!(
            "gap of {} on channel {channel}, signal {rssi_before} dBm before, {rssi_after} dBm after, {}",
            duration(gap_ms),
            noise(e.noise)
        ),
        Ok(Detail::UnifyingPaired {
            wpid,
            kind: device_type,
            store_err,
        }) => {
            let name = match &s.link {
                Link::Unifying(Some(u)) if u.wpid == wpid => s.device.name.as_deref(),
                _ => None,
            };
            let kind = unifying_kind(device_type).unwrap_or("device");
            let device = name.map_or(kind.to_string(), |name| format!("{kind} {name}"));
            format!("paired {device}, WPID {}{}", hex(&wpid), saving(store_err))
        }
        Ok(Detail::PairingOpened {
            cause,
            until_paired,
        }) => {
            let device = match protocol {
                Protocol::Bluetooth => "device",
                Protocol::Nuphy | Protocol::Unifying => "keyboard",
            };
            match (cause, until_paired) {
                (1, true) => format!("pairing opened at boot, until a {device} pairs"),
                (1, false) => "pairing opened at boot".into(),
                (_, true) => format!("pairing opened, until a {device} pairs"),
                _ => "pairing opened".into(),
            }
        }
        Ok(Detail::PairingClosed) => "pairing closed, nothing paired".into(),
        Ok(Detail::PairingTimeout { step }) => match protocol {
            Protocol::Nuphy => {
                "pairing attempt timed out waiting for the keyboard's confirm".into()
            }
            Protocol::Unifying => format!("pairing attempt timed out at request {}", step + 1),
            Protocol::Bluetooth => "pairing attempt timed out".into(),
        },
        Ok(Detail::Reconnect { release_all: true }) => {
            "keyboard reconnected, held keys released".into()
        }
        Ok(Detail::Reconnect { release_all: false }) => "keyboard reconnected".into(),
        Ok(Detail::Attack) => "forged keystroke, attack mode for 5 s".into(),
        Ok(Detail::KeyboardForgotten) => "keyboard forgotten".into(),
        Ok(Detail::BtConnected { addr }) => format!("{kind} {} connected", device(&addr)),
        Ok(Detail::BtDisconnected {
            reason,
            since_input_s,
        }) => format!(
            "{kind} disconnected: {}, {}",
            match reason {
                0x08 => "it went silent".into(),
                0x13 => "it closed the link".into(),
                0x16 => "the dongle closed the link".into(),
                reason => format!("reason {reason:#04x}"),
            },
            since_input(
                e.time_ms,
                since_input_s,
                if kind == "keyboard" { "key" } else { "input" }
            )
        ),
        Ok(Detail::BtPaired { addr, passkey }) => format!(
            "paired {kind} {} {} a passkey",
            device(&addr),
            if passkey { "with" } else { "without" }
        ),
        Ok(Detail::BtRefused { refused })
            if Refused::from_code(refused) == Some(Refused::NoPasskey) =>
        {
            "pairing refused: the passkey was not typed".into()
        }
        Ok(Detail::BtRefused { .. }) => format!("pairing refused by the {kind}"),
        Ok(Detail::BtPasskeyAwaited { addr }) => {
            waits_for_passkey(&format!("{kind} {}", device(&addr)))
        }
        Ok(Detail::BtBondLost { addr }) => format!(
            "{kind} {} lost its pairing; forget it and pair again",
            device(&addr)
        ),
        Ok(Detail::BtForgotten { addr, cause }) => {
            let slot_kind = e.slot.and_then(|slot| s.slot(slot)).map(|slot| slot.kind);
            let why = match (Forgotten::from_code(cause), slot_kind) {
                (Some(Forgotten::OnRequest), _) => String::new(),
                (Some(Forgotten::UnusableMap), Some(DeviceKind::Mouse)) => {
                    ": its report map has no pointer".into()
                }
                (Some(Forgotten::UnusableMap), Some(DeviceKind::Keyboard)) => {
                    ": its report map has no keys".into()
                }
                (Some(Forgotten::UnusableMap), None) => ": its report map is unusable".into(),
                (None, _) => format!(", cause {cause}"),
            };
            format!("forgot {kind} {}{why}", device(&addr))
        }
        Ok(Detail::KeysDiverted {
            outcome,
            diverted,
            refused,
            control,
        }) => match (Divert::from_code(outcome), control) {
            (Some(Divert::NoFeature), _) => "keyboard has no keys to divert over HID++".into(),
            (Some(Divert::Unanswered), 0) => {
                "keyboard did not answer the HID++ feature lookup".into()
            }
            (Some(Divert::Unanswered), cid) => {
                format!("keyboard did not answer the HID++ divert of control {cid:#06x}")
            }
            _ => format!(
                "{diverted} of {} keys diverted over HID++",
                u16::from(diverted) + u16::from(refused)
            ),
        },
        Ok(Detail::WheelSet { outcome, steps }) => match Wheel::from_code(outcome) {
            Some(Wheel::Hires) => {
                format!("mouse wheel in high resolution, {steps} steps per notch")
            }
            Some(Wheel::Notches) => "mouse wheel in notches".into(),
            Some(Wheel::NoFeature) => {
                "mouse wheel left in notches: no high-resolution feature".into()
            }
            Some(Wheel::Unanswered) => {
                "mouse wheel left in notches: HID++ request unanswered".into()
            }
            Some(Wheel::Refused) => "mouse wheel left in notches: HID++ request refused".into(),
            None => format!("mouse wheel setting ended with outcome {outcome}"),
        },
        Err(t) => format!("unknown event {t}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{
        DeviceKind, read_status,
        tests::{fixture, status_in_protocol, status_of, unifying},
    };
    use chrono::TimeZone;
    use diagwire::EventType;

    fn nuphy() -> Status {
        status_of(Protocol::Nuphy)
    }

    fn unpaired_unifying() -> Status {
        let mut s = status_of(Protocol::Unifying);
        s.link = Link::Unifying(None);
        s
    }

    #[test]
    fn the_flash_question_names_the_file_the_board_and_its_state() {
        let q = flash_question(State::Factory, "55D8CCC8B3E3E5D1", "zephyr.uf2");
        assert_eq!(
            q,
            "Flash zephyr.uf2 onto board 55D8CCC8B3E3E5D1, running its factory program?"
        );
    }

    #[test]
    fn a_battery_reads_as_its_level_and_charging() {
        let level = |level, charge| battery(Battery::Level(level, charge));
        assert_eq!(level(85, Some(ChargeState::Discharging)), "85%");
        assert_eq!(level(85, Some(ChargeState::Charging)), "85%, charging");
        assert_eq!(level(100, Some(ChargeState::Charged)), "100%, charged");
        assert_eq!(level(40, None), "40%");
        assert_eq!(battery(Battery::Charge(ChargeState::Charging)), "charging");
        assert_eq!(level(64, Some(ChargeState::Fault)), "64%, battery fault");
        assert_eq!(
            battery(Battery::Charge(ChargeState::Fault)),
            "battery fault"
        );
        assert_eq!(
            battery(Battery::Charge(ChargeState::Discharging)),
            "discharging"
        );
    }

    #[test]
    fn a_flash_ends_on_the_running_version() {
        let line = flashed(read_status(&fixture("status.hex")).unwrap());
        assert!(line.starts_with("flashed; SpareDongle 1.2.3 running, up "));
    }

    #[test]
    fn a_flash_of_firmware_in_a_newer_protocol_says_to_update_the_tool() {
        assert_eq!(
            flashed(read_status(&status_in_protocol(6)).unwrap()),
            "flashed; the dongle speaks diagnostic protocol 6, this tool 5; install the \
             spare-dongle tool from the firmware's release"
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
    fn reads_an_event_address_as_the_firmware_packs_it() {
        let f = fixture("event-address.hex");
        let (addr, packed) = f.split_at(7);
        let value = u32::from_le_bytes(packed[2..6].try_into().unwrap());
        let e = event(EventType::BtConnected, packed[0], packed[1], 0, value);
        let Ok(Detail::BtConnected { addr: got }) = e.detail else {
            panic!("{:?}", e.detail);
        };
        assert_eq!(
            event_address(&got),
            address(addr[1..].iter().rev().copied())
        );
    }

    #[test]
    fn describes_bluetooth_keyboards_by_address() {
        let bt = status_of(Protocol::Bluetooth);
        let at = |kind, c| event(kind, 0xeb, 0xbb, c, 0x49a6_9ad9);
        assert_eq!(
            describe_event(&at(EventType::BtConnected, 0), &bt, &[]),
            "keyboard EB:BB:49:A6:9A:D9 connected"
        );
        assert_eq!(
            describe_event(&at(EventType::BtPaired, 1), &bt, &[]),
            "paired keyboard EB:BB:49:A6:9A:D9 with a passkey"
        );
        assert_eq!(
            describe_event(&at(EventType::BtPaired, 0), &bt, &[]),
            "paired keyboard EB:BB:49:A6:9A:D9 without a passkey"
        );
        assert_eq!(
            describe_event(&at(EventType::BtBondLost, 0), &bt, &[]),
            "keyboard EB:BB:49:A6:9A:D9 lost its pairing; forget it and pair again"
        );
        assert_eq!(
            describe_event(&at(EventType::BtForgotten, 0), &bt, &[]),
            "forgot keyboard EB:BB:49:A6:9A:D9"
        );
        assert_eq!(
            describe_event(&at(EventType::BtForgotten, 1), &bt, &[]),
            "forgot keyboard EB:BB:49:A6:9A:D9: its report map has no keys"
        );
        assert_eq!(
            describe_event(&at(EventType::BtForgotten, 7), &bt, &[]),
            "forgot keyboard EB:BB:49:A6:9A:D9, cause 7"
        );
    }

    #[test]
    fn names_a_stored_bluetooth_keyboard() {
        let bt = status_of(Protocol::Bluetooth);
        let stored = crate::protocol::read_devices(&fixture("devices.hex")).unwrap();
        let at = |kind| event(kind, 0xeb, 0xbb, 1, 0x49a6_9ad9);
        assert_eq!(
            describe_event(&at(EventType::BtConnected), &bt, &stored),
            "keyboard MX Keys connected"
        );
        assert_eq!(
            describe_event(&at(EventType::BtPaired), &bt, &stored),
            "paired keyboard MX Keys with a passkey"
        );
        assert_eq!(
            describe_event(&at(EventType::BtBondLost), &bt, &stored),
            "keyboard MX Keys lost its pairing; forget it and pair again"
        );
    }

    #[test]
    fn the_time_since_the_last_input_reads_as_a_duration() {
        let bt = status_of(Protocol::Bluetooth);
        let gone =
            |b, c| describe_event(&event(EventType::BtDisconnected, 0x08, b, c, 0), &bt, &[]);
        assert_eq!(
            gone(42, 0),
            "keyboard disconnected: it went silent, 42 s after the last key"
        );
        assert_eq!(
            gone(192, 0),
            "keyboard disconnected: it went silent, 3 min 12 s after the last key"
        );
        assert_eq!(
            gone(0x08, 0x47),
            "keyboard disconnected: it went silent, 5 h 3 min after the last key"
        );
    }

    #[test]
    fn describes_a_bluetooth_disconnect_by_its_reason() {
        let bt = status_of(Protocol::Bluetooth);
        let gone =
            |reason| describe_event(&event(EventType::BtDisconnected, reason, 3, 0, 0), &bt, &[]);
        assert_eq!(
            gone(0x08),
            "keyboard disconnected: it went silent, 3 s after the last key"
        );
        assert_eq!(
            gone(0x13),
            "keyboard disconnected: it closed the link, 3 s after the last key"
        );
        assert_eq!(
            gone(0x16),
            "keyboard disconnected: the dongle closed the link, 3 s after the last key"
        );
        assert_eq!(
            gone(0x3d),
            "keyboard disconnected: reason 0x3d, 3 s after the last key"
        );
    }

    #[test]
    fn describes_a_refused_bluetooth_pairing_by_who_refused() {
        let bt = status_of(Protocol::Bluetooth);
        assert_eq!(
            describe_event(&event(EventType::BtRefused, 0, 0, 0, 0), &bt, &[]),
            "pairing refused by the keyboard"
        );
        assert_eq!(
            describe_event(&event(EventType::BtRefused, 1, 0, 0, 0), &bt, &[]),
            "pairing refused: the passkey was not typed"
        );
    }

    #[test]
    fn describes_a_keyboard_waiting_for_the_passkey_by_its_name_or_address() {
        let bt = status_of(Protocol::Bluetooth);
        let stored = crate::protocol::read_devices(&fixture("devices.hex")).unwrap();
        let waiting = event(EventType::BtPasskeyAwaited, 0xeb, 0xbb, 1, 0x49a6_9ad9);
        assert_eq!(
            describe_event(&waiting, &bt, &stored),
            "keyboard MX Keys waits for the passkey"
        );
        assert_eq!(
            describe_event(&waiting, &bt, &[]),
            "keyboard EB:BB:49:A6:9A:D9 waits for the passkey"
        );
    }

    #[test]
    fn describes_the_link_coming_back() {
        let back = event(EventType::LinkUp, 33, 0xc1, 1, 4200);
        assert_eq!(
            describe_event(&back, &nuphy(), &[]),
            "link restored after 4 s on channel 33, signal -63 dBm"
        );
        let first = event(EventType::LinkUp, 33, 0xc1, 0, 900);
        assert_eq!(
            describe_event(&first, &nuphy(), &[]),
            "link up 900 ms after boot on channel 33, signal -63 dBm"
        );
    }

    #[test]
    fn describes_the_move_back_to_the_own_channel() {
        let moved = event(EventType::LinkMoved, 2, 70, 0, 0);
        assert_eq!(
            describe_event(&moved, &nuphy(), &[]),
            "moved from channel 2 to channel 70"
        );
    }

    #[test]
    fn a_move_prints_why_it_moved() {
        let requested = event(EventType::LinkMoved, 41, 5, 2, 0);
        assert_eq!(
            describe_event(&requested, &unpaired_unifying(), &[]),
            "moved from channel 41 to channel 5 on request"
        );
        let moved = event(EventType::LinkMoved, 5, 41, 1, 4600 << 16 | 210);
        assert_eq!(
            describe_event(&moved, &unpaired_unifying(), &[]),
            "moved from channel 5 to channel 41 for less noise: 46.00% on 5, 2.10% on 41"
        );
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
        let gap = event(EventType::LinkGap, 38, 0xae, 0xb1, 340);
        assert_eq!(
            describe_event(&gap, &nuphy(), &[]),
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
            describe_event(&noisy, &nuphy(), &[]),
            "gap of 340 ms on channel 38, signal -82 dBm before, -79 dBm after, \
             noise -50 dBm, 25.00% at -85, 3.00% at -60"
        );
    }

    #[test]
    fn stored_devices_are_numbered_with_their_kinds_and_slots() {
        let list = crate::protocol::read_devices(&fixture("devices.hex")).unwrap();
        assert_eq!(
            texts(device_lines(&list, Protocol::Bluetooth)),
            [
                "1  keyboard  MX Keys       EB:BB:49:A6:9A:D9  slot 0, legacy, passkey",
                "2  mouse     MX Master 3S  C8:F2:E0:11:3C:5A  slot 1, secure, no passkey",
            ]
        );
        assert_eq!(
            device_lines(&list, Protocol::Bluetooth)[1].pairing,
            Some(Pairing {
                secure: true,
                passkey: false
            })
        );
        let stored = StoredDevice {
            slot: None,
            ..list[1].clone()
        };
        assert_eq!(
            texts(device_lines(&[stored], Protocol::Bluetooth)),
            ["1  mouse  MX Master 3S  C8:F2:E0:11:3C:5A  secure, no passkey"]
        );
        let unnamed = StoredDevice {
            addr: [1, 0x24, 0xa8, 0x4b, 0x3c, 0x7d, 0xd6],
            kind: DeviceKind::Keyboard,
            name: None,
            slot: None,
            pairing: Some(Pairing {
                secure: false,
                passkey: true,
            }),
        };
        assert_eq!(
            texts(device_lines(&[unnamed], Protocol::Bluetooth)),
            ["1  keyboard  unnamed  D6:7D:3C:4B:A8:24  legacy, passkey"]
        );
        assert_eq!(
            texts(device_lines(&[], Protocol::Bluetooth)),
            ["no device is stored"]
        );
    }

    fn texts(lines: Vec<DeviceLine>) -> Vec<String> {
        lines.iter().map(ToString::to_string).collect()
    }

    #[test]
    fn a_pairing_reads_as_its_method_and_whether_a_passkey_was_typed() {
        let words = |secure, passkey| pairing(Pairing { secure, passkey });
        assert_eq!(words(true, true), "secure, passkey");
        assert_eq!(words(true, false), "secure, no passkey");
        assert_eq!(words(false, true), "legacy, passkey");
        assert_eq!(words(false, false), "legacy, no passkey");
    }

    #[test]
    fn an_esb_keyboard_is_listed_by_the_ids_its_status_shows() {
        let keyboard = |addr| StoredDevice {
            addr,
            kind: DeviceKind::Keyboard,
            name: Some("MX Keys".into()),
            slot: Some(0),
            pairing: None,
        };
        assert_eq!(
            texts(device_lines(
                &[keyboard([0x70, 0x15, 0xa3, 0x29, 0, 0, 0])],
                Protocol::Nuphy
            )),
            ["1  keyboard  MX Keys  ID 7015a329  slot 0"]
        );
        assert_eq!(
            texts(device_lines(
                &[keyboard([0x70, 0x15, 0xb8, 0xbc, 0xd5, 0xb3, 0])],
                Protocol::Unifying
            )),
            ["1  keyboard  MX Keys  serial b8bcd5b3, WPID 7015  slot 0"]
        );
    }

    #[test]
    fn bluetooth_events_name_the_device_of_their_slot() {
        let s = crate::protocol::tests::mouse_status();
        let stored = crate::protocol::read_devices(&fixture("devices.hex")).unwrap();
        let mouse = |kind, a, c| Event {
            slot: Some(1),
            ..event(kind, a, 0xf2, c, 0xe011_3c5a)
        };
        let d = |e: Event| describe_event(&e, &s, &stored);
        assert_eq!(
            d(mouse(EventType::BtConnected, 0xc8, 0)),
            "mouse MX Master 3S connected"
        );
        assert_eq!(
            d(mouse(EventType::BtPaired, 0xc8, 0)),
            "paired mouse MX Master 3S without a passkey"
        );
        assert_eq!(
            d(mouse(EventType::BtBondLost, 0xc8, 0)),
            "mouse MX Master 3S lost its pairing; forget it and pair again"
        );
        assert_eq!(
            d(mouse(EventType::BtForgotten, 0xc8, 1)),
            "forgot mouse MX Master 3S: its report map has no pointer"
        );
        let gone = Event {
            slot: Some(1),
            ..event(EventType::BtDisconnected, 0x08, 3, 0, 0)
        };
        assert_eq!(
            d(gone),
            "mouse disconnected: it went silent, 3 s after the last input"
        );
        assert_eq!(
            d(event(EventType::PairingOpened, 1, 1, 0, 0)),
            "pairing opened at boot, until a device pairs"
        );
        let refused = Event {
            slot: Some(1),
            ..event(EventType::BtRefused, 0, 0, 0, 0)
        };
        assert_eq!(d(refused), "pairing refused by the mouse");
        let keyboard = event(EventType::BtConnected, 0xeb, 0xbb, 0, 0x49a6_9ad9);
        assert_eq!(d(keyboard), "keyboard MX Keys connected");
    }

    #[test]
    fn forgetting_asks_about_the_keyboard_by_name_and_address() {
        let k = StoredDevice {
            addr: [1, 0xd9, 0x9a, 0xa6, 0x49, 0xbb, 0xeb],
            kind: DeviceKind::Keyboard,
            name: Some("MX Keys".into()),
            slot: None,
            pairing: None,
        };
        assert_eq!(
            forget_question(&k, Protocol::Bluetooth),
            "Forget MX Keys (EB:BB:49:A6:9A:D9)? It has to be paired again to be used."
        );
        let k = StoredDevice {
            addr: [0x40, 0x8a, 0xb8, 0xbc, 0xd5, 0xb3, 0],
            ..k
        };
        assert_eq!(
            forget_question(&k, Protocol::Unifying),
            "Forget MX Keys (serial b8bcd5b3, WPID 408a)? It has to be paired again to be used."
        );
        let k = StoredDevice {
            addr: [0x70, 0x15, 0xa3, 0x29, 0, 0, 0],
            name: Some("NuPhy Air75 V2".into()),
            ..k
        };
        assert_eq!(
            forget_question(&k, Protocol::Nuphy),
            "Forget NuPhy Air75 V2 (ID 7015a329)? It has to be paired again to be used."
        );
    }

    #[test]
    fn a_bluetooth_pairing_opens_without_showing_a_passkey() {
        assert_eq!(
            pairing_open(Protocol::Bluetooth, true),
            "within 30 s, put the keyboard in Bluetooth pairing mode"
        );
        assert_eq!(
            pairing_open(Protocol::Bluetooth, false),
            "within 30 s, put the keyboard or mouse in Bluetooth pairing mode"
        );
        assert_eq!(
            pairing_open(Protocol::Nuphy, false),
            "within 30 s, hold Fn+4 on the keyboard for 3 s"
        );
    }

    #[test]
    fn the_passkey_prompt_keeps_the_leading_zeros() {
        let a = AwaitedPasskey {
            passkey: 1234,
            name: None,
        };
        assert_eq!(
            passkey_prompt(&a).join(" "),
            "type 001234 on the keyboard and press Enter"
        );
        assert_eq!(passkey_prompt(&a)[1], "001234");
    }

    #[test]
    fn the_passkey_prompt_names_the_keyboard_by_its_advertised_name() {
        let a = AwaitedPasskey {
            passkey: 1234,
            name: Some("MX Keys".into()),
        };
        assert_eq!(
            passkey_prompt(&a).join(" "),
            "type 001234 on the MX Keys and press Enter"
        );
    }

    #[test]
    fn describes_a_link_loss_and_a_pairing() {
        let lost = event(EventType::LinkLost, 0xb9, 3, 0, 1200);
        assert_eq!(
            describe_event(&lost, &nuphy(), &[]),
            "link lost after 1 s, last signal -71 dBm, 3 s after the last key, noise -"
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
            describe_event(&noisy, &nuphy(), &[]),
            "link lost after 1 s, last signal -71 dBm, 3 s after the last key, noise -50 dBm, 25.00% at -85, 3.00% at -60"
        );
        let paired = event(EventType::Paired, 26, 30, 34, 0);
        assert_eq!(
            describe_event(&paired, &nuphy(), &[]),
            "paired on channels 26 30 34"
        );
        let failed = event(EventType::Paired, 26, 30, 34, (-5i32) as u32);
        assert_eq!(
            describe_event(&failed, &nuphy(), &[]),
            "paired on channels 26 30 34, saving failed: -5"
        );
    }

    fn event(kind: EventType, a: u8, b: u8, c: u8, value: u32) -> Event {
        let raw = diagwire::Event {
            r#type: kind as u8,
            a,
            b,
            c,
            value,
            ..Default::default()
        };
        Event {
            time_ms: 0,
            detail: Detail::of(&raw),
            slot: Some(0),
            noise: None,
        }
    }

    #[test]
    fn a_unifying_silence_is_not_called_a_loss() {
        let silent = event(EventType::LinkLost, 0xc4, 0xff, 0xff, 2300);
        assert_eq!(
            describe_event(&silent, &unpaired_unifying(), &[]),
            "keyboard silent after 2 s, last signal -60 dBm, no key since boot, noise -"
        );
        let later = Event {
            time_ms: 20 * 3_600_000,
            ..silent
        };
        assert_eq!(
            describe_event(&later, &unpaired_unifying(), &[]),
            "keyboard silent after 2 s, last signal -60 dBm, no key for over 18 h, noise -"
        );
    }

    #[test]
    fn a_link_up_after_a_wake_says_the_keyboard_woke() {
        let woke = event(EventType::LinkUp, 2, 0xc4, 2, 1_920_000);
        assert_eq!(
            describe_event(&woke, &unpaired_unifying(), &[]),
            "keyboard woke after 32 min 0 s on channel 2, signal -60 dBm"
        );
    }

    #[test]
    fn a_pairing_event_names_the_device_when_it_is_the_one_paired_now() {
        let mx_keys = status_of(Protocol::Unifying);
        assert_eq!(
            describe_event(
                &event(EventType::UnifyingPaired, 0x70, 0x15, 1, 0),
                &mx_keys,
                &[]
            ),
            "paired keyboard MX Keys, WPID 7015"
        );
        assert_eq!(
            describe_event(
                &event(EventType::UnifyingPaired, 0x40, 0x82, 2, 0),
                &mx_keys,
                &[]
            ),
            "paired mouse, WPID 4082"
        );
    }

    #[test]
    fn describes_the_unifying_events() {
        let d = |e: Event| describe_event(&e, &unpaired_unifying(), &[]);
        assert_eq!(
            d(event(EventType::UnifyingPaired, 0x40, 0x8a, 2, 0)),
            "paired mouse, WPID 408a"
        );
        assert_eq!(
            d(event(EventType::UnifyingPaired, 0x40, 0x8a, 0, 0)),
            "paired device, WPID 408a"
        );
        assert_eq!(
            d(event(
                EventType::UnifyingPaired,
                0x40,
                0x8a,
                1,
                (-5i32) as u32
            )),
            "paired keyboard, WPID 408a, saving failed: -5"
        );
        assert_eq!(
            d(event(EventType::PairingOpened, 0, 0, 0, 0)),
            "pairing opened"
        );
        assert_eq!(
            d(event(EventType::PairingOpened, 1, 0, 0, 0)),
            "pairing opened at boot"
        );
        assert_eq!(
            d(event(EventType::PairingOpened, 1, 1, 0, 0)),
            "pairing opened at boot, until a keyboard pairs"
        );
        assert_eq!(
            d(event(EventType::PairingClosed, 0, 0, 0, 0)),
            "pairing closed, nothing paired"
        );
        assert_eq!(
            d(event(EventType::PairingTimeout, 1, 0, 0, 0)),
            "pairing attempt timed out at request 2"
        );
        assert_eq!(
            describe_event(&event(EventType::PairingTimeout, 0, 0, 0, 0), &nuphy(), &[]),
            "pairing attempt timed out waiting for the keyboard's confirm"
        );
        assert_eq!(
            d(event(EventType::Reconnect, 1, 0, 0, 0)),
            "keyboard reconnected, held keys released"
        );
        assert_eq!(
            d(event(EventType::Reconnect, 0, 0, 0, 0)),
            "keyboard reconnected"
        );
        assert_eq!(
            d(event(EventType::Attack, 0, 0, 0, 0)),
            "forged keystroke, attack mode for 5 s"
        );
        assert_eq!(
            d(event(EventType::KeyboardForgotten, 0, 0, 0, 0)),
            "keyboard forgotten"
        );
        assert_eq!(
            d(event(EventType::KeysDiverted, 0, 2, 0, 0)),
            "2 of 2 keys diverted over HID++"
        );
        assert_eq!(
            d(event(EventType::KeysDiverted, 0, 1, 1, 0)),
            "1 of 2 keys diverted over HID++"
        );
        assert_eq!(
            d(event(EventType::KeysDiverted, 1, 0, 0, 0)),
            "keyboard has no keys to divert over HID++"
        );
        assert_eq!(
            d(event(EventType::KeysDiverted, 2, 0, 0, 0)),
            "keyboard did not answer the HID++ feature lookup"
        );
        assert_eq!(
            d(event(EventType::KeysDiverted, 2, 1, 0, 0xc8)),
            "keyboard did not answer the HID++ divert of control 0x00c8"
        );
    }

    #[test]
    fn a_wheel_event_names_the_mode_or_why_it_stayed_in_notches() {
        let s = read_status(&fixture("status.hex")).unwrap().unwrap();
        let d = |e: Event| describe_event(&e, &s, &[]);
        assert_eq!(
            d(event(EventType::WheelSet, 0, 8, 0, 0)),
            "mouse wheel in high resolution, 8 steps per notch"
        );
        assert_eq!(
            d(event(EventType::WheelSet, 1, 0, 0, 0)),
            "mouse wheel in notches"
        );
        assert_eq!(
            d(event(EventType::WheelSet, 2, 0, 0, 0)),
            "mouse wheel left in notches: no high-resolution feature"
        );
        assert_eq!(
            d(event(EventType::WheelSet, 3, 0, 0, 0)),
            "mouse wheel left in notches: HID++ request unanswered"
        );
        assert_eq!(
            d(event(EventType::WheelSet, 4, 0, 0, 0)),
            "mouse wheel left in notches: HID++ request refused"
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
    fn a_paused_watch_says_so_instead_of_the_next_move() {
        let w = Watch {
            home: Some(41),
            threshold: Some(5),
            paused: true,
            hold_left_ms: 30_000,
            channels: Vec::new(),
        };
        assert_eq!(
            watch_summary(&w),
            "moves paused; noise at -45 dBm or louder counts"
        );
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
    fn the_firmware_names_the_bootloader_and_softdevice_under_it() {
        assert_eq!(
            firmware(&nuphy()),
            ["1.2.3", "bootloader 0.6.0", "SoftDevice S140 6.1.1"]
        );
    }

    #[test]
    fn a_missing_bootloader_or_softdevice_is_named_as_such() {
        let mut s = nuphy();
        s.bootloader_version = None;
        s.softdevice = SoftDevice::Absent;
        assert_eq!(
            firmware(&s),
            ["1.2.3", "bootloader unknown", "no SoftDevice"]
        );
        s.softdevice = SoftDevice::Unnamed { fwid: 0x00b6 };
        assert_eq!(firmware(&s)[2], "SoftDevice FWID 0x00b6");
        s.softdevice = SoftDevice::Named {
            id: 140,
            version: None,
        };
        assert_eq!(firmware(&s)[2], "SoftDevice S140");
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

    #[test]
    fn unifying_states_have_names() {
        let mut s = status_of(Protocol::Unifying);
        assert_eq!(state_name(&s), "connected");
        unifying(&mut s).state = UnifyingState::Silent;
        assert_eq!(state_name(&s), "silent");
        s.link = Link::Unifying(None);
        assert_eq!(state_name(&s), "not paired");
    }

    #[test]
    fn a_unifying_device_type_has_a_name_when_the_tool_knows_it() {
        assert_eq!(unifying_kind(1), Some("keyboard"));
        assert_eq!(unifying_kind(2), Some("mouse"));
        assert_eq!(unifying_kind(0x7f), None);
    }

    #[test]
    fn the_switch_question_names_the_reboot_and_the_history() {
        assert_eq!(
            switch_question(Protocol::Unifying),
            "Switch to Unifying? The dongle reboots and history starts over."
        );
    }
}
