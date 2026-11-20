//! The diagnostic protocol between the dongle and the tool: its numbers, the records the firmware
//! fills, and the byte layout of each reply, encoded by the firmware and decoded by the tool.
//!
//! Records filled by C hold their enumerated fields as raw bytes, since C can store any value in
//! them.

#![no_std]

/// Bumped only when a layout that a release has shipped, or what a value in it means, changes.
pub const VERSION: u8 = 5;
pub const REPORT_LEN: usize = 64;
pub const HEADER_LEN: usize = 4;
pub const CHUNK: usize = REPORT_LEN - HEADER_LEN;
pub const REQUEST_LEN: usize = 6;
pub const REQUEST_DATA_LEN: usize = 7;
pub const ERROR_FLAG: u8 = 0x80;
pub const PAYLOAD_MAX: usize = 720;
pub const MINUTES_PER_PAGE: usize = 40;
pub const EVENTS_PER_PAGE: usize = 32;
pub const NAME_MAX: usize = 24;
/// The most slots any protocol has; a slot is a numbered place for one device.
pub const SLOTS: usize = 2;
pub const NO_SLOT: u8 = u8::MAX;
/// Marks a signal as absent: no receiver reports a signal this low.
pub const NO_RSSI: i8 = i8::MIN;
pub const NO_BATTERY: u8 = u8::MAX;
pub const SLOT_STORED: u8 = 0x01;
pub const SLOT_READY: u8 = 0x02;
/// The other pairing bits are unset without it.
pub const PAIRING_KNOWN: u8 = 0x01;
/// LE Secure Connections, not legacy pairing.
pub const PAIRING_SECURE: u8 = 0x02;
pub const PAIRING_PASSKEY: u8 = 0x04;
/// In a pair request's arg under Bluetooth: pair without asking the keyboard for a passkey.
pub const PAIR_NO_PASSKEY: u32 = 1;
/// The pair reply under Bluetooth: 1 if a passkey is to be typed.
pub const PAIR_REPLY_LEN: usize = 1;
pub const ADDR_LEN: usize = 7;
/// The most devices a device list carries.
pub const STORED_MAX: usize = 8;
pub const DEVICE_LEN: usize = ADDR_LEN + 3 + NAME_MAX;
pub const DEVICES_MAX: usize = 1 + STORED_MAX * DEVICE_LEN;
/// Readings below it count as quiet.
pub const BUSY_DBM: i8 = -85;
pub const STRONG_DBM: i8 = -60;
/// Bucket 0 holds readings below `BUSY_DBM`; each later bucket starts `BUCKET_DB` above the one
/// before, and the last has no upper edge.
pub const BUCKETS: usize = 6;
pub const BUCKET_DB: i8 = 10;
pub const NUPHY_FIXED_COUNT: usize = 7;
/// The keyboard firmware fixes these channels besides the three it gets at pairing.
pub const NUPHY_FIXED_CHANNELS: [u8; NUPHY_FIXED_COUNT] = [16, 22, 44, 48, 58, 72, 78];
pub const UNIFYING_CHANNEL_COUNT: usize = 11;
/// A paired MX Keys searches only these after an unacknowledged packet; they are its pairing
/// channels too.
pub const UNIFYING_CHANNELS: [u8; UNIFYING_CHANNEL_COUNT] =
    [5, 8, 17, 32, 35, 41, 44, 62, 65, 71, 74];

pub fn bucket(dbm: i8) -> u8 {
    if dbm < BUSY_DBM {
        return 0;
    }
    let above = (i16::from(dbm) - i16::from(BUSY_DBM)) / i16::from(BUCKET_DB);
    (1 + above).min(BUCKETS as i16 - 1) as u8
}

/// The quietest reading `bucket` holds, or `None` for the bucket below `BUSY_DBM`.
pub fn bucket_floor(bucket: u8) -> Option<i8> {
    let above = i8::try_from(bucket.checked_sub(1)?).ok()?;
    BUSY_DBM.checked_add(above.checked_mul(BUCKET_DB)?)
}

