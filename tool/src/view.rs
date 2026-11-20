use std::ops::Range;
use std::time::{Duration, Instant};

use anyhow::Result;
use chrono::{DateTime, Duration as TimeDelta, Local};
use ratatui::{
    DefaultTerminal, Frame,
    crossterm::event::{self, Event as TermEvent, KeyCode, KeyEventKind},
    layout::{Constraint, Layout, Rect},
    style::{Color, Modifier, Style},
    symbols,
    text::{Line, Span},
    widgets::{
        Axis, Bar, BarChart, BarGroup, Block, Borders, Chart, Dataset, GraphType, List, ListItem,
        Paragraph,
        canvas::{Context, Painter},
    },
};

use crate::device::{self, Device, Lost, Transport};
use crate::format::{
    ChannelRole, LinkHealth, NOISE_QUESTION, PAIRING_OPEN, action_question, battery, bucket_label,
    channel_role, counted_share, describe_event, device_at, device_lines, firmware,
    forget_question, last_packet, latency, link_health, noise_level, noise_ruler, paired, pairing,
    share, slot_health, slot_state, state_name, status_label, switch_question, uptime, usb_state,
    wall_clock, watch_role, watch_summary,
};
use crate::protocol::{
    Action, BUSY_DBM, Counter, Event, Link, Minute, NOISE_BUCKETS, NoiseChannel, NuphyLink,
    Pairing, Protocol, SlotStatus, Status, Step, StoredDevice, Watch, shown_slots, step_channel,
};

const POLL_EVERY: Duration = Duration::from_secs(1);
const EVENTS_KEPT: usize = 200;
const EVENT_ROWS: usize = 8;
const CHART_MIN_ROWS: u16 = 5;
const TIME_AXIS_ROWS: u16 = 1;
/// The narrowest a slot's status box and chart draw uncut.
const PANEL_MIN_WIDTH: u16 = 59;
const SEPARATOR: &str = "   ";
/// A link that averaged -70 dBm had no gaps, and gaps followed packets at -78 dBm or weaker.
const SIGNAL_GOOD_DBM: i8 = -70;
const SIGNAL_WEAK_DBM: i8 = -78;
const CLOCK: &str = "%H:%M:%S";

/// Wide enough for a strip's name beside its scale at 100.00%.
const GUTTER: u16 = 12;
const DBM_FLOOR: i8 = -100;
const DBM_CEILING: i8 = -20;
const DBM_BOUNDS: [f64; 2] = [DBM_FLOOR as f64, DBM_CEILING as f64];
const DBM_LABELS: [i8; 3] = [DBM_FLOOR, -60, DBM_CEILING];
const DBM_MARKER: symbols::Marker = symbols::Marker::Braille;

type MinuteDbm = fn(&Minute) -> Option<i8>;

type SignalLine = (&'static str, Color, MinuteDbm);

const NOISE_LINE: SignalLine = ("noise peak", Color::Gray, |m| m.noise.map(|n| n.peak));

/// Each line lies over the ones before it, and all lie over the noise.
const SIGNAL_LINES: [SignalLine; 3] = [
    ("min", Color::Red, |m| m.device.rssi.map(|r| r.min)),
    ("avg", Color::Yellow, |m| m.device.rssi.map(|r| r.avg)),
    ("max", Color::Green, |m| m.device.rssi.map(|r| r.max)),
];

#[derive(Clone, Copy)]
enum Source {
    Counter(Counter),
    Busy,
}

struct Strip {
    name: &'static str,
    source: Source,
}

impl Strip {
    fn value(&self, m: &Minute) -> u16 {
        match self.source {
            Source::Counter(counter) => m.get(counter).or(m.device.get(counter)).unwrap_or(0),
            Source::Busy => m.noise.map_or(0, |n| n.busy),
        }
    }

    fn scale(&self, top: u16) -> String {
        match self.source {
            Source::Counter(_) => top.to_string(),
            Source::Busy => share(top),
        }
    }
}

const STRIPS: [Strip; 5] = [
    Strip {
        name: "losses",
        source: Source::Counter(Counter::LinkLosses),
    },
    Strip {
        name: "gaps",
        source: Source::Counter(Counter::Gaps),
    },
    Strip {
        name: "dropped",
        source: Source::Counter(Counter::Dropped),
    },
    Strip {
        name: "channel",
        source: Source::Counter(Counter::ChannelChanges),
    },
    Strip {
        name: "busy",
        source: Source::Busy,
    },
];

/// With no protocol, the protocols the dongle might run.
fn possible(protocol: Option<Protocol>) -> Vec<Protocol> {
    protocol.map_or(Protocol::ALL.to_vec(), |protocol| vec![protocol])
}

/// The dongle reads noise only between packets on a channel.
fn noise_read(protocol: Option<Protocol>) -> bool {
    possible(protocol).iter().all(|p| p.has_channels())
}

/// With no protocol, only the strips every protocol's firmware counts.
fn strips(protocol: Option<Protocol>) -> impl Iterator<Item = &'static Strip> {
    let counting = possible(protocol);
    STRIPS.iter().filter(move |strip| match strip.source {
        Source::Counter(counter) => counting.iter().all(|p| p.counts(counter)),
        Source::Busy => noise_read(protocol),
    })
}

fn signal_lines(protocol: Option<Protocol>) -> Vec<SignalLine> {
    noise_read(protocol)
        .then_some(NOISE_LINE)
        .into_iter()
        .chain(SIGNAL_LINES)
        .collect()
}

fn panel_rows(strips: usize) -> [Constraint; 3] {
    [
        Constraint::Min(CHART_MIN_ROWS),
        Constraint::Length(strips as u16),
        Constraint::Length(TIME_AXIS_ROWS),
    ]
}

fn history_min_rows(protocol: Option<Protocol>) -> u16 {
    let rows: u16 = panel_rows(strips(protocol).count())
        .iter()
        .map(|row| match row {
            Constraint::Min(n) | Constraint::Length(n) => *n,
            _ => panic!("a panel row needs a fixed minimum"),
        })
        .sum();
    2 + rows
}

/// A frame's title or caption, spaced off the border on both sides.
fn padded<'a>(title: impl Into<Line<'a>>) -> Line<'a> {
    let mut line = title.into();
    line.spans.insert(0, Span::raw(" "));
    line.spans.push(Span::raw(" "));
    line
}

fn status_row(label: &str, spans: impl IntoIterator<Item = Span<'static>>) -> Line<'static> {
    Line::from_iter(std::iter::once(Span::raw(status_label(label))).chain(spans))
}

fn link_color(health: LinkHealth) -> Color {
    match health {
        LinkHealth::Connected => Color::Green,
        LinkHealth::Pairing => Color::Cyan,
        LinkHealth::Waiting => Color::Yellow,
        LinkHealth::NotPaired => Color::Red,
    }
}

fn signal_color(dbm: i8) -> Color {
    match dbm {
        SIGNAL_GOOD_DBM.. => Color::Green,
        SIGNAL_WEAK_DBM.. => Color::Yellow,
        _ => Color::Red,
    }
}

fn pairing_color(p: Pairing) -> Color {
    match (p.secure, p.passkey) {
        (true, true) => Color::Green,
        (false, false) => Color::Red,
        _ => Color::Yellow,
    }
}

fn role_style(role: ChannelRole) -> Style {
    match role {
        ChannelRole::Current => Style::new().add_modifier(Modifier::REVERSED),
        ChannelRole::Paired => Style::new().add_modifier(Modifier::BOLD),
        ChannelRole::Other => Style::new(),
    }
}

fn channel_spans(s: &Status) -> Vec<Span<'static>> {
    let mut channels = s.keyboard_channels();
    if let Err(at) = channels.binary_search(&s.channel) {
        channels.insert(at, s.channel);
    }
    let mut spans = Vec::new();
    for channel in channels {
        if !spans.is_empty() {
            spans.push(Span::raw(" "));
        }
        spans.push(Span::styled(
            channel.to_string(),
            role_style(channel_role(channel, s)),
        ));
    }
    if let Link::Nuphy(NuphyLink { own: None, .. }) = &s.link {
        spans.push(Span::raw(format!("{SEPARATOR}{}", paired(None))));
    }
    spans
}

/// A chart line joins neighboring points, so each minute without a reading ends a run.
fn dbm_runs(shown: &[(u32, Minute)], pick: MinuteDbm) -> Vec<Vec<(f64, f64)>> {
    let points: Vec<Option<(f64, f64)>> = shown
        .iter()
        .enumerate()
        .map(|(x, (_, minute))| {
            pick(minute).map(|dbm| (x as f64, f64::from(dbm.clamp(DBM_FLOOR, DBM_CEILING))))
        })
        .collect();
    points
        .split(Option::is_none)
        .filter(|run| !run.is_empty())
        .map(|run| run.iter().flatten().copied().collect())
        .collect()
}

/// The row the chart's canvas plots a reading on.
fn dbm_row(dbm: i8, rows: u16) -> Option<usize> {
    let mut canvas = Context::new(1, rows, [0.0, 1.0], DBM_BOUNDS, DBM_MARKER);
    let (_, dot) = Painter::from(&mut canvas).get_point(0.0, f64::from(dbm))?;
    Some(dot / 4)
}

/// Rounds each bar up, so a nonzero value never draws blank beside a much larger one.
fn bar_strip(values: &[u16]) -> (u16, String) {
    use symbols::bar::*;
    const BARS: [&str; 8] = [
        ONE_EIGHTH,
        ONE_QUARTER,
        THREE_EIGHTHS,
        HALF,
        FIVE_EIGHTHS,
        THREE_QUARTERS,
        SEVEN_EIGHTHS,
        FULL,
    ];
    let top = values.iter().copied().max().unwrap_or(0);
    let strip = values
        .iter()
        .map(|&v| match v {
            0 => " ",
            _ => BARS[(u32::from(v) * 8).div_ceil(u32::from(top)) as usize - 1],
        })
        .collect();
    (top, strip)
}

const MINUTE_TICK: char = '╵';

fn time_axis(boot: DateTime<Local>, shown: &[(u32, Minute)]) -> String {
    let at = |column: usize| {
        (boot + TimeDelta::minutes(i64::from(shown[column].0)))
            .format(CLOCK)
            .to_string()
    };
    let Some(last) = shown.len().checked_sub(1) else {
        return String::new();
    };
    let first = at(0);
    let span = 1 + first.len();
    let apart = |left: usize, right: usize| left + span + 2 <= right;
    let last_start = shown
        .len()
        .checked_sub(span)
        .filter(|start| apart(0, *start));
    let middle = shown.len() / 2;
    let show_middle = apart(0, middle) && last_start.is_none_or(|l| apart(middle, l));
    let labels = [
        Some((0, format!("{MINUTE_TICK}{first}"))),
        show_middle.then(|| (middle, format!("{MINUTE_TICK}{}", at(middle)))),
        last_start.map(|start| (start, format!("{}{MINUTE_TICK}", at(last)))),
    ];
    labels
        .into_iter()
        .flatten()
        .fold(String::new(), |line, (start, label)| {
            format!("{line:start$}{label}")
        })
}

fn legend<N: Into<std::borrow::Cow<'static, str>>>(
    title: &'static str,
    glyph: &'static str,
    keys: impl IntoIterator<Item = (N, Color)>,
) -> Line<'static> {
    Line::from_iter(
        [Span::raw(title)]
            .into_iter()
            .chain(keys.into_iter().flat_map(|(name, color)| {
                [
                    Span::raw("  "),
                    Span::styled(glyph, Style::default().fg(color)),
                    Span::raw(name),
                ]
            })),
    )
}

fn role_color(role: ChannelRole) -> Color {
    match role {
        ChannelRole::Current => Color::Green,
        ChannelRole::Paired => Color::Yellow,
        ChannelRole::Other => Color::Gray,
    }
}

/// One shown slot's status and minutes as last fetched.
pub(crate) struct SlotHistory {
    pub(crate) device: SlotStatus,
    pub(crate) minutes: Vec<(u32, Minute)>,
}

/// The dongle's status and history as last fetched, and the view's place in them, kept in step
/// with its indexes.
pub(crate) struct History {
    /// Slot 0's status, which carries the link-wide fields and the slot table.
    pub(crate) status: Status,
    pub(crate) boot: DateTime<Local>,
    /// The shown slots, in slot order.
    pub(crate) slots: Vec<SlotHistory>,
    pub(crate) events: Vec<(u32, Event)>,
    /// The number of the event shown at the top of the events list, or `None` to follow the newest.
    pub(crate) events_top: Option<u32>,
    /// The minutes the chart's right edge sits before the newest one.
    pub(crate) minutes_back: usize,
    pub(crate) devices: Vec<StoredDevice>,
}

fn boot_time(s: &Status) -> DateTime<Local> {
    wall_clock(Local::now(), s.uptime_ms, 0)
}

impl History {
    pub(crate) fn new(status: Status) -> Self {
        let slots = shown_slots(&status.slots)
            .contains(&status.device.slot)
            .then(|| SlotHistory {
                device: status.device.clone(),
                minutes: Vec::new(),
            });
        History {
            boot: boot_time(&status),
            slots: slots.into_iter().collect(),
            status,
            events: Vec::new(),
            events_top: None,
            minutes_back: 0,
            devices: Vec::new(),
        }
    }

    /// `minutes_back` clamped so the chart stays full.
    fn scrolled(&self, width: usize) -> usize {
        let held = self.slots.first().map_or(0, |slot| slot.minutes.len());
        self.minutes_back.min(held.saturating_sub(width))
    }

    fn scroll_older(&mut self, width: usize) {
        self.minutes_back = self.scrolled(width) + HISTORY_STEP_MINUTES;
        self.minutes_back = self.scrolled(width);
    }

    fn scroll_newer(&mut self, width: usize) {
        self.minutes_back = self.scrolled(width).saturating_sub(HISTORY_STEP_MINUTES);
    }

