//! The C interface to the diagnostic protocol: each `out` buffer comes with its capacity, and a
//! reply that would not fit stops the firmware rather than writing past it.

#![no_std]

use core::slice;

use diagwire::{
    Command, Device, Event, Minute, Protocol, Request, Status, WatchChannel, WatchHead,
};

/// # Safety
/// `ptr` is valid for `len` items, or `len` is 0.
unsafe fn items<'a, T>(ptr: *const T, len: usize) -> &'a [T] {
    if len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(ptr, len) }
    }
}

/// # Safety
/// `ptr` is valid for writes of `len` bytes.
unsafe fn out<'a>(ptr: *mut u8, len: usize) -> &'a mut [u8] {
    unsafe { slice::from_raw_parts_mut(ptr, len) }
}

/// Returns false for a report shorter than `DIAG_REQUEST_LEN`.
///
/// # Safety
/// `buf` is valid for `len` bytes and `req` for a write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_parse_request(buf: *const u8, len: usize, req: *mut Request) -> bool {
    match diagwire::parse_request(unsafe { items(buf, len) }) {
        Some(r) => {
            unsafe { req.write(r) };
            true
        }
        None => false,
    }
}

/// Returns false for an arg of zero, which clears the forced channels.
///
/// # Safety
/// `channels` is valid for writes of 3 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_channels_arg(arg: u32, channels: *mut u8) -> bool {
    match diagwire::channels_from_arg(arg) {
        Some(c) => {
            unsafe { out(channels, c.len()).copy_from_slice(&c) };
            true
        }
        None => false,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn diag_part_count(payload_len: usize) -> usize {
    diagwire::part_count(payload_len)
}

/// # Safety
/// `report` is valid for writes of `DIAG_REPORT_LEN` bytes, `req` for a read, and `payload` for
/// `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_part(
    report: *mut u8,
    req: *const Request,
    payload: *const u8,
    len: usize,
    index: usize,
) {
    let report = unsafe { &mut *report.cast::<[u8; diagwire::REPORT_LEN]>() };
    unsafe { diagwire::part(report, &*req, items(payload, len), index) }
}

/// `detail` is the active protocol for `DIAG_ERROR_WRONG_PROTOCOL` and 0 otherwise.
///
/// # Safety
/// `report` is valid for writes of `DIAG_REPORT_LEN` bytes and `req` for a read.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_error_report(
    report: *mut u8,
    req: *const Request,
    code: u8,
    detail: u8,
) {
    let report = unsafe { &mut *report.cast::<[u8; diagwire::REPORT_LEN]>() };
    unsafe { diagwire::error_report(report, &*req, code, detail) }
}

/// True for a command this firmware does not know, so the caller answers it as unknown.
#[unsafe(no_mangle)]
pub extern "C" fn diag_command_answered(command: u8, protocol: u8) -> bool {
    match (Command::from_code(command), Protocol::from_code(protocol)) {
        (Some(c), Some(p)) => c.answered(p),
        _ => true,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn diag_protocol_has_esb(protocol: u8) -> bool {
    Protocol::from_code(protocol).is_some_and(Protocol::has_esb)
}

#[unsafe(no_mangle)]
pub extern "C" fn diag_protocol_known(value: u32) -> bool {
    u8::try_from(value).is_ok_and(|v| Protocol::from_code(v).is_some())
}

/// The event `detail` records about `slot`, with no time or noise.
///
/// # Safety
/// `detail` is valid for a read and its tag is one of `diag_detail_Tag`'s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_event(slot: u8, detail: *const diagwire::Detail) -> diagwire::Event {
    unsafe { (*detail).event(slot) }
}

#[unsafe(no_mangle)]
pub extern "C" fn diag_since_input_s(elapsed_ms: i64) -> u16 {
    diagwire::since_input_s(elapsed_ms)
}

#[unsafe(no_mangle)]
pub extern "C" fn diag_noise_bucket(dbm: i8) -> u8 {
    diagwire::bucket(dbm)
}

#[unsafe(no_mangle)]
pub extern "C" fn diag_pairing(secure: bool, passkey: bool) -> u8 {
    diagwire::pairing_flags(secure, passkey)
}

/// # Safety
/// `buf` is valid for writes of `cap` bytes and `status` for a read.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_encode_status(
    buf: *mut u8,
    cap: usize,
    status: *const Status,
) -> usize {
    unsafe { (*status).encode(out(buf, cap)) }
}

/// Each minute carries the link-wide counters and the slot's own.
///
/// # Safety
/// `buf` is valid for writes of `cap` bytes and `minutes` for `n` reads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_encode_minutes(
    buf: *mut u8,
    cap: usize,
    first: u32,
    minutes: *const Minute,
    n: usize,
    slot: u8,
) -> usize {
    unsafe { diagwire::encode_minutes(out(buf, cap), first, items(minutes, n), slot.into()) }
}

/// # Safety
/// `buf` is valid for writes of `cap` bytes and `events` for `n` reads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_encode_events(
    buf: *mut u8,
    cap: usize,
    first: u32,
    events: *const Event,
    n: usize,
) -> usize {
    unsafe { diagwire::encode_events(out(buf, cap), first, items(events, n)) }
}

/// `dbm` holds `samples` readings for each channel in turn, from channel `first` up.
///
/// # Safety
/// `buf` is valid for writes of `cap` bytes and `dbm` for `len` reads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_encode_noise(
    buf: *mut u8,
    cap: usize,
    first: u8,
    samples: usize,
    dbm: *const i8,
    len: usize,
) -> usize {
    unsafe { diagwire::encode_noise(out(buf, cap), first, samples, items(dbm, len)) }
}

/// # Safety
/// `buf` is valid for writes of `cap` bytes, `head` for a read and `channels` for `n` reads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_encode_watch(
    buf: *mut u8,
    cap: usize,
    head: *const WatchHead,
    channels: *const WatchChannel,
    n: usize,
) -> usize {
    unsafe { diagwire::encode_watch(out(buf, cap), &*head, items(channels, n)) }
}

/// # Safety
/// `buf` is valid for writes of `cap` bytes and `devices` for `n` reads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn diag_encode_devices(
    buf: *mut u8,
    cap: usize,
    devices: *const Device,
    n: usize,
) -> usize {
    unsafe { diagwire::encode_devices(out(buf, cap), items(devices, n)) }
}

#[unsafe(no_mangle)]
#[allow(non_upper_case_globals)]
pub static diag_nuphy_fixed_channels: [u8; diagwire::NUPHY_FIXED_COUNT] =
    diagwire::NUPHY_FIXED_CHANNELS;

#[unsafe(no_mangle)]
#[allow(non_upper_case_globals)]
pub static diag_unifying_channels: [u8; diagwire::UNIFYING_CHANNEL_COUNT] =
    diagwire::UNIFYING_CHANNELS;

unsafe extern "C" {
    fn abort() -> !;
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { abort() }
}