/// Lists an enum's variants once, for the lookup from a wire byte; the enum itself stays written
/// out, since cbindgen reads it without expanding macros.
macro_rules! codes {
    ($name:ident { $($variant:ident),+ $(,)? }) => {
        impl $name {
            pub const ALL: [Self; [$(stringify!($variant)),+].len()] = [$(Self::$variant),+];

            pub fn from_code(code: u8) -> Option<Self> {
                $(if code == Self::$variant as u8 {
                    return Some(Self::$variant);
                })+
                None
            }
        }

        impl From<$name> for u8 {
            fn from(v: $name) -> u8 {
                v as u8
            }
        }
    };
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Command {
    /// This and `History` take the slot asked about as data's first byte.
    Status = 1,
    History,
    Events,
    Noise,
    Reboot,
    Bootloader,
    Clear,
    ForceChannels,
    Pair,
    /// arg 0 reads; a protocol's value sets. The reply is the running protocol, then 1 if the
    /// dongle reboots to apply a changed protocol.
    Protocol,
    /// arg is the channel; the reply is the channel the link was on.
    Move,
    Watch,
    /// The stored devices; under Bluetooth the most recently connected first.
    Devices,
    /// data is the device's address as `Devices` gives it.
    Forget,
}

codes!(Command {
    Status,
    History,
    Events,
    Noise,
    Reboot,
    Bootloader,
    Clear,
    ForceChannels,
    Pair,
    Protocol,
    Move,
    Watch,
    Devices,
    Forget,
});

impl Command {
    /// Whether a dongle running `protocol` answers it.
    pub fn answered(self, protocol: Protocol) -> bool {
        match self {
            Self::ForceChannels => protocol == Protocol::Nuphy,
            Self::Noise | Self::Move | Self::Watch => protocol.has_esb(),
            Self::Forget => protocol == Protocol::Bluetooth,
            _ => true,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ErrorCode {
    UnknownCommand = 1,
    Radio,
    BadChannels,
    Store,
    WrongProtocol,
    NotMovable,
    KeyHeld,
    ListFull,
    NoSuchDevice,
}

codes!(ErrorCode {
    UnknownCommand,
    Radio,
    BadChannels,
    Store,
    WrongProtocol,
    NotMovable,
    KeyHeld,
    ListFull,
    NoSuchDevice,
});

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Protocol {
    Nuphy = 1,
    Unifying = 2,
    Bluetooth = 3,
}

codes!(Protocol {
    Nuphy,
    Unifying,
    Bluetooth
});

impl Protocol {
    /// Bluetooth never starts ESB, so the radio is unusable under it.
    pub fn has_esb(self) -> bool {
        self != Self::Bluetooth
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum DeviceKind {
    Keyboard,
    Mouse,
}

codes!(DeviceKind { Keyboard, Mouse });

impl DeviceKind {
    pub fn name(self) -> &'static str {
        match self {
            Self::Keyboard => "keyboard",
            Self::Mouse => "mouse",
        }
    }
}

/// A NuPhy status's link state byte.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum LinkState {
    Idle,
    Survey,
    Pairing,
    Confirmed,
    Connected,
    MovingHome,
}

codes!(LinkState {
    Idle,
    Survey,
    Pairing,
    Confirmed,
    Connected,
    MovingHome
});

/// A Unifying status's link state byte.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum UnifyingStatus {
    NotPaired,
    Connected,
    Silent,
}

codes!(UnifyingStatus {
    NotPaired,
    Connected,
    Silent
});

/// A Bluetooth status's link state byte.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum BtLink {
    NoneReady,
    /// A slot's device is connected and ready.
    Ready,
    StartFailed,
}

codes!(BtLink {
    NoneReady,
    Ready,
    StartFailed
});

/// A status's pairing window byte.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Window {
    Closed,
    Open,
    PasskeyAwaited,
}

codes!(Window {
    Closed,
    Open,
    PasskeyAwaited
});

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Charging {
    Discharging,
    Charging,
    /// On power with the battery full.
    Charged,
    BatteryFault,
    Unknown = u8::MAX,
}

codes!(Charging {
    Discharging,
    Charging,
    Charged,
    BatteryFault,
    Unknown
});

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum EventType {
    Boot = 1,
    Paired,
    LinkLost,
    Survey,
    Noise,
    UsbSuspend,
    UsbResume,
    Clear,
    LinkUp,
    LinkMoved,
    LinkGap,
    UnifyingPaired,
    PairingOpened,
    PairingClosed,
    PairingTimeout,
    Reconnect,
    Attack,
    BtConnected,
    BtDisconnected,
    BtPaired,
    BtRefused,
    BtBondLost,
    BtForgotten,
    /// Byte a is a `Divert`; b and c count the controls diverted and refused, and value holds an
    /// unanswered control, or 0 for an unanswered feature lookup.
    KeysDiverted,
    /// Byte a is a `Wheel`; b holds the steps per notch in high resolution.
    WheelSet,
    BtPasskeyAwaited,
}

codes!(EventType {
    Boot,
    Paired,
    LinkLost,
    Survey,
    Noise,
    UsbSuspend,
    UsbResume,
    Clear,
    LinkUp,
    LinkMoved,
    LinkGap,
    UnifyingPaired,
    PairingOpened,
    PairingClosed,
    PairingTimeout,
    Reconnect,
    Attack,
    BtConnected,
    BtDisconnected,
    BtPaired,
    BtRefused,
    BtBondLost,
    BtForgotten,
    KeysDiverted,
    WheelSet,
    BtPasskeyAwaited,
});

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Divert {
    Finished,
    NoFeature,
    Unanswered,
}

codes!(Divert {
    Finished,
    NoFeature,
    Unanswered
});

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Wheel {
    Hires,
    Notches,
    NoFeature,
    Unanswered,
    Refused,
}

codes!(Wheel {
    Hires,
    Notches,
    NoFeature,
    Unanswered,
    Refused
});

/// Byte a of `EventType::BtRefused`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Refused {
    ByKeyboard,
    NoPasskey,
}

codes!(Refused {
    ByKeyboard,
    NoPasskey
});

/// Byte c of `EventType::BtForgotten`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Forgotten {
    OnRequest,
    /// Its report map declares no keyboard, or no pointer for a mouse.
    UnusableMap,
}

codes!(Forgotten {
    OnRequest,
    UnusableMap
});

/// Byte c of `EventType::LinkUp`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Up {
    First,
    Again,
    Woke,
}

codes!(Up { First, Again, Woke });

/// Byte c of `EventType::LinkMoved`; a noise move's value holds the home share in its high 16
/// bits and the new channel's in its low 16, in hundredths of a percent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Moved {
    ToHome,
    ForNoise,
    OnRequest,
}