    fn scroll_events(&mut self, step: EventsMove, rows: usize) {
        self.events_top = scrolled_top(&self.events, self.events_top, rows, step);
    }

    /// Keeps `s` even when a fetch fails.
    pub(crate) fn refresh<T: Transport>(&mut self, dev: &mut Device<T>, s: Status) -> Result<()> {
        if s.uptime_ms < self.status.uptime_ms {
            *self = History::new(s);
        } else {
            self.status = s;
        }
        let shown = shown_slots(&self.status.slots);
        self.slots.retain(|h| shown.contains(&h.device.slot));
        for slot in shown {
            let s = &self.status;
            let device = if slot == s.device.slot {
                s.device.clone()
            } else {
                dev.slot_status(slot)?
            };
            let at = self.slots.partition_point(|h| h.device.slot < slot);
            if self.slots.get(at).is_none_or(|h| h.device.slot != slot) {
                let minutes = Vec::new();
                self.slots.insert(at, SlotHistory { device, minutes });
            } else {
                self.slots[at].device = device;
            }
            let minutes = &mut self.slots[at].minutes;
            minutes.retain(|(i, _)| *i >= s.minute_oldest && *i < s.minute_next);
            let from = minutes.last().map_or(s.minute_oldest, |(i, _)| i + 1);
            minutes.extend(dev.minutes_between(from, s.minute_next, slot, s.protocol())?);
        }
        let s = &self.status;
        self.events
            .retain(|(i, _)| *i >= s.events_oldest && *i < s.events_total);
        let from = self.events.last().map_or(
            s.events_total.saturating_sub(EVENTS_KEPT as u32),
            |(i, _)| i + 1,
        );
        self.events
            .extend(dev.events_between(from, s.events_total)?);
        let excess = self.events.len().saturating_sub(EVENTS_KEPT);
        self.events.drain(..excess);
        self.devices = crate::cli::stored_devices(dev, self.status.protocol())?;
        Ok(())
    }
}

const HISTORY_STEP_MINUTES: usize = 10;

#[derive(Clone, Copy)]
enum EventsMove {
    Older(usize),
    Newer(usize),
    Newest,
    Oldest,
}

/// The held events to show, oldest first, ending at the one numbered `top` or, without one, at the
/// newest.
fn events_window(events: &[(u32, Event)], top: Option<u32>, rows: usize) -> Range<usize> {
    let end = top.map_or(events.len(), |top| {
        events.partition_point(|(i, _)| *i <= top)
    });
    let end = at_least_a_page(end, events.len(), rows);
    end.saturating_sub(rows)..end
}

fn at_least_a_page(end: usize, len: usize, rows: usize) -> usize {
    end.max(rows.min(len))
}

fn scrolled_top(
    events: &[(u32, Event)],
    top: Option<u32>,
    rows: usize,
    step: EventsMove,
) -> Option<u32> {
    let end = events_window(events, top, rows).end;
    let end = match step {
        EventsMove::Older(n) => end.saturating_sub(n),
        EventsMove::Newer(n) => end + n,
        EventsMove::Newest => events.len(),
        EventsMove::Oldest => 0,
    };
    let end = at_least_a_page(end, events.len(), rows);
    (end < events.len()).then(|| events[end - 1].0)
}

#[derive(Clone)]
enum Prompt {
    Noise,
    Action(Action),
    Switch(Protocol),
    Forget(StoredDevice),
}

impl Prompt {
    fn question(&self) -> String {
        let question = match self {
            Prompt::Noise => NOISE_QUESTION.to_string(),
            Prompt::Action(action) => action_question(*action).to_string(),
            Prompt::Switch(to) => switch_question(*to),
            Prompt::Forget(device) => forget_question(device),
        };
        format!("{question} y/n")
    }
}

fn other_protocols(running: Protocol) -> impl Iterator<Item = Protocol> {
    Protocol::ALL.into_iter().filter(move |p| *p != running)
}

fn choice_question(running: Protocol) -> String {
    let choices: Vec<String> = other_protocols(running)
        .zip(1..)
        .map(|(p, n)| format!("{n} {}", p.name()))
        .collect();
    format!(
        "Switch to which protocol? {}  (any other key cancels)",
        choices.join("  ")
    )
}

fn chosen(running: Protocol, key: KeyCode) -> Option<Protocol> {
    let KeyCode::Char(c) = key else { return None };
    let n = c.to_digit(10)? as usize;
    other_protocols(running).nth(n.checked_sub(1)?)
}

const PASSKEY_QUESTION: &str =
    "Pair how? 1 with a passkey  2 without a passkey  (any other key cancels)";

fn chosen_no_passkey(key: KeyCode) -> Option<bool> {
    match key {
        KeyCode::Char('1') => Some(false),
        KeyCode::Char('2') => Some(true),
        _ => None,
    }
}

#[derive(Clone, Copy)]
enum Choice {
    Protocol,
    Passkey,
}

impl Choice {
    fn question(self, running: Protocol) -> String {
        match self {
            Choice::Protocol => choice_question(running),
            Choice::Passkey => PASSKEY_QUESTION.to_string(),
        }
    }
}

#[derive(Clone)]
enum Key {
    Ask(Prompt),
    Choose(Choice),
    Pair,
    Moves,
    Devices,
}

fn action_keys(protocol: Protocol) -> Vec<(char, &'static str, Key)> {
    const NOISE: (char, &str, Key) = ('n', "noise", Key::Ask(Prompt::Noise));
    const MOVES: (char, &str, Key) = ('m', "moves", Key::Moves);
    const REBOOT: (char, &str, Key) = ('r', "reboot", Key::Ask(Prompt::Action(Action::Reboot)));
    const BOOTLOADER: (char, &str, Key) = (
        'b',
        "bootloader",
        Key::Ask(Prompt::Action(Action::Bootloader)),
    );
    const CLEAR: (char, &str, Key) = ('c', "clear", Key::Ask(Prompt::Action(Action::Clear)));
    const DEVICES: (char, &str, Key) = ('d', "devices", Key::Devices);
    let channels = protocol.has_channels();
    let pair = if protocol == Protocol::Bluetooth {
        ('a', "pair", Key::Choose(Choice::Passkey))
    } else {
        ('a', "pair", Key::Pair)
    };
    let switch = ('p', "protocol", Key::Choose(Choice::Protocol));
    [
        channels.then_some(NOISE),
        channels.then_some(MOVES),
        Some(pair),
        (protocol == Protocol::Bluetooth).then_some(DEVICES),
        Some(switch),
        Some(REBOOT),
        Some(BOOTLOADER),
        Some(CLEAR),
    ]
    .into_iter()
    .flatten()
    .collect()
}

/// Packs pieces greedily into lines of at most `width` columns, so a line breaks only between pieces.
fn flow<'a>(
    pieces: impl IntoIterator<Item = &'a str>,
    separator: &str,
    width: usize,
) -> Vec<String> {
    let mut lines: Vec<String> = Vec::new();
    for piece in pieces {
        match lines.last_mut() {
            Some(line)
                if line.chars().count() + separator.chars().count() + piece.chars().count()
                    <= width =>
            {
                line.push_str(separator);
                line.push_str(piece);
            }
            _ => lines.push(piece.to_string()),
        }
    }
    lines
}

/// Holds every cause, since the outermost context alone often omits why it failed.
fn error_text(e: &anyhow::Error) -> String {
    format!("{e:#}")
}

struct Session<T: Transport> {
    device: Device<T>,
    history: History,
    /// The place in `history.slots` of the leftmost panel drawn.
    first_panel: usize,
    /// The last refresh's error, while the dongle answers but its reply failed.
    refresh_error: Option<String>,
    pane: Pane,
    prompt: Option<Prompt>,
    choosing: Option<Choice>,
    message: String,
}

enum Pane {
    History,
    Noise(Vec<NoiseChannel>),
    /// Reread on every poll.
    Moves(Watch),
    Devices,
}

#[expect(
    clippy::large_enum_variant,
    reason = "one Connection lives for the whole run"
)]
enum Connection<T: Transport> {
    /// Holds the footer's line.
    Disconnected(String),
    Connected(Session<T>),
}

struct App<T: Transport = device::HidTransport> {
    serial: Option<String>,
    connection: Connection<T>,
    drawn: Drawn,
}

enum Outcome {
    Quit,
    /// Answered after a redraw, since a noise survey blocks for seconds.
    Answer(Prompt),
}

#[derive(Clone, Copy, Default)]
struct Drawn {
    event_rows: usize,
    /// `None` while the history chart is not on screen.
    plot_width: Option<usize>,
    /// Whether some shown slot's panel was left out.
    cycling: bool,
}

pub fn run(serial: Option<String>) -> Result<()> {
    let mut terminal = ratatui::init();
    let mut app: App = App::new(serial);
    let result = app.run(&mut terminal);
    ratatui::restore();
    result
}

impl App {
    fn run(&mut self, terminal: &mut DefaultTerminal) -> Result<()> {
        let mut last_poll: Option<Instant> = None;
        loop {
            if last_poll.is_none_or(|t| t.elapsed() >= POLL_EVERY) {
                self.poll();
                last_poll = Some(Instant::now());
            }
            terminal.draw(|f| self.draw(f))?;
            if !event::poll(Duration::from_millis(100))? {
                continue;
            }
            let TermEvent::Key(key) = event::read()? else {
                continue;
            };
            if key.kind != KeyEventKind::Press {
                continue;
            }
            match self.handle(key.code) {
                Some(Outcome::Quit) => return Ok(()),
                Some(Outcome::Answer(prompt)) => {
                    terminal.draw(|f| self.draw(f))?;
                    if let Some(session) = self.session_mut() {
                        session.answer(prompt);
                    }
                }
                None => {}
            }
        }
    }

    fn poll(&mut self) {
        let status = match self.session_mut() {
            Some(session) => session.device.status(),
            None => match device::open(self.serial.as_deref()) {
                Ok((device, status)) => {
                    self.connection = Connection::Connected(Session::new(device, status.clone()));
                    Ok(status)
                }
                Err(e) => {
                    self.disconnect(&e);
                    return;
                }
            },
        };
        self.refresh(status);
    }
}

impl<T: Transport> Session<T> {
    /// The shown slots whose panels the view draws at `width`, left to right.
    fn panels(&self, width: u16) -> Vec<&SlotHistory> {
        let slots = &self.history.slots;
        let drawn = panels_fitting(width).min(slots.len());
        (0..drawn)
            .map(|i| &slots[(self.first_panel + i) % slots.len()])
            .collect()
    }

    /// Whether `width` leaves out some shown slot's panel.
    fn cycling(&self, width: u16) -> bool {
        panels_fitting(width) < self.history.slots.len()
    }

    fn new(device: Device<T>, status: Status) -> Self {
        Session {
            device,
            history: History::new(status),
            first_panel: 0,
            refresh_error: None,
            pane: Pane::History,
            prompt: None,
            choosing: None,
            message: String::new(),
        }
    }

    fn protocol(&self) -> Protocol {
        self.history.status.protocol()
    }

    fn handle(&mut self, key: KeyCode, drawn: Drawn) -> Option<Outcome> {
        self.message.clear();
        if let Some(choice) = self.choosing.take() {
            match choice {
                Choice::Protocol => self.prompt = chosen(self.protocol(), key).map(Prompt::Switch),
                Choice::Passkey => {
                    if let Some(no_passkey) = chosen_no_passkey(key) {
                        self.pair(no_passkey);
                    }
                }
            }
            return None;
        }
        if let Some(prompt) = self.prompt.take() {
            return (key == KeyCode::Char('y')).then(|| {
                self.message = "working…".into();
                Outcome::Answer(prompt)
            });
        }
        let rows = drawn.event_rows;
        let history = &mut self.history;
        match key {
            KeyCode::Char('q') => return Some(Outcome::Quit),
            KeyCode::Char('[') => self.step(Step::Down),
            KeyCode::Char(']') => self.step(Step::Up),
            KeyCode::Char('o') if drawn.cycling => {
                self.first_panel = (self.first_panel + 1) % history.slots.len();
            }
            KeyCode::Char(c)
                if let Pane::Devices = self.pane
                    && let Some(place) = c.to_digit(10)
                    && let Some(device) = device_at(&history.devices, place as usize) =>
            {
                self.prompt = Some(Prompt::Forget(device.clone()));
            }
            KeyCode::Char(c) => match self.key(c) {
                Some(Key::Ask(prompt)) => self.prompt = Some(prompt),
                Some(Key::Choose(choice)) => self.choosing = Some(choice),
                Some(Key::Pair) => self.pair(false),
                Some(Key::Moves) => self.show_moves(),
                Some(Key::Devices) => self.pane = Pane::Devices,
                None => {}
            },
            KeyCode::Esc => self.pane = Pane::History,
            KeyCode::Left if let Some(width) = drawn.plot_width => history.scroll_older(width),
            KeyCode::Right if let Some(width) = drawn.plot_width => history.scroll_newer(width),
            KeyCode::Up => history.scroll_events(EventsMove::Newer(1), rows),
            KeyCode::Down => history.scroll_events(EventsMove::Older(1), rows),
            KeyCode::PageUp => history.scroll_events(EventsMove::Newer(rows), rows),
            KeyCode::PageDown => history.scroll_events(EventsMove::Older(rows), rows),
            KeyCode::Home => history.scroll_events(EventsMove::Newest, rows),
            KeyCode::End => history.scroll_events(EventsMove::Oldest, rows),
            _ => {}
        }
        None
    }

    fn key(&self, c: char) -> Option<Key> {
        action_keys(self.protocol())
            .into_iter()
            .find(|(k, _, _)| *k == c)
            .map(|(_, _, key)| key)
    }

    fn show_moves(&mut self) {
        match self.device.watch() {
            Ok(w) => self.pane = Pane::Moves(w),
            Err(e) => self.message = format!("failed: {}", error_text(&e)),
        }
    }