codes!(Moved {
    ToHome,
    ForNoise,
    OnRequest
});

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Request {
    pub command: u8,
    pub seq: u8,
    pub arg: u32,
    /// The bytes after arg; zeros where the report ends before them.
    pub data: [u8; REQUEST_DATA_LEN],
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Bootinfo {
    /// The RESET_* bits of Zephyr's hwinfo.
    pub reset_cause: u32,
    /// All zero when no bootloader left its version.
    pub bootloader: [u8; 3],
    /// 0 when no SoftDevice is in flash.
    pub softdevice_fwid: u16,
    /// 0 when the SoftDevice's information structure is too short to hold it.
    pub softdevice_id: u32,
    /// major * 1000000 + minor * 1000 + bugfix, or 0 like softdevice_id.
    pub softdevice_version: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Battery {
    /// Percent, or `NO_BATTERY` while unknown.
    pub level: u8,
    /// A `Charging`.
    pub charging: u8,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Slot {
    /// An enum device_kind.
    pub kind: u8,
    pub stored: bool,
    pub ready: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Status {
    pub firmware_version: [u8; 3],
    pub boot: Bootinfo,
    pub uptime_ms: u64,
    pub link_state: u8,
    pub channel: u8,
    pub own: [u8; 3],
    /// All zero while pairing surveys.
    pub forced: [u8; 3],
    pub keyboard_id: [u8; 4],
    pub usb_live: bool,
    pub usb_suspended: bool,
    pub invalid: u32,
    pub dropped: u32,
    pub usb_retries: u32,
    pub merges: u32,
    pub surveys: u32,
    pub minute_next: u32,
    pub minute_oldest: u32,
    pub events_total: u32,
    pub events_oldest: u32,
    /// A `Protocol`.
    pub protocol: u8,
    /// A `Window`.
    pub pairing_window: u8,
    /// The passkey a keyboard waits for and its name, padded with zeros; zero outside
    /// `Window::PasskeyAwaited`.
    pub passkey: u32,
    pub passkey_name: [u8; NAME_MAX],
    pub keyboard_serial: [u8; 4],
    pub keyboard_type: u8,
    pub slot_count: u8,
    pub slots: [Slot; SLOTS],
    /// The rest is the asked slot's.
    pub slot: u8,
    pub last_rssi: i8,
    pub last_rx_age_ms: u32,
    pub packets: u32,
    pub gaps: u32,
    pub link_losses: u32,
    /// `u32::MAX` before a report from the slot reached the host since the last clear; the
    /// maximum is 0 then.
    pub latency_average_us: u32,
    pub latency_max_us: u32,
    pub pairing: u8,
    /// Padded with zeros.
    pub name: [u8; NAME_MAX],
    pub battery: Battery,
}

/// `busy` and `strong` are shares of the readings in hundredths of a percent; all 0 without
/// readings.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct NoiseRecord {
    /// Meaningful only with readings.
    pub peak: i8,
    pub busy: u16,
    pub strong: u16,
}

/// All 0 for a minute without a reading from the slot.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Rssi {
    pub min: i8,
    pub avg: i8,
    pub max: i8,
}

/// Counts saturate.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Minute {
    pub invalid: u8,
    pub dropped: u8,
    pub usb_retries: u8,
    pub noise: NoiseRecord,
    pub packets: [u16; SLOTS],
    pub gaps: [u8; SLOTS],
    pub link_losses: [u8; SLOTS],
    pub rssi: [Rssi; SLOTS],
}

/// A minute as one slot's reply carries it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReplyMinute {
    pub invalid: u8,
    pub dropped: u8,
    pub usb_retries: u8,
    pub noise: NoiseRecord,
    pub packets: u16,
    pub gaps: u8,
    pub link_losses: u8,
    pub rssi: Rssi,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Event {
    pub time_ms: u64,
    pub value: u32,
    /// An `EventType`.
    pub r#type: u8,
    /// `NO_SLOT` for an event about no slot.
    pub slot: u8,
    pub a: u8,
    pub b: u8,
    pub c: u8,
    pub noise: NoiseRecord,
}

/// The address bytes of a Bluetooth device, least significant first, without its type.
pub const EVENT_ADDR_LEN: usize = ADDR_LEN - 1;

/// What an event records, laid out in an `Event`'s `a`, `b`, `c` and `value` by
/// [`Detail::event`]. Fields C fills with enumerated values hold them as raw bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C, u8)]
pub enum Detail {
    Boot {
        /// The RESET_* bits of Zephyr's hwinfo.
        reset_cause: u32,
    },
    Paired {
        own: [u8; 3],
        store_err: i32,
    },
    LinkLost {
        rssi: i8,
        /// `u16::MAX` for no input since boot or one that long ago or longer.
        since_input_s: u16,
        gap_ms: u32,
    },
    Survey {
        picked: [u8; 3],
    },
    Noise,
    UsbSuspend,
    UsbResume,
    Clear,
    LinkUp {
        channel: u8,
        rssi: i8,
        /// An `Up`.
        up: u8,
        down_ms: u32,
    },
    LinkMoved {
        from: u8,
        to: u8,
        /// A `Moved`.
        reason: u8,
        /// In hundredths of a percent, for a move for less noise.
        home_share: u16,
        to_share: u16,
    },
    LinkGap {
        channel: u8,
        rssi_before: i8,
        rssi_after: i8,
        gap_ms: u32,
    },
    UnifyingPaired {
        wpid: [u8; 2],
        kind: u8,
        store_err: i32,
    },
    PairingOpened {
        cause: u8,
        until_paired: bool,
    },
    PairingClosed,
    PairingTimeout {
        /// The Unifying pairing request that went unanswered, from 0.
        step: u8,
    },
    Reconnect {
        release_all: bool,
    },
    Attack,
    BtConnected {
        addr: [u8; EVENT_ADDR_LEN],
    },
    BtDisconnected {
        reason: u8,
        since_input_s: u16,
    },
    BtPaired {
        addr: [u8; EVENT_ADDR_LEN],
        passkey: bool,
    },
    BtRefused {
        /// A `Refused`.
        refused: u8,
    },
    BtBondLost {
        addr: [u8; EVENT_ADDR_LEN],
    },
    BtForgotten {
        addr: [u8; EVENT_ADDR_LEN],
        /// A `Forgotten`.
        cause: u8,
    },
    KeysDiverted {
        /// A `Divert`.
        outcome: u8,
        diverted: u8,
        refused: u8,
        /// An unanswered control, or 0 for an unanswered feature lookup.
        control: u16,
    },
    WheelSet {
        /// A `Wheel`.
        outcome: u8,
        /// The steps per notch in high resolution.
        steps: u8,
    },
    BtPasskeyAwaited {
        addr: [u8; EVENT_ADDR_LEN],
    },
}

/// Seconds since the last input as an event records them; a negative `elapsed_ms` means none
/// since boot.
pub fn since_input_s(elapsed_ms: i64) -> u16 {
    if elapsed_ms < 0 {
        return u16::MAX;
    }
    u16::try_from(elapsed_ms / 1000).unwrap_or(u16::MAX)
}

fn pack_addr(addr: &[u8; EVENT_ADDR_LEN]) -> (u8, u8, u32) {
    (
        addr[5],
        addr[4],
        u32::from_le_bytes([addr[0], addr[1], addr[2], addr[3]]),
    )
}

fn unpack_addr(e: &Event) -> [u8; EVENT_ADDR_LEN] {
    let [v0, v1, v2, v3] = e.value.to_le_bytes();
    [v0, v1, v2, v3, e.b, e.a]
}

impl Detail {
    pub fn event_type(&self) -> EventType {
        use Detail as D;
        use EventType as T;
        match self {
            D::Boot { .. } => T::Boot,
            D::Paired { .. } => T::Paired,
            D::LinkLost { .. } => T::LinkLost,
            D::Survey { .. } => T::Survey,
            D::Noise => T::Noise,
            D::UsbSuspend => T::UsbSuspend,
            D::UsbResume => T::UsbResume,
            D::Clear => T::Clear,
            D::LinkUp { .. } => T::LinkUp,
            D::LinkMoved { .. } => T::LinkMoved,
            D::LinkGap { .. } => T::LinkGap,
            D::UnifyingPaired { .. } => T::UnifyingPaired,
            D::PairingOpened { .. } => T::PairingOpened,
            D::PairingClosed => T::PairingClosed,
            D::PairingTimeout { .. } => T::PairingTimeout,
            D::Reconnect { .. } => T::Reconnect,
            D::Attack => T::Attack,
            D::BtConnected { .. } => T::BtConnected,
            D::BtDisconnected { .. } => T::BtDisconnected,
            D::BtPaired { .. } => T::BtPaired,
            D::BtRefused { .. } => T::BtRefused,
            D::BtBondLost { .. } => T::BtBondLost,
            D::BtForgotten { .. } => T::BtForgotten,
            D::KeysDiverted { .. } => T::KeysDiverted,
            D::WheelSet { .. } => T::WheelSet,
            D::BtPasskeyAwaited { .. } => T::BtPasskeyAwaited,
        }
    }

    /// The event about `slot`, with no time or noise.
    pub fn event(&self, slot: u8) -> Event {
        use Detail as D;
        let (a, b, c, value) = match *self {
            D::Boot { reset_cause } => (0, 0, 0, reset_cause),
            D::Paired { own, store_err } => (own[0], own[1], own[2], store_err as u32),
            D::LinkLost {
                rssi,
                since_input_s,
                gap_ms,
            } => {
                let [low, high] = since_input_s.to_le_bytes();
                (rssi as u8, low, high, gap_ms)
            }
            D::Survey { picked } => (picked[0], picked[1], picked[2], 0),
            D::Noise | D::UsbSuspend | D::UsbResume | D::Clear | D::PairingClosed | D::Attack => {
                (0, 0, 0, 0)
            }
            D::LinkUp {
                channel,
                rssi,
                up,
                down_ms,
            } => (channel, rssi as u8, up, down_ms),
            D::LinkMoved {
                from,
                to,
                reason,
                home_share,
                to_share,
            } => (
                from,
                to,
                reason,
                u32::from(home_share) << 16 | u32::from(to_share),
            ),
            D::LinkGap {
                channel,
                rssi_before,
                rssi_after,
                gap_ms,
            } => (channel, rssi_before as u8, rssi_after as u8, gap_ms),
            D::UnifyingPaired {
                wpid,
                kind,
                store_err,
            } => (wpid[0], wpid[1], kind, store_err as u32),
            D::PairingOpened {
                cause,
                until_paired,
            } => (cause, until_paired.into(), 0, 0),
            D::PairingTimeout { step } => (step, 0, 0, 0),
            D::Reconnect { release_all } => (release_all.into(), 0, 0, 0),
            D::BtConnected { addr } | D::BtBondLost { addr } | D::BtPasskeyAwaited { addr } => {
                let (a, b, value) = pack_addr(&addr);
                (a, b, 0, value)
            }
            D::BtPaired { addr, passkey } => {
                let (a, b, value) = pack_addr(&addr);
                (a, b, passkey.into(), value)
            }
            D::BtForgotten { addr, cause } => {
                let (a, b, value) = pack_addr(&addr);
                (a, b, cause, value)
            }
            D::BtDisconnected {
                reason,
                since_input_s,
            } => {
                let [low, high] = since_input_s.to_le_bytes();
                (reason, low, high, 0)
            }
            D::BtRefused { refused } => (refused, 0, 0, 0),
            D::KeysDiverted {
                outcome,
                diverted,
                refused,
                control,
            } => (outcome, diverted, refused, control.into()),
            D::WheelSet { outcome, steps } => (outcome, steps, 0, 0),
        };
        Event {
            r#type: self.event_type() as u8,
            slot,
            a,
            b,
            c,
            value,
            ..Event::default()
        }
    }

    /// `Err` with the type byte of an event this crate does not know.
    pub fn of(e: &Event) -> Result<Self, u8> {
        use Detail as D;
        use EventType as T;
        let since_input_s = u16::from_le_bytes([e.b, e.c]);
        Ok(match EventType::from_code(e.r#type).ok_or(e.r#type)? {
            T::Boot => D::Boot {
                reset_cause: e.value,
            },
            T::Paired => D::Paired {
                own: [e.a, e.b, e.c],
                store_err: e.value as i32,
            },
            T::LinkLost => D::LinkLost {
                rssi: e.a as i8,
                since_input_s,
                gap_ms: e.value,
            },
            T::Survey => D::Survey {
                picked: [e.a, e.b, e.c],
            },
            T::Noise => D::Noise,
            T::UsbSuspend => D::UsbSuspend,
            T::UsbResume => D::UsbResume,
            T::Clear => D::Clear,
            T::LinkUp => D::LinkUp {
                channel: e.a,
                rssi: e.b as i8,
                up: e.c,
                down_ms: e.value,
            },
            T::LinkMoved => D::LinkMoved {
                from: e.a,
                to: e.b,
                reason: e.c,
                home_share: (e.value >> 16) as u16,
                to_share: e.value as u16,
            },
            T::LinkGap => D::LinkGap {
                channel: e.a,
                rssi_before: e.b as i8,
                rssi_after: e.c as i8,
                gap_ms: e.value,
            },
            T::UnifyingPaired => D::UnifyingPaired {
                wpid: [e.a, e.b],
                kind: e.c,
                store_err: e.value as i32,
            },
            T::PairingOpened => D::PairingOpened {
                cause: e.a,
                until_paired: e.b != 0,
            },
            T::PairingClosed => D::PairingClosed,
            T::PairingTimeout => D::PairingTimeout { step: e.a },
            T::Reconnect => D::Reconnect {
                release_all: e.a != 0,
            },
            T::Attack => D::Attack,
            T::BtConnected => D::BtConnected {
                addr: unpack_addr(e),
            },
            T::BtDisconnected => D::BtDisconnected {
                reason: e.a,
                since_input_s,
            },
            T::BtPaired => D::BtPaired {
                addr: unpack_addr(e),
                passkey: e.c != 0,
            },
            T::BtRefused => D::BtRefused { refused: e.a },
            T::BtBondLost => D::BtBondLost {
                addr: unpack_addr(e),
            },
            T::BtForgotten => D::BtForgotten {
                addr: unpack_addr(e),
                cause: e.c,
            },
            T::KeysDiverted => D::KeysDiverted {
                outcome: e.a,
                diverted: e.b,
                refused: e.c,
                control: e.value as u16,
            },
            T::WheelSet => D::WheelSet {
                outcome: e.a,
                steps: e.b,
            },
            T::BtPasskeyAwaited => D::BtPasskeyAwaited {
                addr: unpack_addr(e),
            },
        })
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct WatchChannel {
    pub channel: u8,
    /// Whether the channel has heard enough to be moved to.
    pub enough: bool,
    /// The share of readings in the threshold bucket and louder, in hundredths of a percent.
    pub counted: u16,
    pub buckets: [u32; BUCKETS],
}

impl WatchChannel {
    pub fn readings(&self) -> u64 {
        self.buckets.iter().map(|&b| u64::from(b)).sum()
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Device {
    pub addr: [u8; ADDR_LEN],
    /// An enum device_kind.
    pub kind: u8,
    /// `PAIRING_*` bits.
    pub pairing: u8,
    /// The slot it is connected in, or `NO_SLOT`.
    pub slot: u8,
    /// Padded with zeros.
    pub name: [u8; NAME_MAX],
}

/// A reply payload ended before its layout did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Short;

impl core::fmt::Display for Short {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        f.write_str("reply payload is shorter than its layout")
    }
}

impl core::error::Error for Short {}

pub fn pairing_flags(secure: bool, passkey: bool) -> u8 {
    PAIRING_KNOWN
        | if secure { PAIRING_SECURE } else { 0 }
        | if passkey { PAIRING_PASSKEY } else { 0 }
}

/// Zero clears the forced channels.
pub fn channels_arg(channels: Option<[u8; 3]>) -> u32 {
    channels.map_or(0, |[a, b, c]| u32::from_le_bytes([a, b, c, 0]))
}

pub fn channels_from_arg(arg: u32) -> Option<[u8; 3]> {
    let [a, b, c, _] = arg.to_le_bytes();
    (arg != 0).then_some([a, b, c])
}

/// `data` follows the argument.
pub fn request(command: Command, seq: u8, arg: u32, data: &[u8]) -> [u8; REPORT_LEN] {
    let mut r = [0; REPORT_LEN];
    r[0] = command as u8;
    r[1] = seq;
    r[2..REQUEST_LEN].copy_from_slice(&arg.to_le_bytes());
    r[REQUEST_LEN..REQUEST_LEN + data.len()].copy_from_slice(data);
    r
}

/// `None` for a report shorter than `REQUEST_LEN`.
pub fn parse_request(buf: &[u8]) -> Option<Request> {
    let (head, rest) = buf.split_first_chunk::<REQUEST_LEN>()?;
    let mut data = [0; REQUEST_DATA_LEN];
    let n = rest.len().min(REQUEST_DATA_LEN);
    data[..n].copy_from_slice(&rest[..n]);
    Some(Request {
        command: head[0],
        seq: head[1],
        arg: u32::from_le_bytes([head[2], head[3], head[4], head[5]]),
        data,
    })
}

pub fn part_count(payload_len: usize) -> usize {
    payload_len.div_ceil(CHUNK).max(1)
}

pub fn part(out: &mut [u8; REPORT_LEN], req: &Request, payload: &[u8], index: usize) {
    *out = [0; REPORT_LEN];
    out[0] = req.command;
    out[1] = req.seq;
    out[2] = index as u8;
    out[3] = part_count(payload.len()) as u8;
    let chunk = payload.chunks(CHUNK).nth(index).unwrap_or(&[]);
    out[HEADER_LEN..HEADER_LEN + chunk.len()].copy_from_slice(chunk);
}

/// `detail` is the active protocol for `ErrorCode::WrongProtocol` and 0 otherwise.
pub fn error_report(out: &mut [u8; REPORT_LEN], req: &Request, code: u8, detail: u8) {
    *out = [0; REPORT_LEN];
    out[0] = req.command | ERROR_FLAG;
    out[1] = req.seq;
    out[3] = 1;
    out[HEADER_LEN] = code;
    out[HEADER_LEN + 1] = detail;
}

#[derive(Debug, PartialEq, Eq)]
pub enum Part<'a> {
    Data {
        command: u8,
        seq: u8,
        index: u8,
        count: u8,
        chunk: &'a [u8],
    },
    Error {
        command: u8,
        seq: u8,
        code: u8,
        detail: u8,
    },
}

pub fn parse_part(buf: &[u8]) -> Option<Part<'_>> {
    let buf = buf.get(..REPORT_LEN)?;
    Some(if buf[0] & ERROR_FLAG != 0 {
        Part::Error {
            command: buf[0] & !ERROR_FLAG,
            seq: buf[1],
            code: buf[HEADER_LEN],
            detail: buf[HEADER_LEN + 1],
        }
    } else {
        Part::Data {
            command: buf[0],
            seq: buf[1],
            index: buf[2],
            count: buf[3],
            chunk: &buf[HEADER_LEN..],
        }
    })
}

struct Writer<'a> {
    buf: &'a mut [u8],
    at: usize,
}

impl Writer<'_> {
    fn bytes(&mut self, b: &[u8]) {
        self.buf[self.at..self.at + b.len()].copy_from_slice(b);
        self.at += b.len();
    }

    fn u8(&mut self, v: u8) {
        self.bytes(&[v]);
    }

    fn u16(&mut self, v: u16) {
        self.bytes(&v.to_le_bytes());
    }

    fn u32(&mut self, v: u32) {
        self.bytes(&v.to_le_bytes());
    }

    fn u64(&mut self, v: u64) {
        self.bytes(&v.to_le_bytes());
    }

    fn noise(&mut self, n: &NoiseRecord) {
        self.u8(n.peak as u8);
        self.u16(n.busy);
        self.u16(n.strong);
    }

    fn rssi(&mut self, r: &Rssi) {
        self.bytes(&[r.min as u8, r.avg as u8, r.max as u8]);
    }
}

pub struct Reader<'a> {
    rest: &'a [u8],
}

impl<'a> Reader<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Self { rest: buf }
    }

    fn bytes<const N: usize>(&mut self) -> Result<[u8; N], Short> {
        let (head, rest) = self.rest.split_first_chunk::<N>().ok_or(Short)?;
        self.rest = rest;
        Ok(*head)
    }

    pub fn u8(&mut self) -> Result<u8, Short> {
        Ok(self.bytes::<1>()?[0])
    }

    fn i8(&mut self) -> Result<i8, Short> {
        Ok(self.u8()? as i8)
    }

    fn u16(&mut self) -> Result<u16, Short> {
        Ok(u16::from_le_bytes(self.bytes()?))
    }

    fn u32(&mut self) -> Result<u32, Short> {
        Ok(u32::from_le_bytes(self.bytes()?))
    }

    fn u64(&mut self) -> Result<u64, Short> {
        Ok(u64::from_le_bytes(self.bytes()?))
    }

    fn noise(&mut self) -> Result<NoiseRecord, Short> {
        Ok(NoiseRecord {
            peak: self.i8()?,
            busy: self.u16()?,
            strong: self.u16()?,
        })
    }

    fn rssi(&mut self) -> Result<Rssi, Short> {
        Ok(Rssi {
            min: self.i8()?,
            avg: self.i8()?,
            max: self.i8()?,
        })
    }
}