    fn pair(&mut self, no_passkey: bool) {
        let protocol = self.protocol();
        self.message = crate::cli::pair(&mut self.device, protocol, no_passkey)
            .unwrap_or_else(|e| format!("failed: {}", error_text(&e)));
    }

    fn step(&mut self, step: Step) {
        let status = &mut self.history.status;
        let Some(to) = step_channel(&status.keyboard_channels(), status.channel, step) else {
            return;
        };
        self.message = match crate::cli::move_link(&mut self.device, status, to) {
            Ok(line) => {
                status.channel = to;
                line
            }
            Err(e) => format!("failed: {}", error_text(&e)),
        };
    }

    /// Returns the error that lost the dongle.
    fn refresh(&mut self, status: Result<Status>) -> Result<()> {
        let read = status.is_ok();
        let refreshed = status
            .and_then(|s| self.history.refresh(&mut self.device, s))
            .and_then(|()| self.reread_pane());
        match refreshed {
            Ok(()) => self.refresh_error = None,
            Err(e) if e.is::<Lost>() => return Err(e),
            Err(e) => self.refresh_error = Some(error_text(&e)),
        }
        if read {
            self.drop_pairing_message_once_closed();
        }
        Ok(())
    }

    fn reread_pane(&mut self) -> Result<()> {
        if let Pane::Moves(w) = &mut self.pane {
            *w = self.device.watch()?;
        }
        Ok(())
    }

    /// The dongle closes its pairing window on its own, so the message outlives the window.
    fn drop_pairing_message_once_closed(&mut self) {
        let status = &self.history.status;
        if !status.pairing_open && self.message.starts_with(PAIRING_OPEN) {
            self.message.clear();
        }
    }

    fn answer(&mut self, prompt: Prompt) {
        let result = match prompt {
            Prompt::Noise => self.device.noise().map(|n| {
                self.pane = Pane::Noise(n);
                "done".to_string()
            }),
            Prompt::Action(c) => self.device.action(c).map(|()| "done".to_string()),
            Prompt::Forget(stored) => self
                .device
                .forget(&stored)
                .map(|()| "forgotten".to_string()),
            Prompt::Switch(to) => self.device.protocol(Some(to)).map(|(_, rebooting)| {
                if rebooting {
                    format!("switching to {}; the dongle reboots", to.name())
                } else {
                    format!("the dongle already runs {}", to.name())
                }
            }),
        };
        self.message = result.unwrap_or_else(|e| format!("failed: {}", error_text(&e)));
    }

    fn help(&self, width: usize) -> Vec<String> {
        if let Some(choice) = self.choosing {
            return flow(choice.question(self.protocol()).split(' '), " ", width);
        }
        match &self.prompt {
            Some(p) => flow(p.question().split(' '), " ", width),
            None if !self.message.is_empty() => flow(self.message.split(' '), " ", width),
            None => match &self.refresh_error {
                Some(e) => flow(e.split(' '), " ", width),
                None => {
                    let mut hints: Vec<_> = action_keys(self.protocol())
                        .into_iter()
                        .map(|(key, label, _)| format!("{key} {label}"))
                        .collect();
                    if self.cycling(u16::try_from(width).unwrap_or(u16::MAX)) {
                        hints.push("o device".into());
                    }
                    if self.protocol().has_channels() {
                        hints.push("[/] channel".into());
                    }
                    hints.extend(
                        ["←/→ history", "↑/↓ events", "esc back", "q quit"].map(String::from),
                    );
                    flow(hints.iter().map(String::as_str), "  ", width)
                }
            },
        }
    }
}

impl<T: Transport> App<T> {
    fn new(serial: Option<String>) -> Self {
        App {
            serial,
            connection: Connection::Disconnected("disconnected".into()),
            drawn: Drawn::default(),
        }
    }

    fn session_mut(&mut self) -> Option<&mut Session<T>> {
        match &mut self.connection {
            Connection::Connected(session) => Some(session),
            Connection::Disconnected(_) => None,
        }
    }

    fn handle(&mut self, key: KeyCode) -> Option<Outcome> {
        let drawn = self.drawn;
        match self.session_mut() {
            Some(session) => session.handle(key, drawn),
            None => (key == KeyCode::Char('q')).then_some(Outcome::Quit),
        }
    }

    fn refresh(&mut self, status: Result<Status>) {
        if let Some(session) = self.session_mut()
            && let Err(e) = session.refresh(status)
        {
            self.disconnect(&e);
        }
    }

    fn disconnect(&mut self, reason: &anyhow::Error) {
        self.connection = Connection::Disconnected(format!("disconnected: {}", error_text(reason)));
    }

    fn draw(&mut self, f: &mut Frame) {
        let width = f.area().width;
        let (session, help) = self.connection.view(usize::from(width));
        let history = session.map(|s| &s.history);
        let status = history.map(|h| &h.status);
        let protocol = status.map(Status::protocol);
        let dongle_rows = dongle_lines(status);
        let [dongle, top, middle, bottom, footer] = Layout::vertical([
            Constraint::Length(dongle_rows.len() as u16 + 2),
            Constraint::Length(device_rows(protocol) + 2),
            Constraint::Min(history_min_rows(protocol)),
            Constraint::Max(EVENT_ROWS as u16 + 2),
            Constraint::Length(help.len() as u16),
        ])
        .areas(f.area());
        let mut panels: Vec<Option<&SlotHistory>> = session
            .map_or(Vec::new(), |s| s.panels(width))
            .into_iter()
            .map(Some)
            .collect();
        if panels.is_empty() {
            panels.push(None);
        }
        let cycling = session.is_some_and(|s| s.cycling(width));
        f.render_widget(
            Paragraph::new(dongle_rows)
                .block(Block::bordered().title(padded(dongle_title(status)))),
            dongle,
        );
        let columns = Layout::horizontal(vec![Constraint::Fill(1); panels.len()]);
        for (area, slot) in columns.split(top).iter().zip(&panels) {
            let device = slot.map(|h| &h.device);
            let mut block = Block::bordered();
            if let Some((s, d)) = status.zip(device) {
                block = block.title(padded(device_title(s, d)));
            }
            f.render_widget(
                Paragraph::new(slot_lines(status, device)).block(block),
                *area,
            );
        }
        let plot_width = match session.map(|s| (&s.pane, &s.history)) {
            Some((Pane::Noise(noise), history)) => {
                draw_noise(f, middle, noise, &history.status);
                None
            }
            Some((Pane::Moves(watch), history)) => {
                draw_moves(f, middle, watch, &history.status);
                None
            }
            Some((Pane::Devices, history)) => {
                draw_devices(f, middle, history);
                None
            }
            Some((Pane::History, _)) | None => columns
                .split(middle)
                .iter()
                .zip(&panels)
                .map(|(area, slot)| draw_history(f, *area, history, *slot))
                .min(),
        };
        let event_rows = draw_events(f, bottom, history);
        f.render_widget(Paragraph::new(help.join("\n")), footer);
        self.drawn = Drawn {
            event_rows,
            plot_width,
            cycling,
        };
    }
}

fn dongle_title(status: Option<&Status>) -> String {
    std::iter::once(crate::PRODUCT)
        .chain(status.map(|s| s.protocol().name()))
        .collect::<Vec<_>>()
        .join(" · ")
}

fn device_title(s: &Status, d: &SlotStatus) -> String {
    std::iter::once(s.kind_name(d.slot).to_string())
        .chain(d.name.clone())
        .chain(d.battery.map(battery))
        .collect::<Vec<_>>()
        .join(" · ")
}

impl<T: Transport> Connection<T> {
    fn view(&self, width: usize) -> (Option<&Session<T>>, Vec<String>) {
        match self {
            Connection::Connected(session) => (Some(session), session.help(width)),
            Connection::Disconnected(line) => {
                let mut lines = flow(line.split(' '), " ", width);
                lines.push("q quit".into());
                (None, lines)
            }
        }
    }
}

fn panels_fitting(width: u16) -> usize {
    usize::from(width / PANEL_MIN_WIDTH).max(1)
}

fn dongle_lines(status: Option<&Status>) -> Vec<Line<'static>> {
    let Some(s) = status else {
        return vec![Line::from("disconnected")];
    };
    let mut dongle = vec![Span::raw(format!(
        "usb {}{SEPARATOR}up {}",
        usb_state(s.usb_live, s.usb_suspended),
        uptime(s.uptime_ms)
    ))];
    if s.pairing_open {
        dongle.push(Span::raw(SEPARATOR));
        dongle.push(Span::styled(
            "pairing open",
            Style::new().fg(link_color(LinkHealth::Pairing)),
        ));
    }
    let mut rows = vec![status_row("dongle", dongle)];
    if s.protocol().has_channels() {
        rows.push(status_row("channels", channel_spans(s)));
    }
    rows.push(status_row(
        "firmware",
        [Span::raw(firmware(s).join(SEPARATOR))],
    ));
    rows
}

fn device_rows(protocol: Option<Protocol>) -> u16 {
    2 + u16::from(protocol.is_some_and(Protocol::reports_pairing))
}

fn slot_lines(status: Option<&Status>, device: Option<&SlotStatus>) -> Vec<Line<'static>> {
    let Some(s) = status else {
        return Vec::new();
    };
    let (state, health) = match device.and_then(|d| s.slot(d.slot)) {
        Some(slot) => (slot_state(s, slot), slot_health(s, slot)),
        None => (state_name(s), link_health(s)),
    };
    let mut link = vec![Span::styled(state, Style::new().fg(link_color(health)))];
    if let Some(d) = device
        && s.signal_shown(d)
    {
        if let Some(dbm) = d.signal {
            link.extend([
                Span::raw(SEPARATOR),
                Span::styled(format!("{dbm} dBm"), Style::new().fg(signal_color(dbm))),
            ]);
        }
        link.push(Span::raw(format!(
            "{SEPARATOR}{}",
            last_packet(d.last_packet_ms)
        )));
    }
    let mut rows = vec![status_row("link", link)];
    if let Some(p) = device.and_then(|d| d.pairing) {
        rows.push(status_row(
            "security",
            [Span::styled(pairing(p), Style::new().fg(pairing_color(p)))],
        ));
    }
    if let Some(d) = device.filter(|d| s.connected(d.slot)) {
        rows.push(status_row("latency", vec![Span::raw(latency(d.latency))]));
    }
    rows
}

fn draw_history(
    f: &mut Frame,
    area: Rect,
    history: Option<&History>,
    slot: Option<&SlotHistory>,
) -> usize {
    let protocol = history.map(|h| h.status.protocol());
    let lines = signal_lines(protocol);
    let title = legend(
        "signal dBm",
        "━ ",
        lines.iter().rev().map(|&(name, color, _)| (name, color)),
    );
    let kept: Vec<&Strip> = strips(protocol).collect();
    let mut block = Block::bordered().title(padded(title));
    if kept.iter().any(|s| matches!(s.source, Source::Busy)) {
        block = block.title_bottom(padded(format!(
            "busy: readings at {BUSY_DBM} dBm or louder"
        )));
    }
    let inner = block.inner(area);
    f.render_widget(block, area);
    let [gutter, axis_line, plot] = Layout::horizontal([
        Constraint::Length(GUTTER),
        Constraint::Length(1),
        Constraint::Fill(1),
    ])
    .areas(inner);
    let rows = Layout::vertical(panel_rows(kept.len()));
    let [dbm_labels, strip_labels, _] = rows.areas(gutter);
    let [chart_axis, _, _] = rows.areas(axis_line);
    let [chart, strips, axis] = rows.areas(plot);

    let width = usize::from(plot.width);
    let (shown, boot): (&[(u32, Minute)], _) = match (history, slot) {
        (Some(history), Some(slot)) => {
            let minutes = &slot.minutes;
            let end = minutes.len().saturating_sub(history.scrolled(width));
            (&minutes[end.saturating_sub(width)..end], Some(history.boot))
        }
        _ => (&[], None),
    };

    let runs: Vec<_> = lines
        .iter()
        .map(|(_, color, pick)| (*color, dbm_runs(shown, *pick)))
        .collect();
    let datasets: Vec<Dataset> = runs
        .iter()
        .flat_map(|(color, runs)| {
            runs.iter().map(|run| {
                Dataset::default()
                    .marker(DBM_MARKER)
                    .graph_type(GraphType::Line)
                    .style(Style::default().fg(*color))
                    .data(run)
            })
        })
        .collect();
    f.render_widget(
        Chart::new(datasets)
            .x_axis(Axis::default().bounds([0.0, width.saturating_sub(1).max(1) as f64]))
            .y_axis(Axis::default().bounds(DBM_BOUNDS)),
        chart,
    );
    f.render_widget(Block::new().borders(Borders::LEFT), chart_axis);
    let mut label_rows = vec![Line::default(); usize::from(dbm_labels.height)];
    for dbm in DBM_LABELS {
        if let Some(row) = dbm_row(dbm, dbm_labels.height).and_then(|r| label_rows.get_mut(r)) {
            *row = Line::from(dbm.to_string());
        }
    }
    f.render_widget(Paragraph::new(label_rows).right_aligned(), dbm_labels);

    let mut names = Vec::new();
    let mut scales = Vec::new();
    let mut bars = Vec::new();
    for strip in kept {
        let values: Vec<u16> = shown.iter().map(|(_, m)| strip.value(m)).collect();
        let (top, bar) = bar_strip(&values);
        names.push(Line::from(strip.name));
        scales.push(match top {
            0 => Line::default(),
            _ => Line::from(strip.scale(top)),
        });
        bars.push(Line::from(bar));
    }
    f.render_widget(Paragraph::new(names), strip_labels);
    f.render_widget(Paragraph::new(scales).right_aligned(), strip_labels);
    f.render_widget(Paragraph::new(bars), strips);

    if let Some(boot) = boot {
        f.render_widget(Paragraph::new(time_axis(boot, shown)), axis);
    }
    width
}