/// The items of a reply that counts them in a byte before them.
pub struct Items<'a, T> {
    r: Reader<'a>,
    left: u8,
    item: fn(&mut Reader<'a>) -> Result<T, Short>,
}

impl<'a, T> Items<'a, T> {
    fn new(
        mut r: Reader<'a>,
        item: fn(&mut Reader<'a>) -> Result<T, Short>,
    ) -> Result<Self, Short> {
        let left = r.u8()?;
        Ok(Self { r, left, item })
    }
}

impl<T> Iterator for Items<'_, T> {
    type Item = Result<T, Short>;

    fn next(&mut self) -> Option<Self::Item> {
        self.left = self.left.checked_sub(1)?;
        Some((self.item)(&mut self.r))
    }
}

impl Status {
    pub fn encode(&self, out: &mut [u8]) -> usize {
        let mut w = Writer { buf: out, at: 0 };
        w.u8(VERSION);
        w.bytes(&self.firmware_version);
        w.bytes(&self.boot.bootloader);
        w.u16(self.boot.softdevice_fwid);
        w.u32(self.boot.softdevice_id);
        w.u32(self.boot.softdevice_version);
        w.u64(self.uptime_ms);
        w.u32(self.boot.reset_cause);
        w.u8(self.link_state);
        w.u8(self.channel);
        w.bytes(&self.own);
        w.bytes(&self.forced);
        w.bytes(&self.keyboard_id);
        w.u8(u8::from(self.usb_live) | u8::from(self.usb_suspended) << 1);
        w.u32(self.invalid);
        w.u32(self.dropped);
        w.u32(self.usb_retries);
        w.u32(self.merges);
        w.u32(self.surveys);
        w.u32(self.minute_next);
        w.u32(self.minute_oldest);
        w.u32(self.events_total);
        w.u32(self.events_oldest);
        w.u8(self.protocol);
        w.u8(self.pairing_window);
        w.u32(self.passkey);
        w.bytes(&self.passkey_name);
        w.bytes(&self.keyboard_serial);
        w.u8(self.keyboard_type);
        w.u8(self.slot_count);
        for s in self.slots.iter().take(self.slot_count.into()) {
            w.u8(s.kind);
            w.u8(if s.stored { SLOT_STORED } else { 0 } | if s.ready { SLOT_READY } else { 0 });
        }
        w.u8(self.slot);
        w.u8(self.last_rssi as u8);
        w.u32(self.last_rx_age_ms);
        w.u32(self.packets);
        w.u32(self.gaps);
        w.u32(self.link_losses);
        w.u32(self.latency_average_us);
        w.u32(self.latency_max_us);
        w.u8(self.pairing);
        w.bytes(&self.name);
        w.u8(self.battery.level);
        let charging = Charging::from_code(self.battery.charging).unwrap_or(Charging::Unknown);
        w.u8(charging as u8);
        w.at
    }

    /// Reads past the version byte, which the caller checks; the slot table keeps at most
    /// `SLOTS` entries.
    pub fn decode(payload: &[u8]) -> Result<Self, Short> {
        let mut r = Reader::new(payload);
        r.u8()?;
        let firmware_version = r.bytes()?;
        let bootloader = r.bytes()?;
        let (softdevice_fwid, softdevice_id, softdevice_version) = (r.u16()?, r.u32()?, r.u32()?);
        let uptime_ms = r.u64()?;
        let reset_cause = r.u32()?;
        let (link_state, channel) = (r.u8()?, r.u8()?);
        let (own, forced, keyboard_id) = (r.bytes()?, r.bytes()?, r.bytes()?);
        let usb = r.u8()?;
        let [invalid, dropped, usb_retries, merges, surveys] =
            [r.u32()?, r.u32()?, r.u32()?, r.u32()?, r.u32()?];
        let [minute_next, minute_oldest, events_total, events_oldest] =
            [r.u32()?, r.u32()?, r.u32()?, r.u32()?];
        let (protocol, pairing_window, passkey) = (r.u8()?, r.u8()?, r.u32()?);
        let passkey_name = r.bytes()?;
        let keyboard_serial = r.bytes()?;
        let (keyboard_type, slot_count) = (r.u8()?, r.u8()?);
        let mut slots = [Slot::default(); SLOTS];
        for i in 0..usize::from(slot_count) {
            let (kind, flags) = (r.u8()?, r.u8()?);
            // Entries past SLOTS are read past and dropped.
            if let Some(s) = slots.get_mut(i) {
                *s = Slot {
                    kind,
                    stored: flags & SLOT_STORED != 0,
                    ready: flags & SLOT_READY != 0,
                };
            }
        }
        let (slot, last_rssi, last_rx_age_ms) = (r.u8()?, r.i8()?, r.u32()?);
        let [packets, gaps, link_losses] = [r.u32()?, r.u32()?, r.u32()?];
        let [latency_average_us, latency_max_us] = [r.u32()?, r.u32()?];
        let (pairing, name) = (r.u8()?, r.bytes()?);
        let battery = Battery {
            level: r.u8()?,
            charging: r.u8()?,
        };
        Ok(Self {
            firmware_version,
            boot: Bootinfo {
                reset_cause,
                bootloader,
                softdevice_fwid,
                softdevice_id,
                softdevice_version,
            },
            uptime_ms,
            link_state,
            channel,
            own,
            forced,
            keyboard_id,
            usb_live: usb & 1 != 0,
            usb_suspended: usb & 2 != 0,
            invalid,
            dropped,
            usb_retries,
            merges,
            surveys,
            minute_next,
            minute_oldest,
            events_total,
            events_oldest,
            protocol,
            pairing_window,
            passkey,
            passkey_name,
            keyboard_serial,
            keyboard_type,
            slot_count,
            slots,
            slot,
            last_rssi,
            last_rx_age_ms,
            packets,
            gaps,
            link_losses,
            latency_average_us,
            latency_max_us,
            pairing,
            name,
            battery,
        })
    }
}