fn draw_noise(f: &mut Frame, area: Rect, noise: &[NoiseChannel], s: &Status) {
    let reachable = s.keyboard_channels();
    let bars: Vec<Bar> = noise
        .iter()
        .map(|c| {
            let mut style = Style::new().fg(role_color(channel_role(c.channel, s)));
            if !reachable.contains(&c.channel) {
                style = style.add_modifier(Modifier::DIM);
            }
            Bar::default()
                .value(u64::from(noise_level(c.loudest)))
                .style(style)
                .text_value(String::new())
        })
        .collect();
    let [chart, ruler] = Layout::vertical([Constraint::Min(3), Constraint::Length(1)]).areas(area);
    let block = Block::bordered()
        .title(padded(legend(
            "loudest per channel",
            "━ ",
            [ChannelRole::Current, ChannelRole::Paired]
                .into_iter()
                .filter_map(|role| role.label().map(|name| (name, role_color(role)))),
        )))
        .title_bottom(padded(Line::from_iter([
            Span::raw("▲ BLE advertising  ├─┤ Wi-Fi 1/6/11  "),
            Span::styled(
                "dim: no move reaches",
                Style::new().add_modifier(Modifier::DIM),
            ),
        ])));
    let bars_area = block.inner(chart);
    f.render_widget(
        BarChart::default()
            .block(block)
            .bar_width(1)
            .bar_gap(0)
            .data(BarGroup::default().bars(&bars)),
        chart,
    );
    let first = noise.first().map_or(0, |c| c.channel);
    let count = u8::try_from(noise.len()).unwrap_or(u8::MAX);
    let under_bars = Rect::new(bars_area.x, ruler.y, bars_area.width, 1);
    f.render_widget(Paragraph::new(noise_ruler(first, count)), under_bars);
}

/// Splits `room` cells among the buckets by their share of the readings, giving each non-empty
/// bucket at least one cell; a row too narrow for that keeps the loudest.
fn band_cells(buckets: &[u32; NOISE_BUCKETS], room: usize) -> [usize; NOISE_BUCKETS] {
    let mut cells = [0; NOISE_BUCKETS];
    let present: Vec<usize> = (0..NOISE_BUCKETS).filter(|&b| buckets[b] > 0).collect();
    if room <= present.len() {
        for &b in present.iter().rev().take(room) {
            cells[b] = 1;
        }
        return cells;
    }
    let spare = (room - present.len()) as u64;
    let total: u64 = buckets.iter().map(|&b| u64::from(b)).sum();
    let mut remainders = Vec::new();
    for &b in &present {
        let scaled = spare * u64::from(buckets[b]);
        cells[b] = 1 + (scaled / total) as usize;
        remainders.push((scaled % total, b));
    }
    remainders.sort_by_key(|x| std::cmp::Reverse(x.0));
    let left = room - cells.iter().sum::<usize>();
    for &(_, b) in remainders.iter().take(left) {
        cells[b] += 1;
    }
    cells
}

const BAND_COLORS: [Color; NOISE_BUCKETS] = [
    Color::DarkGray,
    Color::Blue,
    Color::Green,
    Color::Yellow,
    Color::Indexed(208),
    Color::Red,
];

fn band_legend() -> Line<'static> {
    let mut line = legend(
        "noise by level",
        "█ ",
        (0..).map(bucket_label).zip(BAND_COLORS),
    );
    line.push_span(Span::styled(
        "  gray: few readings",
        Style::default().fg(Color::DarkGray),
    ));
    line
}

fn draw_moves(f: &mut Frame, area: Rect, w: &Watch, s: &Status) {
    let block = Block::bordered()
        .title(padded(band_legend()))
        .title_bottom(padded(watch_summary(w)));
    let width = usize::from(block.inner(area).width);
    let lines: Vec<Line> = w
        .channels
        .iter()
        .map(|c| {
            let color = if w.too_few(c) {
                Color::DarkGray
            } else {
                role_color(watch_role(c.channel, w, s))
            };
            let label = Span::styled(
                format!("{:>3}  {:>7}  ", c.channel, counted_share(w.counted(c))),
                Style::default().fg(color),
            );
            let room = width.saturating_sub(label.width());
            let bands = band_cells(&c.buckets, room)
                .into_iter()
                .zip(BAND_COLORS)
                .map(|(cells, color)| Span::styled("█".repeat(cells), Style::default().fg(color)));
            Line::from_iter([label].into_iter().chain(bands))
        })
        .collect();
    f.render_widget(Paragraph::new(lines).block(block), area);
}

fn draw_devices(f: &mut Frame, area: Rect, history: &History) {
    let block = Block::bordered().title(padded("stored devices; a device's number forgets it"));
    let lines: Vec<Line> = device_lines(&history.devices, history.status.protocol())
        .into_iter()
        .map(|l| {
            Line::from_iter(l.parts().into_iter().map(|(text, p)| match p {
                Some(p) => Span::styled(text, Style::new().fg(pairing_color(p))),
                None => Span::raw(text),
            }))
        })
        .collect();
    f.render_widget(Paragraph::new(lines).block(block), area);
}

fn draw_events(f: &mut Frame, area: Rect, history: Option<&History>) -> usize {
    let rows = usize::from(area.height.saturating_sub(2));
    let Some(history) = history else {
        f.render_widget(Block::bordered().title(padded("events")), area);
        return rows;
    };
    let events = &history.events;
    let shown = events_window(events, history.events_top, rows);
    let title = if shown.end == events.len() {
        "events".to_string()
    } else {
        format!(
            "events, {}–{} of {} (Home for newest)",
            events.len() - shown.end + 1,
            events.len() - shown.start,
            events.len()
        )
    };
    let items: Vec<ListItem> = events[shown]
        .iter()
        .rev()
        .map(|(_, e)| {
            let at = history.boot + TimeDelta::milliseconds(e.time_ms as i64);
            ListItem::new(format!(
                "{}  {}",
                at.format(CLOCK),
                describe_event(e, &history.status, &history.devices)
            ))
        })
        .collect();
    f.render_widget(
        List::new(items).block(Block::bordered().title(padded(title))),
        area,
    );
    rows
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::device::tests::{Fake, error_part, parts};
    use crate::format::pairing_open;
    use crate::protocol::REPORT_LEN;
    use crate::protocol::tests::{mouse_status, nuphy, status_of, status_running};
    use crate::protocol::{
        BluetoothLink, Command, ErrorCode, EventKind, Link, Noise, Pairing, Protocol, Rssi,
        SlotMinute,
    };
    use chrono::TimeZone;
    use ratatui::{Terminal, backend::TestBackend};

    fn events(indexes: Range<u32>) -> Vec<(u32, Event)> {
        let event = Event {
            time_ms: 0,
            kind: EventKind::LinkLost,
            slot: Some(0),
            a: 0,
            b: 0,
            c: 0,
            value: 0,
            noise: None,
        };
        indexes.map(|i| (i, event)).collect()
    }

    #[test]
    fn the_events_list_follows_the_newest() {
        let held = events(10..30);
        assert_eq!(events_window(&held, None, 8), 12..20);
        assert_eq!(events_window(&held[..5], None, 8), 0..5);
        assert_eq!(events_window(&[], None, 8), 0..0);
        assert_eq!(
            scrolled_top(&held[..5], None, 8, EventsMove::Older(1)),
            None
        );
    }

    #[test]
    fn scrolling_holds_the_shown_events_as_new_ones_arrive() {
        let held = events(10..30);
        let top = scrolled_top(&held, None, 8, EventsMove::Older(3));
        assert_eq!(top, Some(26));
        assert_eq!(events_window(&held, top, 8), 9..17);
        let more = events(10..35);
        assert_eq!(events_window(&more, top, 8), 9..17);
    }

    #[test]
    fn scrolling_stops_at_the_oldest_page() {
        let held = events(10..30);
        let top = scrolled_top(&held, None, 8, EventsMove::Older(100));
        assert_eq!(top, Some(17));
        assert_eq!(scrolled_top(&held, top, 8, EventsMove::Older(1)), Some(17));
        assert_eq!(scrolled_top(&held, None, 8, EventsMove::Oldest), Some(17));
    }

    #[test]
    fn scrolling_back_to_the_newest_follows_it_again() {
        let held = events(10..30);
        let top = scrolled_top(&held, None, 8, EventsMove::Older(3));
        assert_eq!(scrolled_top(&held, top, 8, EventsMove::Newer(2)), Some(28));
        assert_eq!(scrolled_top(&held, top, 8, EventsMove::Newer(3)), None);
        assert_eq!(scrolled_top(&held, top, 8, EventsMove::Newer(50)), None);
        assert_eq!(scrolled_top(&held, top, 8, EventsMove::Newest), None);
    }

    #[test]
    fn a_dropped_top_event_shows_the_oldest_page() {
        let held = events(10..30);
        assert_eq!(events_window(&held, Some(4), 8), 0..8);
    }

    #[test]
    fn a_bar_strip_scales_to_its_largest_value_and_shows_every_nonzero_one() {
        assert_eq!(bar_strip(&[0, 1, 12, 6, 0]), (12, " ▁█▄ ".to_string()));
        assert_eq!(bar_strip(&[0, 0]), (0, "  ".to_string()));
        assert_eq!(bar_strip(&[]), (0, String::new()));
    }

    fn quiet() -> Minute {
        Minute {
            invalid: 0,
            dropped: 0,
            channel_changes: 0,
            usb_retries: 0,
            noise: None,
            device: SlotMinute {
                packets: 1,
                gaps: Some(0),
                link_losses: Some(0),
                rssi: None,
            },
        }
    }

    fn with_signal(dbm: i8) -> Minute {
        let mut m = quiet();
        m.device.rssi = Some(Rssi {
            min: dbm,
            avg: dbm,
            max: dbm,
        });
        m
    }

    fn boot() -> DateTime<Local> {
        Local.with_ymd_and_hms(2026, 11, 12, 10, 0, 5).unwrap()
    }

    #[test]
    fn the_time_axis_labels_the_first_middle_and_last_minute() {
        let shown: Vec<_> = (0..40).map(|i| (i, quiet())).collect();
        assert_eq!(
            time_axis(boot(), &shown),
            "╵10:00:05           ╵10:20:05  10:39:05╵"
        );
    }

    #[test]
    fn the_time_axis_leaves_out_labels_that_would_touch() {
        let shown: Vec<_> = (0..20).map(|i| (i, quiet())).collect();
        assert_eq!(time_axis(boot(), &shown), "╵10:00:05  10:19:05╵");
        assert_eq!(time_axis(boot(), &shown[..3]), "╵10:00:05");
        assert_eq!(time_axis(boot(), &[]), "");
    }

    #[test]
    fn every_counter_strip_plots_a_counter_a_minute_holds() {
        for strip in &STRIPS {
            if let Source::Counter(counter) = strip.source {
                let m = quiet();
                assert!(
                    m.get(counter).or(m.device.get(counter)).is_some(),
                    "{}",
                    strip.name
                );
            }
        }
    }

    fn panel_with(marked: Minute, height: u16) -> Vec<String> {
        panel_in(Some(Protocol::Nuphy), marked, height)
    }

    fn silent() -> Device<Fake> {
        Device::new(Fake::new(|req| parts(req[0], req[1], &[])), 0)
    }

    fn app_with(status: Status) -> App<Fake> {
        app_on(silent(), status)
    }

    fn app_on<T: Transport>(device: Device<T>, status: Status) -> App<T> {
        let mut session = Session::new(device, status);
        session.history.boot = boot();
        let mut app = App::new(None);
        app.connection = Connection::Connected(session);
        app
    }

    fn session<T: Transport>(app: &mut App<T>) -> &mut Session<T> {
        app.session_mut().expect("a connected app")
    }

    fn press<T: Transport>(app: &mut App<T>, c: char) {
        app.handle(KeyCode::Char(c));
    }

    fn history<T: Transport>(app: &mut App<T>) -> &mut History {
        &mut session(app).history
    }

    fn panel_in(protocol: Option<Protocol>, marked: Minute, height: u16) -> Vec<String> {
        let mut app = match protocol {
            Some(protocol) => {
                let mut app = app_with(status_of(protocol));
                let h = history(&mut app);
                h.slots[0].minutes = (0..10).map(|i| (i, quiet())).collect();
                h.slots[0].minutes[4].1 = marked;
                app
            }
            None => App::new(None),
        };
        let mut terminal = Terminal::new(TestBackend::new(60, height)).unwrap();
        terminal
            .draw(|f| {
                let history = app.session_mut().map(|s| &s.history);
                draw_history(f, f.area(), history, history.and_then(|h| h.slots.first()));
            })
            .unwrap();
        screen(&terminal)
    }

    fn screen(terminal: &Terminal<TestBackend>) -> Vec<String> {
        let buffer = terminal.backend().buffer();
        (0..buffer.area.height)
            .map(|y| {
                (0..buffer.area.width)
                    .map(|x| buffer[(x, y)].symbol())
                    .collect()
            })
            .collect()
    }

    fn drawn<T: Transport>(app: &mut App<T>) -> Terminal<TestBackend> {
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        terminal
    }

    fn channel_fg(terminal: &Terminal<TestBackend>, channel: u8, x: u16) -> Color {
        let rows = screen(terminal);
        let y = rows
            .iter()
            .position(|r| r.starts_with(&format!("│{channel:>3}  ")))
            .unwrap();
        terminal.backend().buffer()[(x, y as u16)].fg
    }

    fn footer<T: Transport>(app: &mut App<T>) -> String {
        let rows = screen(&drawn(app));
        rows[rows.len() - app.connection.view(80).1.len()..]
            .iter()
            .map(|row| row.trim_end())
            .collect::<Vec<_>>()
            .join("\n")
    }

    const UNIFYING_HINTS: [&str; 11] = [
        "n noise",
        "m moves",
        "a pair",
        "p protocol",
        "r reboot",
        "b bootloader",
        "c clear",
        "←/→ history",
        "↑/↓ events",
        "esc back",
        "q quit",
    ];

    #[test]
    fn hints_break_only_between_hints() {
        let lines = flow(UNIFYING_HINTS, "  ", 80);
        assert_eq!(
            lines,
            [
                "n noise  m moves  a pair  p protocol  r reboot  b bootloader  c clear",
                "←/→ history  ↑/↓ events  esc back  q quit",
            ]
        );
        assert!(lines.iter().all(|l| l.chars().count() <= 80));
    }

    #[test]
    fn text_that_fits_stays_on_one_line() {
        let nuphy = [
            "n noise",
            "p protocol",
            "r reboot",
            "b bootloader",
            "c clear",
            "←/→ history",
            "↑/↓ events",
            "esc back",
            "q quit",
        ];
        assert_eq!(flow(nuphy, "  ", 100), [nuphy.join("  ")]);
    }

    #[test]
    fn a_message_breaks_between_words() {
        let message = pairing_open(Protocol::Unifying, None);
        let lines = flow(message.split(' '), " ", 80);
        assert_eq!(lines.len(), 2, "{lines:?}");
        assert!(lines.iter().all(|l| l.chars().count() <= 80), "{lines:?}");
        assert_eq!(lines.join(" "), message);
    }

    #[test]
    fn a_piece_wider_than_the_line_gets_a_line_of_its_own() {
        assert_eq!(flow(["a", "bbbbbb", "c"], " ", 4), ["a", "bbbbbb", "c"]);
    }

    #[test]
    fn the_unifying_footer_flows_onto_the_last_rows_of_an_80_column_screen() {
        let mut app = app_with(status_of(Protocol::Unifying));
        let rows = screen(&drawn(&mut app));
        assert_eq!(
            rows[22].trim_end(),
            "n noise  m moves  a pair  p protocol  r reboot  b bootloader  c clear"
        );
        assert_eq!(
            rows[23].trim_end(),
            "[/] channel  ←/→ history  ↑/↓ events  esc back  q quit"
        );
        let text = rows.join("\n");
        for part in ["SpareDongle", "signal dBm", "events"] {
            assert!(text.contains(part), "{part} missing from {text}");
        }
    }

    #[test]
    fn an_error_reply_keeps_the_dongle_and_its_history() {
        let mut app = app_on(
            Device::new(
                Fake::new(|req| vec![error_part(req[0], req[1], ErrorCode::Radio as u8)]),
                0,
            ),
            status_of(Protocol::Nuphy),
        );
        history(&mut app).slots[0].minutes = vec![(0, quiet())];
        let status = session(&mut app).device.status();
        app.refresh(status);
        assert_eq!(
            footer(&mut app),
            "the dongle refused Status: the radio failed"
        );
        assert!(app.session_mut().is_some());
        assert_eq!(history(&mut app).slots[0].minutes.len(), 1);
    }

    #[test]
    fn a_unifying_dongle_fetches_its_history() {
        let h = fetched(dongle_in(Protocol::Unifying, 1, 200 * 60_000, 130, 0));
        assert_eq!(h.status.protocol(), Protocol::Unifying);
        assert_eq!(h.slots[0].minutes.len(), 130);
    }

    #[test]
    fn a_unifying_dongle_draws_the_full_layout_and_its_keys() {
        let mut app = app_with(status_of(Protocol::Unifying));
        let mut terminal = Terminal::new(TestBackend::new(100, 24)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        let rows = screen(&terminal);
        let text = rows.join("\n");
        assert!(text.contains("events"), "{text}");
        assert!(
            !text.contains("history is kept only in NuPhy mode"),
            "{text}"
        );
        assert_eq!(
            rows[22].trim_end(),
            "n noise  m moves  a pair  p protocol  r reboot  b bootloader  c clear  [/] channel  ←/→ history"
        );
        assert_eq!(rows[23].trim_end(), "↑/↓ events  esc back  q quit");
    }

    #[test]
    fn a_terminal_too_short_for_the_unifying_layout_still_draws() {
        let mut app = app_with(status_of(Protocol::Unifying));
        let mut terminal = Terminal::new(TestBackend::new(80, 10)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        assert_eq!(screen(&terminal).len(), 10);
    }

    #[test]
    fn the_nuphy_footer_is_one_line_of_hints() {
        let mut app = app_with(status_of(Protocol::Nuphy));
        assert_eq!(session(&mut app).protocol(), Protocol::Nuphy);
        let hints = [
            "n noise",
            "m moves",
            "a pair",
            "p protocol",
            "r reboot",
            "b bootloader",
            "c clear",
            "[/] channel",
            "←/→ history",
            "↑/↓ events",
            "esc back",
            "q quit",
        ];
        assert_eq!(app.connection.view(130).1, [hints.join("  ")]);
    }

    #[test]
    fn the_bluetooth_footer_offers_only_what_the_dongle_answers() {
        let mut app = app_with(status_of(Protocol::Bluetooth));
        let hints = [
            "a pair",
            "d devices",
            "p protocol",
            "r reboot",
            "b bootloader",
            "c clear",
            "←/→ history",
            "↑/↓ events",
            "esc back",
            "q quit",
        ];
        assert_eq!(app.connection.view(200).1, [hints.join("  ")]);
        for c in ['n', 'm'] {
            press(&mut app, c);
            assert!(session(&mut app).prompt.is_none(), "{c}");
            assert!(matches!(session(&mut app).pane, Pane::History), "{c}");
        }
    }

    fn bluetooth_dongle_forgetting_into(
        sent: std::rc::Rc<std::cell::Cell<Option<[u8; 7]>>>,
    ) -> App<Fake> {
        let list = crate::protocol::tests::fixture("devices.hex");
        let mut app = app_on(
            Device::new(
                Fake::new(move |req| {
                    if req[0] == Command::Forget as u8 {
                        sent.set(Some(req[6..13].try_into().unwrap()));
                    }
                    let payload: &[u8] = if req[0] == Command::Devices as u8 {
                        &list
                    } else {
                        &[]
                    };
                    parts(req[0], req[1], payload)
                }),
                0,
            ),
            status_of(Protocol::Bluetooth),
        );
        refreshed_with_pairing(&mut app, Protocol::Bluetooth, false);
        app
    }

    #[test]
    fn the_devices_pane_lists_each_kind_and_forgets_the_device_at_the_pressed_number() {
        let sent = std::rc::Rc::new(std::cell::Cell::new(None));
        let mut app = bluetooth_dongle_forgetting_into(sent.clone());
        press(&mut app, 'k');
        assert!(matches!(session(&mut app).pane, Pane::History));
        press(&mut app, 'd');
        let terminal = drawn(&mut app);
        let text = screen(&terminal).join("\n");
        assert!(text.contains("1  keyboard  MX Keys"), "{text}");
        assert!(text.contains("2  mouse     MX Master 3S"), "{text}");
        let mouse_row = screen(&terminal)
            .iter()
            .position(|r| r.contains("2  mouse"))
            .unwrap() as u16;
        assert_eq!(
            cell_at(&terminal, mouse_row, "secure, no passkey").fg,
            Color::Yellow
        );
        assert_eq!(cell_at(&terminal, mouse_row, "C8:F2").fg, Color::Reset);
        press(&mut app, '2');
        assert!(
            footer(&mut app).starts_with("Forget MX Master 3S (C8:F2:E0:11:3C:5A)?"),
            "{}",
            footer(&mut app)
        );
        let Some(Outcome::Answer(prompt)) = app.handle(KeyCode::Char('y')) else {
            panic!("y should answer the forget question");
        };
        session(&mut app).answer(prompt);
        assert_eq!(sent.get(), Some([1, 0x5a, 0x3c, 0x11, 0xe0, 0xf2, 0xc8]));
        assert_eq!(footer(&mut app), "forgotten");
    }

    fn drawn_at<T: Transport>(app: &mut App<T>, width: u16, height: u16) -> Vec<String> {
        let mut terminal = Terminal::new(TestBackend::new(width, height)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        screen(&terminal)
    }

    fn footer_at<T: Transport>(app: &App<T>, width: u16) -> String {
        app.connection.view(usize::from(width)).1.join("\n")
    }

    /// A Bluetooth dongle with the MX Keys stored and the MX Master 3S connected.
    fn keyboard_and_mouse() -> App<Fake> {
        let mouse = mouse_status();
        let mut s = mouse.clone();
        s.device = SlotStatus {
            slot: 0,
            name: Some("MX Keys".into()),
            signal: None,
            last_packet_ms: None,
            packets: 10,
            gaps: None,
            link_losses: None,
            latency: None,
            pairing: None,
            battery: None,
        };
        let mut app = app_with(s);
        history(&mut app).slots.push(SlotHistory {
            device: mouse.device,
            minutes: Vec::new(),
        });
        app
    }

    const KEYBOARD_TITLE: &str = "┌ keyboard · MX Keys ─";
    const MOUSE_TITLE: &str = "┌ mouse · MX Master 3S ─";
    /// In Bluetooth mode, under the dongle box's dongle and firmware rows.
    const DEVICE_TITLE_ROW: usize = 4;
    /// In NuPhy and Unifying mode, where the dongle box adds a channels row.
    const CHANNELS_DEVICE_TITLE_ROW: usize = DEVICE_TITLE_ROW + 1;
    const CHANNELS_LINK_ROW: usize = CHANNELS_DEVICE_TITLE_ROW + 1;
    /// Under the device box's three rows.
    const CHART_ROW: usize = DEVICE_TITLE_ROW + 5;

    fn signal_legend(protocol: Protocol) -> String {
        legend(
            "signal dBm",
            "━ ",
            signal_lines(Some(protocol))
                .iter()
                .rev()
                .map(|&(name, color, _)| (name, color)),
        )
        .to_string()
    }

    /// The firmware row spans the view.
    const FIRMWARE_LABEL: &str = "firmware ";

    /// The narrowest view that draws its one slot's device box, the chart titles and the dongle
    /// box uncut, all but the dongle box's firmware row.
    fn narrowest_whole(mut app: App<Fake>) -> u16 {
        let h = history(&mut app);
        let device = &h.slots[0].device;
        let text = |lines: Vec<Line>| lines.iter().map(Line::to_string).collect::<Vec<_>>();
        let dongle = text(dongle_lines(Some(&h.status)));
        let device_top = dongle.len() + 2;
        let boxes = [
            (dongle_title(Some(&h.status)), dongle),
            (
                device_title(&h.status, device),
                text(slot_lines(Some(&h.status), Some(device))),
            ),
        ];
        let chart_row = device_top + usize::from(device_rows(Some(h.status.protocol()))) + 2;
        let chart_legend = signal_legend(h.status.protocol());
        (20..=200)
            .find(|&width| {
                let rows = drawn_at(&mut app, width, 25);
                boxes
                    .iter()
                    .zip([0, device_top])
                    .all(|((title, lines), top)| {
                        rows[top].contains(title.as_str())
                            && lines
                                .iter()
                                .enumerate()
                                .filter(|(_, line)| !line.starts_with(FIRMWARE_LABEL))
                                .all(|(i, line)| inside_box(&rows[top + 1 + i]) == line)
                    })
                    && rows[chart_row].contains(&chart_legend)
            })
            .unwrap()
    }

    #[test]
    fn a_slot_panel_is_as_wide_as_the_view_needs_for_one_slot() {
        let mouse = app_with(mouse_status());
        let needed = [
            app_with(status_of(Protocol::Nuphy)),
            app_with(status_of(Protocol::Unifying)),
            app_with(status_of(Protocol::Bluetooth)),
            mouse,
        ]
        .into_iter()
        .map(narrowest_whole)
        .max();
        assert_eq!(Some(PANEL_MIN_WIDTH), needed);
    }

    #[test]
    fn slots_sit_side_by_side_in_slot_order_when_the_width_fits_them() {
        let mut app = keyboard_and_mouse();
        let width = 2 * PANEL_MIN_WIDTH;
        let rows = drawn_at(&mut app, width, 30);
        assert!(
            rows[0].starts_with("┌ SpareDongle · Bluetooth ─"),
            "{rows:#?}"
        );
        assert_eq!(rows.iter().filter(|r| r.contains("│dongle ")).count(), 1);
        let keyboard = rows[DEVICE_TITLE_ROW].find(KEYBOARD_TITLE);
        let mouse = rows[DEVICE_TITLE_ROW].find(MOUSE_TITLE);
        assert!(keyboard.zip(mouse).is_some_and(|(k, m)| k < m), "{rows:#?}");
        assert_eq!(
            rows[CHART_ROW].matches("┌ signal dBm").count(),
            2,
            "{rows:#?}"
        );
        assert_eq!(rows.iter().filter(|r| r.contains("┌ events")).count(), 1);
        assert!(!footer_at(&app, width).contains("o device"));
    }

    #[test]
    fn each_slot_panel_shows_its_own_device_state_and_signal() {
        let mut app = keyboard_and_mouse();
        let rows = drawn_at(&mut app, 2 * PANEL_MIN_WIDTH, 30);
        let link = &rows[DEVICE_TITLE_ROW + 1];
        let (keyboard, mouse) = link.split_at(link.find("││").unwrap() + 3);
        assert_eq!(inside_box(keyboard), "link      not connected");
        assert_eq!(
            inside_box(mouse),
            "link      connected   -60 dBm   last packet 250 ms ago"
        );
        let security = &rows[DEVICE_TITLE_ROW + 2];
        let (keyboard, mouse) = security.split_at(security.find("││").unwrap() + 3);
        assert_eq!(inside_box(keyboard), "");
        assert_eq!(inside_box(mouse), "security  secure, no passkey");
    }

    #[test]
    fn the_pairing_is_colored_by_how_well_it_protects_the_link() {
        let colored = |secure, passkey| {
            let mut s = mouse_status();
            s.device.pairing = Some(Pairing { secure, passkey });
            let words = pairing(Pairing { secure, passkey });
            cell_at(&status_box(&s), DEVICE_TITLE_ROW as u16 + 2, &words).fg
        };
        assert_eq!(colored(true, true), Color::Green);
        assert_eq!(colored(true, false), Color::Yellow);
        assert_eq!(colored(false, true), Color::Yellow);
        assert_eq!(colored(false, false), Color::Red);
    }

    #[test]
    fn a_view_too_narrow_for_every_slot_shows_one_and_o_cycles_through_them() {
        let mut app = keyboard_and_mouse();
        let width = 2 * PANEL_MIN_WIDTH - 1;
        let rows = drawn_at(&mut app, width, 30);
        assert!(rows[DEVICE_TITLE_ROW].contains(KEYBOARD_TITLE), "{rows:#?}");
        assert!(!rows[DEVICE_TITLE_ROW].contains(MOUSE_TITLE), "{rows:#?}");
        assert!(rows[CHART_ROW].starts_with("┌ signal dBm"), "{rows:#?}");
        assert!(footer_at(&app, width).contains("o device"));
        press(&mut app, 'o');
        let rows = drawn_at(&mut app, width, 30);
        assert!(rows[DEVICE_TITLE_ROW].contains(MOUSE_TITLE), "{rows:#?}");
        assert!(rows[CHART_ROW].starts_with("┌ signal dBm"), "{rows:#?}");
        press(&mut app, 'o');
        let rows = drawn_at(&mut app, width, 30);
        assert!(rows[DEVICE_TITLE_ROW].contains(KEYBOARD_TITLE), "{rows:#?}");
    }

    #[test]
    fn a_view_of_one_slot_has_no_o() {
        let mut app = app_with(status_of(Protocol::Bluetooth));
        let rows = drawn_at(&mut app, 80, 24);
        assert!(!footer_at(&app, 80).contains("o device"));
        assert!(rows[CHART_ROW].starts_with("┌ signal dBm"), "{rows:#?}");
        press(&mut app, 'o');
        assert_eq!(drawn_at(&mut app, 80, 24), rows);
    }

    /// A Bluetooth dongle answering for whichever slot is asked, with `stored` its slot table and
    /// each slot's minutes holding `7 + 2 * slot` packets.
    fn bluetooth_dongle(stored: [bool; 2]) -> Device<Fake> {
        Device::new(
            Fake::new(move |req| {
                let payload = match req[0] {
                    1 => {
                        let mut p = crate::protocol::tests::fixture("status-slot1.hex");
                        let slot_at = p.len() - crate::protocol::tests::SLOT_FROM_END;
                        p[slot_at] = req[6];
                        p[slot_at - 3] = u8::from(stored[0]);
                        p[slot_at - 1] = u8::from(stored[1]);
                        p
                    }
                    2 => {
                        let from = u32::from_le_bytes(req[2..6].try_into().unwrap()).max(60);
                        let n = 1500u32.saturating_sub(from).min(40) as u8;
                        let mut p = from.to_le_bytes().to_vec();
                        p.push(n);
                        for _ in 0..n {
                            p.extend([0, 0, 0, 0, 0, 0, 0, 0, 0, 7 + 2 * req[6], 0, 0, 0, 0, 0, 0]);
                        }
                        p
                    }
                    13 => vec![0],
                    _ => vec![0, 0, 0, 0, 0],
                };
                parts(req[0], req[1], &payload)
            }),
            0,
        )
    }

    fn slots_fetched(stored: [bool; 2]) -> Vec<(u8, Option<u16>)> {
        fetched(bluetooth_dongle(stored))
            .slots
            .iter()
            .map(|h| {
                let packets = h.minutes.first().map(|(_, m)| m.device.packets);
                (h.device.slot, packets)
            })
            .collect()
    }

    #[test]
    fn a_refresh_fetches_each_shown_slots_status_and_minutes() {
        assert_eq!(slots_fetched([true, true]), [(0, Some(7)), (1, Some(9))]);
        assert_eq!(slots_fetched([false, true]), [(1, Some(9))]);
        assert_eq!(slots_fetched([true, false]), [(0, Some(7))]);
    }

    #[test]
    fn the_events_list_names_a_stored_bluetooth_keyboard() {
        let mut app = bluetooth_dongle_forgetting_into(Default::default());
        let connected = Event {
            time_ms: 0,
            kind: EventKind::BluetoothConnected,
            slot: Some(0),
            a: 0xeb,
            b: 0xbb,
            c: 0,
            value: 0x49a6_9ad9,
            noise: None,
        };
        history(&mut app).events = vec![(0, connected)];
        let text = screen(&drawn(&mut app)).join("\n");
        assert!(text.contains("keyboard MX Keys connected"), "{text}");
    }

    #[test]
    fn the_protocol_key_asks_which_of_the_other_two() {
        let mut app = app_with(status_of(Protocol::Unifying));
        let s = session(&mut app);
        assert!(matches!(s.key('p'), Some(Key::Choose(Choice::Protocol))));
        s.handle(KeyCode::Char('p'), Drawn::default());
        assert_eq!(
            s.help(200),
            ["Switch to which protocol? 1 NuPhy  2 Bluetooth  (any other key cancels)"]
        );
        assert!(s.handle(KeyCode::Char('2'), Drawn::default()).is_none());
        assert!(matches!(
            s.prompt,
            Some(Prompt::Switch(Protocol::Bluetooth))
        ));
        let outcome = s.handle(KeyCode::Char('y'), Drawn::default());
        assert!(matches!(
            outcome,
            Some(Outcome::Answer(Prompt::Switch(Protocol::Bluetooth)))
        ));
    }

    #[test]
    fn any_other_key_cancels_the_protocol_choice() {
        let mut app = app_with(status_of(Protocol::Nuphy));
        let s = session(&mut app);
        s.handle(KeyCode::Char('p'), Drawn::default());
        assert!(s.handle(KeyCode::Char('3'), Drawn::default()).is_none());
        assert!(s.prompt.is_none() && s.choosing.is_none());
    }

    #[test]
    fn every_mode_offers_pairing_and_noise() {
        let mut app = app_with(status_of(Protocol::Unifying));
        assert!(matches!(session(&mut app).key('a'), Some(Key::Pair)));
        assert!(matches!(
            session(&mut app).key('n'),
            Some(Key::Ask(Prompt::Noise))
        ));
    }

    #[test]
    fn switching_asks_the_dongle_for_the_other_protocol() {
        let sent = std::rc::Rc::new(std::cell::Cell::new(None));
        let seen = sent.clone();
        let mut app = app_on(
            Device::new(
                Fake::new(move |req| {
                    seen.set(Some((
                        req[0],
                        u32::from_le_bytes(req[2..6].try_into().unwrap()),
                    )));
                    parts(req[0], req[1], &[2, 1])
                }),
                0,
            ),
            status_of(Protocol::Nuphy),
        );
        session(&mut app).answer(Prompt::Switch(Protocol::Unifying));
        assert_eq!(sent.get(), Some((Command::Protocol as u8, 2)));
        assert_eq!(
            footer(&mut app),
            "switching to Unifying; the dongle reboots"
        );
    }

    #[test]
    fn the_pair_key_opens_pairing_without_asking() {
        let sent = std::rc::Rc::new(std::cell::Cell::new(None));
        let seen = sent.clone();
        let mut app = app_on(
            Device::new(
                Fake::new(move |req| {
                    seen.set(Some(req[0]));
                    parts(req[0], req[1], &[])
                }),
                0,
            ),
            status_of(Protocol::Unifying),
        );
        press(&mut app, 'a');
        assert!(session(&mut app).prompt.is_none());
        assert_eq!(sent.get(), Some(Command::Pair as u8));
        assert_eq!(
            footer(&mut app),
            "pairing is open for 30 s; switch the keyboard off and on, after holding an\nEasy-Switch key until it blinks fast if it has them"
        );
    }

    #[test]
    fn the_bluetooth_pair_key_asks_whether_the_keyboard_types_a_passkey() {
        let sent = std::rc::Rc::new(std::cell::Cell::new(None));
        let seen = sent.clone();
        let mut app = app_on(
            Device::new(
                Fake::new(move |req| {
                    seen.set(Some((req[0], req[2])));
                    parts(req[0], req[1], &[0])
                }),
                0,
            ),
            status_of(Protocol::Bluetooth),
        );
        press(&mut app, 'a');
        assert_eq!(sent.get(), None);
        assert_eq!(
            footer(&mut app),
            "Pair how? 1 with a passkey  2 without a passkey  (any other key cancels)"
        );
        press(&mut app, '2');
        assert_eq!(sent.get(), Some((Command::Pair as u8, 1)));
        assert_eq!(
            footer(&mut app),
            "pairing is open for 30 s; put the keyboard or mouse in Bluetooth pairing mode"
        );
        press(&mut app, 'a');
        press(&mut app, 'x');
        assert!(footer(&mut app).starts_with("a pair"));
        assert_eq!(sent.get(), Some((Command::Pair as u8, 1)));
    }

    fn app_moving_from(status: Status) -> (App<Fake>, std::rc::Rc<std::cell::Cell<Option<u32>>>) {
        let sent = std::rc::Rc::new(std::cell::Cell::new(None));
        let seen = sent.clone();
        let from = status.channel;
        let app = app_on(
            Device::new(
                Fake::new(move |req| {
                    assert_eq!(req[0], Command::Move as u8);
                    seen.set(Some(u32::from_le_bytes(req[2..6].try_into().unwrap())));
                    parts(req[0], req[1], &[from])
                }),
                0,
            ),
            status,
        );
        (app, sent)
    }

    #[test]
    fn the_bracket_keys_move_to_the_neighbouring_channel() {
        let (mut app, sent) = app_moving_from(status_of(Protocol::Unifying));
        press(&mut app, ']');
        assert_eq!(sent.get(), Some(65));
        assert_eq!(
            footer(&mut app),
            "moved from channel 62 to channel 65; the keyboard follows on its next send"
        );
        assert_eq!(history(&mut app).status.channel, 65);
        let mut status = status_of(Protocol::Nuphy);
        status.channel = 16;
        let (mut app, sent) = app_moving_from(status);
        press(&mut app, '[');
        assert_eq!(sent.get(), Some(78));
    }

    #[test]
    fn a_refused_move_shows_why() {
        let mut app = app_on(
            Device::new(
                Fake::new(|req| vec![error_part(req[0], req[1], ErrorCode::NotMovable as u8)]),
                0,
            ),
            status_of(Protocol::Unifying),
        );
        press(&mut app, ']');
        assert!(footer(&mut app).starts_with(
            "failed: the dongle refused Move: it moves the link only with a keyboard paired"
        ));
    }

    #[test]
    fn the_dongle_box_ends_with_the_firmware_bootloader_and_softdevice() {
        let rows = dongle_lines(Some(&status_of(Protocol::Bluetooth)));
        assert_eq!(
            rows.last().map(|l| l.to_string()).as_deref(),
            Some("firmware  1.2.3   bootloader 0.6.0   SoftDevice S140 6.1.1")
        );
    }

    #[test]
    fn the_status_shows_pairing_open_in_either_mode() {
        let dongle_row = |s: &Status| dongle_lines(Some(s))[0].to_string();
        assert!(dongle_row(&status_of(Protocol::Unifying)).ends_with("   pairing open"));
        let mut s = status_of(Protocol::Nuphy);
        assert!(dongle_row(&s).ends_with("   pairing open"));
        s.pairing_open = false;
        assert!(!dongle_row(&s).contains("pairing"));
    }

    #[test]
    fn the_pair_key_in_nuphy_mode_says_to_hold_fn_4() {
        let mut app = app_showing("");
        press(&mut app, 'a');
        assert_eq!(
            footer(&mut app),
            "pairing is open for 30 s; hold Fn+4 on the keyboard for 3 s"
        );
    }

    #[test]
    fn the_pairing_message_goes_with_a_closed_status_even_when_the_fetch_fails() {
        let mut closed = status_payload(Protocol::Nuphy, 60_000, 1, 0);
        closed[crate::protocol::tests::PROTOCOL_AT + 1] = 0;
        let device = Device::new(
            Fake::new(move |req| match req[0] {
                1 => parts(req[0], req[1], &closed),
                _ => vec![error_part(req[0], req[1], ErrorCode::Radio as u8)],
            }),
            0,
        );
        let mut app = app_on(device, status_of(Protocol::Nuphy));
        session(&mut app).message = pairing_open(Protocol::Nuphy, None);
        let status = session(&mut app).device.status();
        app.refresh(status);
        assert_eq!(session(&mut app).message, "");
    }

    fn refreshed_with_pairing(app: &mut App<Fake>, protocol: Protocol, open: bool) {
        let mut s = status_of(protocol);
        s.pairing_open = open;
        s.minute_next = s.minute_oldest;
        s.events_total = s.events_oldest;
        app.refresh(Ok(s));
    }

    fn app_showing(message: &str) -> App<Fake> {
        let mut app = app_with(status_of(Protocol::Nuphy));
        session(&mut app).message = message.into();
        app
    }

    #[test]
    fn the_pairing_message_stays_while_the_status_reads_pairing() {
        for protocol in Protocol::ALL {
            let mut app = app_showing(&pairing_open(protocol, None));
            refreshed_with_pairing(&mut app, protocol, true);
            assert_eq!(session(&mut app).message, pairing_open(protocol, None));
        }
    }

    #[test]
    fn the_pairing_message_goes_once_the_status_no_longer_reads_pairing() {
        let mut app = app_showing(&pairing_open(Protocol::Unifying, None));
        refreshed_with_pairing(&mut app, Protocol::Unifying, false);
        assert_eq!(session(&mut app).message, "");
        assert_eq!(
            footer(&mut app),
            "n noise  m moves  a pair  p protocol  r reboot  b bootloader  c clear\n[/] channel  ←/→ history  ↑/↓ events  esc back  q quit"
        );
    }

    #[test]
    fn another_message_stays_when_the_status_no_longer_reads_pairing() {
        let mut app = app_showing("done");
        refreshed_with_pairing(&mut app, Protocol::Unifying, false);
        assert_eq!(session(&mut app).message, "done");
    }

    struct Unplugged;

    impl Transport for Unplugged {
        fn write(&mut self, _: &[u8; REPORT_LEN]) -> Result<(), Lost> {
            Err(Lost {
                doing: "writing to the dongle",
                cause: hidapi::HidError::HidApiError {
                    message: "device disconnected".into(),
                },
            })
        }

        fn read(&mut self, _: &mut [u8; REPORT_LEN], _: Duration) -> Result<bool, Lost> {
            Ok(false)
        }
    }

    #[test]
    fn a_lost_dongle_disconnects() {
        let mut app = app_on(Device::new(Unplugged, 0), status_of(Protocol::Nuphy));
        let status = session(&mut app).device.status();
        app.refresh(status);
        assert_eq!(
            footer(&mut app),
            "disconnected: writing to the dongle: hidapi error: device disconnected\nq quit"
        );
        assert!(app.session_mut().is_none());
    }

    #[test]
    fn a_failed_action_shows_its_cause() {
        let mut app = app_on(Device::new(Unplugged, 0), status_of(Protocol::Nuphy));
        session(&mut app).answer(Prompt::Action(Action::Reboot));
        assert_eq!(
            footer(&mut app),
            "failed: writing to the dongle: hidapi error: device disconnected"
        );
    }

    const NOISE_FIRST: u8 = 2;
    const NOISE_ROWS: u16 = 12;

    fn noise_map() -> Terminal<TestBackend> {
        let status = status_of(Protocol::Nuphy);
        let noise: Vec<NoiseChannel> = (NOISE_FIRST..=80)
            .map(|channel| NoiseChannel {
                channel,
                loudest: -40,
                average: -60,
            })
            .collect();
        let mut terminal = Terminal::new(TestBackend::new(80, NOISE_ROWS)).unwrap();
        terminal
            .draw(|f| draw_noise(f, f.area(), &noise, &status))
            .unwrap();
        terminal
    }

    #[test]
    fn the_noise_legend_fits_an_80_column_terminal() {
        let terminal = noise_map();
        let rows = screen(&terminal);
        let text = format!("{}{}", rows[0], rows[10]);
        for key in [
            "loudest per channel",
            "━ current",
            "━ paired",
            "▲ BLE advertising",
            "├─┤ Wi-Fi 1/6/11",
            "dim: no move reaches",
        ] {
            assert!(text.contains(key), "{key} missing from {text}");
        }
    }

    #[test]
    fn the_noise_map_dims_the_channels_no_move_can_reach() {
        let terminal = noise_map();
        let bottom_bar_row = NOISE_ROWS - 3;
        let dimmed = |channel: u8| {
            terminal.backend().buffer()[(1 + u16::from(channel - NOISE_FIRST), bottom_bar_row)]
                .modifier
                .contains(Modifier::DIM)
        };
        assert!(dimmed(17));
        assert!(!dimmed(16));
    }

    fn plotted(rows: &[String]) -> Vec<(usize, usize)> {
        rows.iter()
            .enumerate()
            .flat_map(|(y, r)| {
                r.chars()
                    .enumerate()
                    .filter(|(_, c)| ('\u{2801}'..='\u{28ff}').contains(c))
                    .map(move |(x, _)| (x, y))
            })
            .collect()
    }

    #[test]
    fn a_minute_sits_in_one_column_across_the_chart_and_the_strips() {
        let mut marked = with_signal(-60);
        marked.device.gaps = Some(3);
        let rows = panel_with(marked, history_min_rows(Some(Protocol::Nuphy)));
        let gaps = rows
            .iter()
            .find(|r| r.starts_with("│gaps"))
            .expect("a gaps strip");
        let column = gaps.chars().position(|c| ('▁'..='█').contains(&c)).unwrap();
        let plotted = plotted(&rows);
        assert!(!plotted.is_empty());
        assert!(plotted.iter().all(|(x, _)| *x == column), "{rows:#?}");
    }

    #[test]
    fn a_reading_sits_on_its_labeled_row_or_on_the_edge_past_the_chart() {
        for (dbm, label) in [(-60, "-60"), (-15, "-20"), (-110, "-100")] {
            for height in
                history_min_rows(Some(Protocol::Nuphy))..history_min_rows(Some(Protocol::Nuphy)) + 6
            {
                let rows = panel_with(with_signal(dbm), height);
                let labeled = rows.iter().position(|r| r.contains(label)).unwrap();
                let plotted = plotted(&rows);
                assert!(!plotted.is_empty(), "{rows:#?}");
                assert!(plotted.iter().all(|(_, y)| *y == labeled), "{rows:#?}");
            }
        }
    }

    #[test]
    fn a_full_busy_share_keeps_apart_from_its_name() {
        let marked = Minute {
            noise: Some(Noise {
                peak: -40,
                busy: 10_000,
                strong: 10_000,
            }),
            ..quiet()
        };
        let rows = panel_with(marked, history_min_rows(Some(Protocol::Nuphy)));
        assert!(
            rows.iter().any(|r| r.starts_with("│busy 100.00% ")),
            "{rows:#?}"
        );
    }

    #[test]
    fn minutes_without_packets_split_the_signal_into_runs() {
        let mut without = quiet();
        without.device.packets = 0;
        let shown = [
            (10, with_signal(-60)),
            (11, with_signal(-61)),
            (12, without),
            (13, with_signal(-70)),
        ];
        assert_eq!(
            dbm_runs(&shown, |m| m.device.rssi.map(|r| r.avg)),
            vec![vec![(0.0, -60.0), (1.0, -61.0)], vec![(3.0, -70.0)]]
        );
    }

    fn status_payload(
        protocol: Protocol,
        uptime_ms: u64,
        minute_next: u32,
        minute_oldest: u32,
    ) -> Vec<u8> {
        let mut p = status_running(protocol as u8);
        let u = crate::protocol::tests::UPTIME_AT;
        p[u..u + 8].copy_from_slice(&uptime_ms.to_le_bytes());
        let n = crate::protocol::tests::PROTOCOL_AT;
        p[n - 16..n - 12].copy_from_slice(&minute_next.to_le_bytes());
        p[n - 12..n - 8].copy_from_slice(&minute_oldest.to_le_bytes());
        p[n - 8..n - 4].copy_from_slice(&0u32.to_le_bytes());
        p[n - 4..n].copy_from_slice(&0u32.to_le_bytes());
        p
    }

    fn minutes_payload(first: u32, count: u8, boot: u8) -> Vec<u8> {
        let mut p = first.to_le_bytes().to_vec();
        p.push(count);
        for _ in 0..count {
            p.extend([0, 0, 0, 0, 0, 0, 0, 0, 0, boot, 0, 0, 0, 0, 0, 0]);
        }
        p
    }

    fn dongle(boot: u8, uptime_ms: u64, next: u32, oldest: u32) -> Device<Fake> {
        dongle_in(Protocol::Nuphy, boot, uptime_ms, next, oldest)
    }

    fn dongle_in(
        protocol: Protocol,
        boot: u8,
        uptime_ms: u64,
        next: u32,
        oldest: u32,
    ) -> Device<Fake> {
        Device::new(
            Fake::new(move |req| {
                let payload = match req[0] {
                    1 => status_payload(protocol, uptime_ms, next, oldest),
                    2 => {
                        let from = u32::from_le_bytes(req[2..6].try_into().unwrap()).max(oldest);
                        let n = next.saturating_sub(from).min(40) as u8;
                        minutes_payload(from, n, boot)
                    }
                    _ => vec![0, 0, 0, 0, 0],
                };
                parts(req[0], req[1], &payload)
            }),
            0,
        )
    }

    fn fetched(mut dev: Device<Fake>) -> History {
        let s = dev.status().unwrap();
        let mut h = History::new(s.clone());
        h.refresh(&mut dev, s).unwrap();
        h
    }

    fn catch_up(h: &mut History, mut dev: Device<Fake>) -> Result<()> {
        let s = dev.status()?;
        h.refresh(&mut dev, s)
    }

    #[test]
    fn a_failed_fetch_keeps_the_status_it_followed() {
        let mut h = fetched(dongle(1, 200 * 60_000, 130, 0));
        let later = status_payload(Protocol::Nuphy, 201 * 60_000, 131, 0);
        let mut dev = Device::new(
            Fake::new(move |req| match req[0] {
                1 => parts(req[0], req[1], &later),
                _ => vec![error_part(req[0], req[1], ErrorCode::Radio as u8)],
            }),
            0,
        );
        let s = dev.status().unwrap();
        assert!(h.refresh(&mut dev, s).is_err());
        assert_eq!(h.status.uptime_ms, 201 * 60_000);
        assert_eq!(h.slots[0].minutes.len(), 130);
    }

    fn app_watching(watch: Vec<u8>) -> (App<Fake>, std::rc::Rc<std::cell::Cell<usize>>) {
        let asked = std::rc::Rc::new(std::cell::Cell::new(0));
        let count = asked.clone();
        let mut status = status_of(Protocol::Nuphy);
        status.channel = 41;
        let app = app_on(
            Device::new(
                Fake::new(move |req| {
                    if req[0] == Command::Watch as u8 {
                        count.set(count.get() + 1);
                        parts(req[0], req[1], &watch)
                    } else {
                        parts(req[0], req[1], &[0, 0, 0, 0, 0])
                    }
                }),
                0,
            ),
            status,
        );
        (app, asked)
    }

    fn drawn_text<T: Transport>(app: &mut App<T>) -> String {
        screen(&drawn(app)).join("\n")
    }

    #[test]
    fn the_moves_key_shows_the_noise_the_dongle_moves_by() {
        let (mut app, asked) = app_watching(crate::protocol::tests::fixture("watch.hex"));
        press(&mut app, 'm');
        assert_eq!(asked.get(), 1);
        let text = drawn_text(&mut app);
        assert!(text.contains("noise at -75 dBm or louder counts"), "{text}");
        assert!(text.contains("2.91%"), "{text}");
        assert_eq!(app.drawn.plot_width, None);
    }

    #[test]
    fn bands_split_the_row_by_share() {
        assert_eq!(band_cells(&[50, 50, 0, 0, 0, 0], 10), [5, 5, 0, 0, 0, 0]);
        assert_eq!(band_cells(&[60, 0, 0, 0, 0, 0], 10), [10, 0, 0, 0, 0, 0]);
        assert_eq!(band_cells(&[0; NOISE_BUCKETS], 10), [0; NOISE_BUCKETS]);
    }

    #[test]
    fn a_tiny_band_keeps_one_cell_and_the_row_stays_full() {
        let cells = band_cells(&[1000, 200, 30, 5, 0, 1], 60);
        assert_eq!(cells.iter().sum::<usize>(), 60);
        assert_eq!(cells[4], 0);
        assert!(cells[5] >= 1 && cells[3] >= 1, "{cells:?}");
        assert!(cells[0] > cells[1] && cells[1] > cells[2], "{cells:?}");
    }

    #[test]
    fn a_row_too_narrow_for_every_band_keeps_the_loudest() {
        assert_eq!(band_cells(&[10, 10, 10, 10, 10, 10], 4), [0, 0, 1, 1, 1, 1]);
    }

    #[test]
    fn each_channel_ends_in_the_color_of_its_loudest_band() {
        let (mut app, _) = app_watching(crate::protocol::tests::fixture("watch.hex"));
        press(&mut app, 'm');
        let terminal = drawn(&mut app);
        assert_eq!(channel_fg(&terminal, 2, 78), Color::DarkGray);
        assert_eq!(channel_fg(&terminal, 41, 78), Color::Red);
    }

    #[test]
    fn the_moves_pane_marks_the_home_it_reports_as_current() {
        let (mut app, _) = app_watching(crate::protocol::tests::fixture("watch.hex"));
        session(&mut app).history.status.channel = 2;
        press(&mut app, 'm');
        let terminal = drawn(&mut app);
        assert_eq!(
            channel_fg(&terminal, 41, 3),
            role_color(ChannelRole::Current)
        );
        assert_eq!(channel_fg(&terminal, 2, 3), role_color(ChannelRole::Other));
    }

    #[test]
    fn a_poll_rereads_the_moves_while_they_are_shown() {
        let (mut app, asked) = app_watching(crate::protocol::tests::fixture("watch.hex"));
        let status = session(&mut app).history.status.clone();
        app.refresh(Ok(status.clone()));
        assert_eq!(asked.get(), 0);
        press(&mut app, 'm');
        app.refresh(Ok(status));
        assert_eq!(asked.get(), 2);
    }

    #[test]
    fn esc_brings_back_the_history_from_the_moves() {
        let (mut app, _) = app_watching(crate::protocol::tests::fixture("watch.hex"));
        press(&mut app, 'm');
        app.handle(KeyCode::Esc);
        drawn_text(&mut app);
        assert!(app.drawn.plot_width.is_some());
    }

    #[test]
    fn a_dongle_without_moves_keeps_the_history_and_says_why() {
        let mut app = app_on(
            Device::new(
                Fake::new(|req| vec![error_part(req[0], req[1], ErrorCode::UnknownCommand as u8)]),
                0,
            ),
            status_of(Protocol::Nuphy),
        );
        press(&mut app, 'm');
        assert!(matches!(session(&mut app).pane, Pane::History));
        assert!(
            footer(&mut app).starts_with("failed:"),
            "{}",
            footer(&mut app)
        );
    }

    #[test]
    fn the_chart_width_is_kept_only_while_the_chart_is_drawn() {
        let mut app = app_with(status_of(Protocol::Nuphy));
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        assert!(app.drawn.plot_width.is_some());
        session(&mut app).pane = Pane::Noise(Vec::new());
        terminal.draw(|f| app.draw(f)).unwrap();
        assert_eq!(app.drawn.plot_width, None);
    }

    #[test]
    fn a_prompt_is_answered_after_a_redraw_only_on_y() {
        let drawn = Drawn::default();
        let mut app = app_with(status_of(Protocol::Nuphy));
        let s = session(&mut app);
        s.handle(KeyCode::Char('r'), Drawn::default());
        assert!(s.handle(KeyCode::Char('n'), drawn).is_none());
        assert!(s.prompt.is_none() && s.message.is_empty());
        s.handle(KeyCode::Char('r'), Drawn::default());
        let outcome = s.handle(KeyCode::Char('y'), drawn);
        assert!(matches!(
            outcome,
            Some(Outcome::Answer(Prompt::Action(Action::Reboot)))
        ));
        assert_eq!(s.message, "working…");
    }

    #[test]
    fn the_arrows_scroll_the_chart_only_while_it_is_drawn() {
        let mut app = app_with(status_of(Protocol::Nuphy));
        let s = session(&mut app);
        s.history.slots[0].minutes = (0..130).map(|i| (i, quiet())).collect();
        let hidden = Drawn::default();
        s.handle(KeyCode::Left, hidden);
        assert_eq!(s.history.minutes_back, 0);
        let shown = Drawn {
            plot_width: Some(50),
            ..hidden
        };
        s.handle(KeyCode::Left, shown);
        assert_eq!(s.history.minutes_back, HISTORY_STEP_MINUTES);
    }

    #[test]
    fn a_disconnected_view_reads_only_q() {
        let mut app: App<Fake> = App::new(None);
        assert!(app.handle(KeyCode::Char('n')).is_none());
        assert!(matches!(
            app.handle(KeyCode::Char('q')),
            Some(Outcome::Quit)
        ));
    }

    #[test]
    fn the_history_scroll_stops_where_the_chart_is_still_full() {
        let mut h = History::new(status_of(Protocol::Nuphy));
        h.slots[0].minutes = (0..130).map(|i| (i, quiet())).collect();
        for _ in 0..20 {
            h.scroll_older(50);
        }
        assert_eq!(h.minutes_back, 80);
        h.scroll_newer(50);
        assert_eq!(h.minutes_back, 70);
        h.slots[0].minutes.truncate(60);
        h.scroll_newer(50);
        assert_eq!(h.minutes_back, 0);
    }

    #[test]
    fn fetches_all_held_minutes_page_by_page() {
        let h = fetched(dongle(1, 200 * 60_000, 130, 0));
        assert_eq!(h.slots[0].minutes.len(), 130);
        assert_eq!(h.slots[0].minutes.first().map(|m| m.0), Some(0));
        assert_eq!(h.slots[0].minutes.last().map(|m| m.0), Some(129));
    }

    #[test]
    fn a_clear_drops_minutes_before_the_new_oldest() {
        let mut h = fetched(dongle(1, 200 * 60_000, 130, 0));
        catch_up(&mut h, dongle(1, 201 * 60_000, 131, 130)).unwrap();
        assert_eq!(
            h.slots[0].minutes.iter().map(|m| m.0).collect::<Vec<_>>(),
            vec![130]
        );
    }

    #[test]
    fn the_boot_time_holds_across_refreshes() {
        let mut h = fetched(dongle(1, 200 * 60_000, 130, 0));
        let boot = h.boot;
        catch_up(&mut h, dongle(1, 200 * 60_000 + 5_000, 130, 0)).unwrap();
        assert_eq!(h.boot, boot);
    }

    #[test]
    fn a_reboot_replaces_the_boot_time() {
        let mut h = fetched(dongle(1, 200 * 60_000, 130, 0));
        let boot = h.boot;
        catch_up(&mut h, dongle(2, 3 * 60_000, 2, 0)).unwrap();
        assert_ne!(h.boot, boot);
    }

    #[test]
    fn a_reboot_starts_the_history_over() {
        let mut h = fetched(dongle(1, 200 * 60_000, 130, 0));
        h.events_top = Some(0);
        h.minutes_back = 30;
        catch_up(&mut h, dongle(2, 3 * 60_000, 2, 0)).unwrap();
        assert_eq!(h.events_top, None);
        assert_eq!(h.minutes_back, 0);
        assert_eq!(
            h.slots[0]
                .minutes
                .iter()
                .map(|m| (m.0, m.1.device.packets))
                .collect::<Vec<_>>(),
            vec![(0, 2), (1, 2)]
        );
    }

    fn inside_box(row: &str) -> &str {
        row.trim_end().trim_matches('│').trim_end()
    }

    fn status_box(status: &Status) -> Terminal<TestBackend> {
        drawn(&mut app_with(status.clone()))
    }

    #[test]
    fn the_dongle_box_holds_the_dongle_and_channels_and_the_device_box_the_link_and_latency() {
        let s = status_of(Protocol::Nuphy);
        let terminal = status_box(&s);
        let rows = screen(&terminal);
        assert!(
            rows[0].starts_with("┌ SpareDongle · NuPhy ─"),
            "{}",
            rows[0]
        );
        assert_eq!(
            inside_box(&rows[1]),
            format!(
                "dongle    usb live   up {}   pairing open",
                uptime(s.uptime_ms)
            )
        );
        assert_eq!(
            inside_box(&rows[2]),
            "channels  16 22 26 30 34 44 48 58 72 78"
        );
        assert!(rows[3].starts_with("│firmware "), "{}", rows[3]);
        assert!(rows[4].starts_with('└'), "{}", rows[4]);
        assert!(
            rows[5].starts_with("┌ keyboard · MX Keys · 85%, charging ─"),
            "{}",
            rows[5]
        );
        assert_eq!(
            inside_box(&rows[6]),
            "link      connected   -71 dBm   last packet 12 ms ago"
        );
        assert_eq!(inside_box(&rows[7]), "latency   0.9 ms average, 4.2 ms max");
        assert!(rows[8].starts_with('└'), "{}", rows[8]);
    }

    #[test]
    fn a_connected_device_with_no_report_yet_says_so_on_the_latency_row() {
        let mut s = status_of(Protocol::Unifying);
        s.device.latency = None;
        let rows = screen(&status_box(&s));
        assert!(
            rows.iter()
                .any(|r| inside_box(r) == "latency   no report yet"),
            "{rows:#?}"
        );
    }

    #[test]
    fn a_bluetooth_status_box_shows_no_signal_or_channels() {
        let mut s = status_of(Protocol::Bluetooth);
        s.device.signal = None;
        let rows = screen(&status_box(&s));
        let rows = &rows[..CHART_ROW];
        assert!(rows.iter().any(|r| r.contains("not connected")));
        assert!(s.device.latency.is_some());
        for hidden in ["dBm", "channels", "latency"] {
            assert!(
                !rows.iter().any(|r| r.contains(hidden)),
                "{hidden} in {rows:#?}"
            );
        }
    }

    #[test]
    fn a_connected_bluetooth_status_box_shows_the_signal_and_last_packet() {
        let mut s = status_of(Protocol::Bluetooth);
        s.link = Link::Bluetooth(BluetoothLink::Connected);
        s.slots[0].connected = true;
        s.device.signal = Some(-58);
        s.device.last_packet_ms = Some(400);
        let rows = screen(&status_box(&s));
        assert!(rows[1].starts_with("│dongle"), "{}", rows[1]);
        assert_eq!(
            inside_box(&rows[DEVICE_TITLE_ROW + 1]),
            "link      connected   -58 dBm   last packet 400 ms ago"
        );
    }

    #[test]
    fn a_status_box_with_no_reading_shows_no_signal_level() {
        let mut s = status_of(Protocol::Nuphy);
        s.device.signal = None;
        s.device.last_packet_ms = None;
        let rows = screen(&status_box(&s));
        assert_eq!(
            inside_box(&rows[CHANNELS_LINK_ROW]),
            "link      connected   no packet since boot"
        );
    }

    #[test]
    fn a_unifying_device_box_names_the_keyboard_in_its_title() {
        let terminal = status_box(&status_of(Protocol::Unifying));
        let rows = screen(&terminal);
        assert!(
            rows[0].starts_with("┌ SpareDongle · Unifying ─"),
            "{}",
            rows[0]
        );
        assert_eq!(
            inside_box(&rows[2]),
            "channels  5 8 17 32 35 41 44 62 65 71 74"
        );
        let title = &rows[CHANNELS_DEVICE_TITLE_ROW];
        assert!(
            title.starts_with("┌ keyboard · MX Keys · 85%, charging ─"),
            "{title}"
        );
    }

    #[test]
    fn the_device_title_ends_at_the_kind_before_the_keyboard_sent_its_name() {
        let mut unnamed = status_of(Protocol::Nuphy);
        unnamed.device.name = None;
        unnamed.device.battery = None;
        let rows = screen(&status_box(&unnamed));
        let title = &rows[CHANNELS_DEVICE_TITLE_ROW];
        assert!(title.starts_with("┌ keyboard ─"), "{title}");
    }

    #[test]
    fn a_never_paired_nuphy_dongle_says_so_on_the_channels_row() {
        let mut s = status_of(Protocol::Nuphy);
        nuphy(&mut s).own = None;
        s.channel = 16;
        let terminal = status_box(&s);
        assert_eq!(
            inside_box(&screen(&terminal)[2]),
            "channels  16 22 44 48 58 72 78   never paired"
        );
    }

    #[test]
    fn the_channels_row_holds_a_current_channel_the_keyboard_does_not_listen_on() {
        let mut s = status_of(Protocol::Nuphy);
        s.channel = 2;
        let terminal = status_box(&s);
        assert_eq!(
            inside_box(&screen(&terminal)[2]),
            "channels  2 16 22 26 30 34 44 48 58 72 78"
        );
        assert_eq!(cell_at(&terminal, 2, "2 ").modifier, Modifier::REVERSED);
    }

    fn cell_at(terminal: &Terminal<TestBackend>, row: u16, text: &str) -> ratatui::buffer::Cell {
        let line = &screen(terminal)[usize::from(row)];
        let at = line
            .find(text)
            .unwrap_or_else(|| panic!("{text} not in {line}"));
        let col = line[..at].chars().count() as u16;
        terminal.backend().buffer()[(col, row)].clone()
    }

    #[test]
    fn the_channels_row_marks_the_current_channel_and_the_paired_ones() {
        let terminal = status_box(&status_of(Protocol::Nuphy));
        assert_eq!(cell_at(&terminal, 2, "26").modifier, Modifier::REVERSED);
        assert_eq!(cell_at(&terminal, 2, "30").modifier, Modifier::BOLD);
        assert_eq!(cell_at(&terminal, 2, "16").modifier, Modifier::empty());
    }

    #[test]
    fn the_link_state_and_signal_are_colored_by_how_good_they_are() {
        const LINK: u16 = CHANNELS_LINK_ROW as u16;
        let terminal = status_box(&status_of(Protocol::Nuphy));
        assert_eq!(cell_at(&terminal, LINK, "connected").fg, Color::Green);
        assert_eq!(cell_at(&terminal, LINK, "-71 dBm").fg, Color::Yellow);
        let mut weak = status_of(Protocol::Nuphy);
        weak.device.signal = Some(-80);
        assert_eq!(cell_at(&status_box(&weak), LINK, "-80 dBm").fg, Color::Red);
        assert_eq!(cell_at(&terminal, 1, "pairing open").fg, Color::Cyan);
        let mut s = status_of(Protocol::Unifying);
        s.link = Link::Unifying(None);
        let terminal = status_box(&s);
        assert_eq!(cell_at(&terminal, LINK, "not paired").fg, Color::Red);
    }

    fn strip_names(protocol: Option<Protocol>) -> String {
        panel_in(protocol, quiet(), history_min_rows(protocol)).join("\n")
    }

    #[test]
    fn a_panel_leaves_out_the_strips_its_firmware_never_counts() {
        let nuphy = strip_names(Some(Protocol::Nuphy));
        for name in ["losses", "gaps", "dropped", "channel", "busy"] {
            assert!(nuphy.contains(name), "{name} missing from {nuphy}");
        }
        let unifying = strip_names(Some(Protocol::Unifying));
        for name in ["dropped", "channel", "busy"] {
            assert!(unifying.contains(name), "{name} missing from {unifying}");
        }
        for name in ["losses", "gaps"] {
            assert!(!unifying.contains(name), "{name} shown in {unifying}");
        }
        for shared in [strip_names(Some(Protocol::Bluetooth)), strip_names(None)] {
            assert!(shared.contains("dropped"), "dropped missing from {shared}");
            for name in ["losses", "gaps", "channel", "busy"] {
                assert!(!shared.contains(name), "{name} shown in {shared}");
            }
        }
    }

    #[test]
    fn a_chart_without_channels_leaves_out_the_noise_line() {
        assert!(strip_names(Some(Protocol::Nuphy)).contains("━ noise peak"));
        let bluetooth = strip_names(Some(Protocol::Bluetooth));
        assert!(bluetooth.contains("━ min"), "{bluetooth}");
        assert!(!bluetooth.contains("noise"), "{bluetooth}");
    }

    #[test]
    fn the_busy_caption_shows_only_beside_the_busy_strip() {
        let caption = format!("busy: readings at {BUSY_DBM} dBm");
        assert!(strip_names(Some(Protocol::Nuphy)).contains(&format!("└ {caption}")));
        assert!(!strip_names(Some(Protocol::Bluetooth)).contains(&caption));
    }
}