pub fn encode_minutes(out: &mut [u8], first: u32, minutes: &[Minute], slot: usize) -> usize {
    let mut w = Writer { buf: out, at: 0 };
    w.u32(first);
    w.u8(minutes.len() as u8);
    for m in minutes {
        w.u8(m.invalid);
        w.u8(m.dropped);
        w.u8(m.usb_retries);
        w.noise(&m.noise);
        w.u16(m.packets[slot]);
        w.u8(m.gaps[slot]);
        w.u8(m.link_losses[slot]);
        w.rssi(&m.rssi[slot]);
    }
    w.at
}

pub fn decode_minutes(payload: &[u8]) -> Result<(u32, Items<'_, ReplyMinute>), Short> {
    let mut r = Reader::new(payload);
    let first = r.u32()?;
    let items = Items::new(r, |r| {
        Ok(ReplyMinute {
            invalid: r.u8()?,
            dropped: r.u8()?,
            usb_retries: r.u8()?,
            noise: r.noise()?,
            packets: r.u16()?,
            gaps: r.u8()?,
            link_losses: r.u8()?,
            rssi: r.rssi()?,
        })
    })?;
    Ok((first, items))
}

pub fn encode_events(out: &mut [u8], first: u32, events: &[Event]) -> usize {
    let mut w = Writer { buf: out, at: 0 };
    w.u32(first);
    w.u8(events.len() as u8);
    for e in events {
        w.u64(e.time_ms);
        w.bytes(&[e.r#type, e.slot, e.a, e.b, e.c]);
        w.u32(e.value);
        w.noise(&e.noise);
    }
    w.at
}

pub fn decode_events(payload: &[u8]) -> Result<(u32, Items<'_, Event>), Short> {
    let mut r = Reader::new(payload);
    let first = r.u32()?;
    let items = Items::new(r, |r| {
        Ok(Event {
            time_ms: r.u64()?,
            r#type: r.u8()?,
            slot: r.u8()?,
            a: r.u8()?,
            b: r.u8()?,
            c: r.u8()?,
            value: r.u32()?,
            noise: r.noise()?,
        })
    })?;
    Ok((first, items))
}

/// `dbm` holds each channel's samples in turn, from channel `first` up.
pub fn encode_noise(out: &mut [u8], first: u8, samples: usize, dbm: &[i8]) -> usize {
    let mut w = Writer { buf: out, at: 0 };
    let channels = dbm.chunks_exact(samples);
    w.u8(first);
    w.u8(channels.len() as u8);
    for c in channels {
        let loudest = c.iter().copied().max().unwrap_or(0);
        let sum: i32 = c.iter().map(|&d| i32::from(d)).sum();
        w.u8(loudest as u8);
        w.u8((sum / samples as i32) as u8);
    }
    w.at
}

/// Each channel's loudest and average reading.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NoiseChannel {
    pub channel: u8,
    pub loudest: i8,
    pub average: i8,
}

pub fn decode_noise(
    payload: &[u8],
) -> Result<impl Iterator<Item = Result<NoiseChannel, Short>>, Short> {
    let mut r = Reader::new(payload);
    let first = r.u8()?;
    let mut channel = first;
    Ok(Items::new(r, |r| Ok((r.i8()?, r.i8()?)))?.map(move |n| {
        let (loudest, average) = n?;
        let c = NoiseChannel {
            channel,
            loudest,
            average,
        };
        channel = channel.wrapping_add(1);
        Ok(c)
    }))
}

/// The watch reply's fields before its channels.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct WatchHead {
    /// 0 without a home channel.
    pub home: u8,
    /// 0 without a threshold.
    pub threshold: u8,
    pub paused: bool,
    pub hold_left_ms: u32,
}

pub fn encode_watch(out: &mut [u8], head: &WatchHead, channels: &[WatchChannel]) -> usize {
    let mut w = Writer { buf: out, at: 0 };
    w.u8(head.home);
    w.u8(head.threshold);
    w.u8(head.paused.into());
    w.u32(head.hold_left_ms);
    w.u8(channels.len() as u8);
    for c in channels {
        w.u8(c.channel);
        w.u8(c.enough.into());
        w.u16(c.counted);
        for &b in &c.buckets {
            w.u32(b);
        }
    }
    w.at
}

pub fn decode_watch(payload: &[u8]) -> Result<(WatchHead, Items<'_, WatchChannel>), Short> {
    let mut r = Reader::new(payload);
    let head = WatchHead {
        home: r.u8()?,
        threshold: r.u8()?,
        paused: r.u8()? != 0,
        hold_left_ms: r.u32()?,
    };
    let items = Items::new(r, |r| {
        let (channel, enough, counted) = (r.u8()?, r.u8()? != 0, r.u16()?);
        let mut buckets = [0; BUCKETS];
        for b in &mut buckets {
            *b = r.u32()?;
        }
        Ok(WatchChannel {
            channel,
            enough,
            counted,
            buckets,
        })
    })?;
    Ok((head, items))
}

pub fn encode_devices(out: &mut [u8], devices: &[Device]) -> usize {
    let mut w = Writer { buf: out, at: 0 };
    w.u8(devices.len() as u8);
    for d in devices {
        w.bytes(&d.addr);
        w.bytes(&[d.kind, d.pairing, d.slot]);
        w.bytes(&d.name);
    }
    w.at
}

pub fn decode_devices(payload: &[u8]) -> Result<Items<'_, Device>, Short> {
    Items::new(Reader::new(payload), |r| {
        Ok(Device {
            addr: r.bytes()?,
            kind: r.u8()?,
            pairing: r.u8()?,
            slot: r.u8()?,
            name: r.bytes()?,
        })
    })
}
