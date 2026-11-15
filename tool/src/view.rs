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
    ChannelRole, LinkHealth, NOISE_QUESTION, action_question, channel_role, describe_event,
    last_packet, link_health, noise_level, noise_ruler, paired, pairing_open, share, state_name,
    status_label, switch_question, uptime, usb_state, wall_clock,
};
use crate::protocol::{
    Action, BUSY_DBM, Counter, Event, Link, Minute, NoiseChannel, NuphyLink, Protocol, Status,
    Step, step_channel,
};

const POLL_EVERY: Duration = Duration::from_secs(1);
const EVENTS_KEPT: usize = 200;
const EVENT_ROWS: usize = 8;
const CHART_MIN_ROWS: u16 = 5;
const TIME_AXIS_ROWS: u16 = 1;
const STATUS_ROWS: u16 = 3;
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

/// Each line lies over the ones before it.
const SIGNAL_LINES: [(&str, Color, MinuteDbm); 4] = [
    ("noise peak", Color::Gray, |m| m.noise.map(|n| n.peak)),
    ("min", Color::Red, |m| m.rssi.map(|r| r.min)),
    ("avg", Color::Yellow, |m| m.rssi.map(|r| r.avg)),
    ("max", Color::Green, |m| m.rssi.map(|r| r.max)),
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
            Source::Counter(counter) => m.get(counter).unwrap_or(0),
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

/// With no protocol, only the strips both protocols' firmware count.
fn strips(protocol: Option<Protocol>) -> impl Iterator<Item = &'static Strip> {
    let counting = protocol.map_or(Protocol::ALL.to_vec(), |protocol| vec![protocol]);
    STRIPS.iter().filter(move |strip| match strip.source {
        Source::Counter(counter) => counting.iter().all(|p| p.counts(counter)),
        Source::Busy => true,
    })
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

fn legend(
    title: &'static str,
    keys: impl IntoIterator<Item = (&'static str, Color)>,
) -> Line<'static> {
    Line::from_iter(
        [Span::raw(title)]
            .into_iter()
            .chain(keys.into_iter().flat_map(|(name, color)| {
                [
                    Span::raw("  "),
                    Span::styled("━ ", Style::default().fg(color)),
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

/// The dongle's status and history as last fetched, kept in step with its indexes.
pub(crate) struct History {
    pub(crate) status: Status,
    pub(crate) boot: DateTime<Local>,
    pub(crate) minutes: Vec<(u32, Minute)>,
    pub(crate) events: Vec<(u32, Event)>,
    /// The number of the event shown at the top of the events list, or `None` to follow the newest.
    pub(crate) events_top: Option<u32>,
}

fn boot_time(s: &Status) -> DateTime<Local> {
    wall_clock(Local::now(), s.uptime_ms, 0)
}

impl History {
    pub(crate) fn new(status: Status) -> Self {
        History {
            boot: boot_time(&status),
            status,
            minutes: Vec::new(),
            events: Vec::new(),
            events_top: None,
        }
    }

    /// Keeps `s` even when a fetch fails.
    pub(crate) fn refresh<T: Transport>(&mut self, dev: &mut Device<T>, s: Status) -> Result<()> {
        if s.uptime_ms < self.status.uptime_ms {
            self.minutes.clear();
            self.events.clear();
            self.events_top = None;
            self.boot = boot_time(&s);
        }
        self.status = s;
        let s = &self.status;
        self.minutes
            .retain(|(i, _)| *i >= s.minute_oldest && *i < s.minute_next);
        self.events
            .retain(|(i, _)| *i >= s.events_oldest && *i < s.events_total);
        let from = self.minutes.last().map_or(s.minute_oldest, |(i, _)| i + 1);
        self.minutes
            .extend(dev.minutes_between(from, s.minute_next, s.protocol())?);
        let from = self.events.last().map_or(
            s.events_total.saturating_sub(EVENTS_KEPT as u32),
            |(i, _)| i + 1,
        );
        self.events
            .extend(dev.events_between(from, s.events_total)?);
        let excess = self.events.len().saturating_sub(EVENTS_KEPT);
        self.events.drain(..excess);
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

#[derive(Clone, Copy)]
enum Prompt {
    Noise,
    Action(Action),
    Switch(Protocol),
}

impl Prompt {
    fn question(self) -> String {
        let question = match self {
            Prompt::Noise => NOISE_QUESTION.to_string(),
            Prompt::Action(action) => action_question(action).to_string(),
            Prompt::Switch(to) => switch_question(to),
        };
        format!("{question} y/n")
    }
}

#[derive(Clone, Copy)]
enum Key {
    Ask(Prompt),
    Pair(Protocol),
}

fn action_keys(protocol: Protocol) -> [(char, &'static str, Key); 6] {
    const NOISE: (char, &str, Key) = ('n', "noise", Key::Ask(Prompt::Noise));
    const REBOOT: (char, &str, Key) = ('r', "reboot", Key::Ask(Prompt::Action(Action::Reboot)));
    const BOOTLOADER: (char, &str, Key) = (
        'b',
        "bootloader",
        Key::Ask(Prompt::Action(Action::Bootloader)),
    );
    const CLEAR: (char, &str, Key) = ('c', "clear", Key::Ask(Prompt::Action(Action::Clear)));
    let (label, other) = match protocol {
        Protocol::Nuphy => ("unifying", Protocol::Unifying),
        Protocol::Unifying => ("nuphy", Protocol::Nuphy),
    };
    let pair = ('a', "pair", Key::Pair(protocol));
    let switch = ('p', label, Key::Ask(Prompt::Switch(other)));
    [NOISE, pair, switch, REBOOT, BOOTLOADER, CLEAR]
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
    /// The last refresh's error, while the dongle answers but its reply failed.
    refresh_error: Option<String>,
    noise: Option<Vec<NoiseChannel>>,
    prompt: Option<Prompt>,
    message: String,
    scroll: usize,
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

#[derive(Clone, Copy, Default)]
struct Drawn {
    event_rows: usize,
    /// `None` while the history chart is not on screen.
    plot_width: Option<usize>,
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
            let Drawn {
                event_rows: rows,
                plot_width: width,
            } = self.drawn;
            let Some(session) = self.session_mut() else {
                if key.code == KeyCode::Char('q') {
                    return Ok(());
                }
                continue;
            };
            if let Some(prompt) = session.prompt.take() {
                if key.code == KeyCode::Char('y') {
                    session.message = "working…".into();
                    terminal.draw(|f| self.draw(f))?;
                    if let Some(session) = self.session_mut() {
                        session.answer(prompt);
                    }
                } else {
                    session.message.clear();
                }
                continue;
            }
            session.message.clear();
            match key.code {
                KeyCode::Char('q') => return Ok(()),
                KeyCode::Char(c) => session.press(c),
                KeyCode::Esc => session.noise = None,
                KeyCode::Left if let Some(width) = width => session.scroll_older(width),
                KeyCode::Right if let Some(width) = width => session.scroll_newer(width),
                KeyCode::Up => session.scroll_events(EventsMove::Newer(1), rows),
                KeyCode::Down => session.scroll_events(EventsMove::Older(1), rows),
                KeyCode::PageUp => session.scroll_events(EventsMove::Newer(rows), rows),
                KeyCode::PageDown => session.scroll_events(EventsMove::Older(rows), rows),
                KeyCode::Home => session.scroll_events(EventsMove::Newest, rows),
                KeyCode::End => session.scroll_events(EventsMove::Oldest, rows),
                _ => {}
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
    fn new(device: Device<T>, status: Status) -> Self {
        Session {
            device,
            history: History::new(status),
            refresh_error: None,
            noise: None,
            prompt: None,
            message: String::new(),
            scroll: 0,
        }
    }

    fn protocol(&self) -> Protocol {
        self.history.status.protocol()
    }

    fn key(&self, c: char) -> Option<Key> {
        action_keys(self.protocol())
            .into_iter()
            .find(|(k, _, _)| *k == c)
            .map(|(_, _, key)| key)
    }

    fn press(&mut self, c: char) {
        match c {
            '[' => return self.step(Step::Down),
            ']' => return self.step(Step::Up),
            _ => {}
        }
        match self.key(c) {
            Some(Key::Ask(prompt)) => self.prompt = Some(prompt),
            Some(Key::Pair(protocol)) => self.pair(protocol),
            None => {}
        }
    }

    fn pair(&mut self, protocol: Protocol) {
        self.message = match self.device.pair() {
            Ok(()) => pairing_open(protocol).into(),
            Err(e) => format!("failed: {}", error_text(&e)),
        };
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

    /// `scroll` clamped so the chart stays full.
    fn scrolled(&self, width: usize) -> usize {
        self.scroll
            .min(self.history.minutes.len().saturating_sub(width))
    }

    fn scroll_older(&mut self, width: usize) {
        self.scroll = self.scrolled(width) + HISTORY_STEP_MINUTES;
        self.scroll = self.scrolled(width);
    }

    fn scroll_newer(&mut self, width: usize) {
        self.scroll = self.scrolled(width).saturating_sub(HISTORY_STEP_MINUTES);
    }

    fn scroll_events(&mut self, step: EventsMove, rows: usize) {
        let history = &mut self.history;
        history.events_top = scrolled_top(&history.events, history.events_top, rows, step);
    }

    /// Returns the error that lost the dongle.
    fn refresh(&mut self, status: Result<Status>) -> Result<()> {
        let read = status.is_ok();
        match status.and_then(|s| self.history.refresh(&mut self.device, s)) {
            Ok(()) => self.refresh_error = None,
            Err(e) if e.is::<Lost>() => return Err(e),
            Err(e) => self.refresh_error = Some(error_text(&e)),
        }
        if read {
            self.drop_pairing_message_once_closed();
        }
        Ok(())
    }

    /// The dongle closes its pairing window on its own, so the message outlives the window.
    fn drop_pairing_message_once_closed(&mut self) {
        let status = &self.history.status;
        if !status.pairing_open && self.message == pairing_open(status.protocol()) {
            self.message.clear();
        }
    }

    fn answer(&mut self, prompt: Prompt) {
        let result = match prompt {
            Prompt::Noise => self.device.noise().map(|n| {
                self.noise = Some(n);
                "done".to_string()
            }),
            Prompt::Action(c) => self.device.action(c).map(|()| "done".to_string()),
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
        match self.prompt {
            Some(p) => flow(p.question().split(' '), " ", width),
            None if !self.message.is_empty() => flow(self.message.split(' '), " ", width),
            None => match &self.refresh_error {
                Some(e) => flow(e.split(' '), " ", width),
                None => {
                    let hints: Vec<_> = action_keys(self.protocol())
                        .into_iter()
                        .map(|(key, label, _)| format!("{key} {label}"))
                        .chain(
                            [
                                "[/] channel",
                                "←/→ history",
                                "↑/↓ events",
                                "esc back",
                                "q quit",
                            ]
                            .map(String::from),
                        )
                        .collect();
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
        let (session, help) = self.connection.view(f.area().width as usize);
        let status = session.map(|s| &s.history.status);
        let title = match status {
            Some(s) => ["OpenDongle", s.protocol().name()]
                .into_iter()
                .chain(s.link.keyboard_name())
                .collect::<Vec<_>>()
                .join(" · "),
            None => "OpenDongle".into(),
        };
        let [top, middle, bottom, footer] = Layout::vertical([
            Constraint::Length(STATUS_ROWS + 2),
            Constraint::Min(history_min_rows(status.map(Status::protocol))),
            Constraint::Max(EVENT_ROWS as u16 + 2),
            Constraint::Length(help.len() as u16),
        ])
        .areas(f.area());
        f.render_widget(
            Paragraph::new(status_lines(status)).block(Block::bordered().title(title)),
            top,
        );
        let plot_width = if let Some(session) = session
            && let Some(noise) = &session.noise
        {
            draw_noise(f, middle, noise, &session.history.status);
            None
        } else {
            Some(draw_history(f, middle, session))
        };
        let event_rows = draw_events(f, bottom, session.map(|s| &s.history));
        f.render_widget(Paragraph::new(help.join("\n")), footer);
        self.drawn = Drawn {
            event_rows,
            plot_width,
        };
    }
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

fn status_lines(status: Option<&Status>) -> Vec<Line<'static>> {
    let Some(s) = status else {
        return vec![Line::from("disconnected")];
    };
    let signal_style = match s.last_rx_age_ms {
        Some(_) => Style::new().fg(signal_color(s.last_rssi)),
        None => Style::new(),
    };
    let link = vec![
        Span::styled(state_name(s), Style::new().fg(link_color(link_health(s)))),
        Span::raw(SEPARATOR),
        Span::styled(format!("{} dBm", s.last_rssi), signal_style),
        Span::raw(format!("{SEPARATOR}{}", last_packet(s.last_rx_age_ms))),
    ];
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
    vec![
        status_row("link", link),
        status_row("channels", channel_spans(s)),
        status_row("dongle", dongle),
    ]
}

fn draw_history<T: Transport>(f: &mut Frame, area: Rect, session: Option<&Session<T>>) -> usize {
    let block = Block::bordered()
        .title(legend(
            "signal dBm",
            SIGNAL_LINES
                .iter()
                .rev()
                .map(|&(name, color, _)| (name, color)),
        ))
        .title_bottom(format!("busy: readings at {BUSY_DBM} dBm or louder"));
    let inner = block.inner(area);
    f.render_widget(block, area);
    let [gutter, axis_line, plot] = Layout::horizontal([
        Constraint::Length(GUTTER),
        Constraint::Length(1),
        Constraint::Fill(1),
    ])
    .areas(inner);
    let kept: Vec<&Strip> = strips(session.map(Session::protocol)).collect();
    let rows = Layout::vertical(panel_rows(kept.len()));
    let [dbm_labels, strip_labels, _] = rows.areas(gutter);
    let [chart_axis, _, _] = rows.areas(axis_line);
    let [chart, strips, axis] = rows.areas(plot);

    let width = usize::from(plot.width);
    let (shown, boot): (&[(u32, Minute)], _) = match session {
        Some(session) => {
            let minutes = &session.history.minutes;
            let end = minutes.len() - session.scrolled(width);
            (
                &minutes[end.saturating_sub(width)..end],
                Some(session.history.boot),
            )
        }
        None => (&[], None),
    };

    let runs: Vec<_> = SIGNAL_LINES
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
    let bars: Vec<Bar> = noise
        .iter()
        .map(|c| {
            let role = channel_role(c.channel, s);
            Bar::default()
                .value(u64::from(noise_level(c.loudest)))
                .style(Style::default().fg(role_color(role)))
                .text_value(String::new())
        })
        .collect();
    let [chart, ruler] = Layout::vertical([Constraint::Min(3), Constraint::Length(1)]).areas(area);
    let block = Block::bordered()
        .title(legend(
            "loudest per channel",
            [ChannelRole::Current, ChannelRole::Paired]
                .into_iter()
                .filter_map(|role| role.label().map(|name| (name, role_color(role)))),
        ))
        .title_bottom("▲ BLE advertising  ├─┤ Wi-Fi 1/6/11");
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

fn draw_events(f: &mut Frame, area: Rect, history: Option<&History>) -> usize {
    let rows = usize::from(area.height.saturating_sub(2));
    let Some(history) = history else {
        f.render_widget(Block::bordered().title("events"), area);
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
                describe_event(e, &history.status.link)
            ))
        })
        .collect();
    f.render_widget(List::new(items).block(Block::bordered().title(title)), area);
    rows
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::device::tests::{Fake, error_part, parts};
    use crate::protocol::REPORT_LEN;
    use crate::protocol::tests::{nuphy, status_of, status_running};
    use crate::protocol::{Command, ErrorCode, EventKind, Noise, Protocol, Rssi};
    use chrono::TimeZone;
    use ratatui::{Terminal, backend::TestBackend};

    fn events(indexes: Range<u32>) -> Vec<(u32, Event)> {
        let event = Event {
            time_ms: 0,
            kind: EventKind::LinkLost,
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
            packets: 1,
            invalid: 0,
            dropped: 0,
            channel_changes: 0,
            link_losses: Some(0),
            usb_retries: 0,
            gaps: Some(0),
            rssi: None,
            noise: None,
        }
    }

    fn with_signal(dbm: i8) -> Minute {
        Minute {
            rssi: Some(Rssi {
                min: dbm,
                avg: dbm,
                max: dbm,
            }),
            ..quiet()
        }
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
                assert!(quiet().get(counter).is_some(), "{}", strip.name);
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

    fn history<T: Transport>(app: &mut App<T>) -> &mut History {
        &mut session(app).history
    }

    fn panel_in(protocol: Option<Protocol>, marked: Minute, height: u16) -> Vec<String> {
        let mut app = match protocol {
            Some(protocol) => {
                let mut app = app_with(status_of(protocol));
                let h = history(&mut app);
                h.minutes = (0..10).map(|i| (i, quiet())).collect();
                h.minutes[4].1 = marked;
                app
            }
            None => App::new(None),
        };
        let mut terminal = Terminal::new(TestBackend::new(60, height)).unwrap();
        terminal
            .draw(|f| {
                draw_history(f, f.area(), app.session_mut().map(|s| &*s));
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

    fn footer<T: Transport>(app: &mut App<T>) -> String {
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        let rows = screen(&terminal);
        rows[rows.len() - app.connection.view(80).1.len()..]
            .iter()
            .map(|row| row.trim_end())
            .collect::<Vec<_>>()
            .join("\n")
    }

    const UNIFYING_HINTS: [&str; 10] = [
        "n noise",
        "a pair",
        "p nuphy",
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
                "n noise  a pair  p nuphy  r reboot  b bootloader  c clear  ←/→ history",
                "↑/↓ events  esc back  q quit",
            ]
        );
        assert!(lines.iter().all(|l| l.chars().count() <= 80));
    }

    #[test]
    fn text_that_fits_stays_on_one_line() {
        let nuphy = [
            "n noise",
            "p unifying",
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
        let lines = flow(pairing_open(Protocol::Unifying).split(' '), " ", 80);
        assert_eq!(lines.len(), 2, "{lines:?}");
        assert!(lines.iter().all(|l| l.chars().count() <= 80), "{lines:?}");
        assert_eq!(lines.join(" "), pairing_open(Protocol::Unifying));
    }

    #[test]
    fn a_piece_wider_than_the_line_gets_a_line_of_its_own() {
        assert_eq!(flow(["a", "bbbbbb", "c"], " ", 4), ["a", "bbbbbb", "c"]);
    }

    #[test]
    fn the_unifying_footer_flows_onto_the_last_rows_of_an_80_column_screen() {
        let mut app = app_with(status_of(Protocol::Unifying));
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        let rows = screen(&terminal);
        assert_eq!(
            rows[22].trim_end(),
            "n noise  a pair  p nuphy  r reboot  b bootloader  c clear  [/] channel"
        );
        assert_eq!(
            rows[23].trim_end(),
            "←/→ history  ↑/↓ events  esc back  q quit"
        );
        let text = rows.join("\n");
        for part in ["OpenDongle", "signal dBm", "events"] {
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
        history(&mut app).minutes = vec![(0, quiet())];
        let status = session(&mut app).device.status();
        app.refresh(status);
        assert_eq!(
            footer(&mut app),
            "the dongle refused Status: the radio failed"
        );
        assert!(app.session_mut().is_some());
        assert_eq!(history(&mut app).minutes.len(), 1);
    }

    #[test]
    fn a_unifying_dongle_fetches_its_history() {
        let h = fetched(dongle_in(Protocol::Unifying, 1, 200 * 60_000, 130, 0));
        assert_eq!(h.status.protocol(), Protocol::Unifying);
        assert_eq!(h.minutes.len(), 130);
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
            "n noise  a pair  p nuphy  r reboot  b bootloader  c clear  [/] channel  ←/→ history  ↑/↓ events"
        );
        assert_eq!(rows[23].trim_end(), "esc back  q quit");
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
            "a pair",
            "p unifying",
            "r reboot",
            "b bootloader",
            "c clear",
            "[/] channel",
            "←/→ history",
            "↑/↓ events",
            "esc back",
            "q quit",
        ];
        assert_eq!(app.connection.view(120).1, [hints.join("  ")]);
    }

    #[test]
    fn each_mode_offers_switching_to_the_other() {
        let mut app = app_with(status_of(Protocol::Unifying));
        assert!(matches!(
            session(&mut app).key('p'),
            Some(Key::Ask(Prompt::Switch(Protocol::Nuphy)))
        ));
        assert!(matches!(session(&mut app).key('a'), Some(Key::Pair(_))));
        history(&mut app).status = status_of(Protocol::Nuphy);
        assert!(matches!(
            session(&mut app).key('p'),
            Some(Key::Ask(Prompt::Switch(Protocol::Unifying)))
        ));
        assert!(matches!(session(&mut app).key('a'), Some(Key::Pair(_))));
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
        session(&mut app).press('a');
        assert!(session(&mut app).prompt.is_none());
        assert_eq!(sent.get(), Some(Command::Pair as u8));
        assert_eq!(
            footer(&mut app),
            "pairing is open for 30 s; switch the keyboard off and on, after holding an\nEasy-Switch key until it blinks fast if it has them"
        );
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
        session(&mut app).press(']');
        assert_eq!(sent.get(), Some(65));
        assert_eq!(
            footer(&mut app),
            "moved from channel 62 to channel 65; the keyboard follows on its next send"
        );
        assert_eq!(history(&mut app).status.channel, 65);
        let mut status = status_of(Protocol::Nuphy);
        status.channel = 16;
        let (mut app, sent) = app_moving_from(status);
        session(&mut app).press('[');
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
        session(&mut app).press(']');
        assert!(footer(&mut app).starts_with(
            "failed: the dongle refused Move: it moves the link only with a keyboard paired"
        ));
    }

    #[test]
    fn the_status_shows_pairing_open_in_either_mode() {
        let dongle_row = |s: &Status| status_lines(Some(s))[2].to_string();
        assert!(dongle_row(&status_of(Protocol::Unifying)).ends_with("   pairing open"));
        let mut s = status_of(Protocol::Nuphy);
        assert!(dongle_row(&s).ends_with("   pairing open"));
        s.pairing_open = false;
        assert!(!dongle_row(&s).contains("pairing"));
    }

    #[test]
    fn the_pair_key_in_nuphy_mode_says_to_hold_fn_4() {
        let mut app = app_showing("");
        session(&mut app).press('a');
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
        session(&mut app).message = pairing_open(Protocol::Nuphy).into();
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
            let mut app = app_showing(pairing_open(protocol));
            refreshed_with_pairing(&mut app, protocol, true);
            assert_eq!(session(&mut app).message, pairing_open(protocol));
        }
    }

    #[test]
    fn the_pairing_message_goes_once_the_status_no_longer_reads_pairing() {
        let mut app = app_showing(pairing_open(Protocol::Unifying));
        refreshed_with_pairing(&mut app, Protocol::Unifying, false);
        assert_eq!(session(&mut app).message, "");
        assert_eq!(
            footer(&mut app),
            "n noise  a pair  p nuphy  r reboot  b bootloader  c clear  [/] channel\n←/→ history  ↑/↓ events  esc back  q quit"
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

    #[test]
    fn the_noise_legend_fits_an_80_column_terminal() {
        let status = status_of(Protocol::Nuphy);
        let noise: Vec<NoiseChannel> = (2..=80)
            .map(|channel| NoiseChannel {
                channel,
                loudest: -90,
                average: -95,
            })
            .collect();
        let mut terminal = Terminal::new(TestBackend::new(80, 12)).unwrap();
        terminal
            .draw(|f| draw_noise(f, f.area(), &noise, &status))
            .unwrap();
        let rows = screen(&terminal);
        let text = format!("{}{}", rows[0], rows[10]);
        for key in [
            "loudest per channel",
            "━ current",
            "━ paired",
            "▲ BLE advertising",
            "├─┤ Wi-Fi 1/6/11",
        ] {
            assert!(text.contains(key), "{key} missing from {text}");
        }
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
        let marked = Minute {
            gaps: Some(3),
            ..with_signal(-60)
        };
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
        let without = Minute {
            packets: 0,
            ..quiet()
        };
        let shown = [
            (10, with_signal(-60)),
            (11, with_signal(-61)),
            (12, without),
            (13, with_signal(-70)),
        ];
        assert_eq!(
            dbm_runs(&shown, |m| m.rssi.map(|r| r.avg)),
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
        p[4..12].copy_from_slice(&uptime_ms.to_le_bytes());
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
            p.extend([boot, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
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
        assert_eq!(h.minutes.len(), 130);
    }

    #[test]
    fn the_chart_width_is_kept_only_while_the_chart_is_drawn() {
        let mut app = app_with(status_of(Protocol::Nuphy));
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        assert!(app.drawn.plot_width.is_some());
        session(&mut app).noise = Some(Vec::new());
        terminal.draw(|f| app.draw(f)).unwrap();
        assert_eq!(app.drawn.plot_width, None);
    }

    #[test]
    fn the_history_scroll_stops_where_the_chart_is_still_full() {
        let mut app = app_with(status_of(Protocol::Nuphy));
        let s = session(&mut app);
        s.history.minutes = (0..130).map(|i| (i, quiet())).collect();
        for _ in 0..20 {
            s.scroll_older(50);
        }
        assert_eq!(s.scroll, 80);
        s.scroll_newer(50);
        assert_eq!(s.scroll, 70);
        s.history.minutes.truncate(60);
        s.scroll_newer(50);
        assert_eq!(s.scroll, 0);
    }

    #[test]
    fn fetches_all_held_minutes_page_by_page() {
        let h = fetched(dongle(1, 200 * 60_000, 130, 0));
        assert_eq!(h.minutes.len(), 130);
        assert_eq!(h.minutes.first().map(|m| m.0), Some(0));
        assert_eq!(h.minutes.last().map(|m| m.0), Some(129));
    }

    #[test]
    fn a_clear_drops_minutes_before_the_new_oldest() {
        let mut h = fetched(dongle(1, 200 * 60_000, 130, 0));
        catch_up(&mut h, dongle(1, 201 * 60_000, 131, 130)).unwrap();
        assert_eq!(h.minutes.iter().map(|m| m.0).collect::<Vec<_>>(), vec![130]);
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
        catch_up(&mut h, dongle(2, 3 * 60_000, 2, 0)).unwrap();
        assert_eq!(h.events_top, None);
        assert_eq!(
            h.minutes
                .iter()
                .map(|m| (m.0, m.1.packets))
                .collect::<Vec<_>>(),
            vec![(0, 2), (1, 2)]
        );
    }

    fn inside_box(row: &str) -> &str {
        row.trim_end().trim_matches('│').trim_end()
    }

    fn status_box(status: &Status) -> Terminal<TestBackend> {
        let mut app = app_with(status.clone());
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        terminal
    }

    #[test]
    fn the_status_box_holds_a_row_each_for_link_channels_and_dongle() {
        let s = status_of(Protocol::Nuphy);
        let terminal = status_box(&s);
        let rows = screen(&terminal);
        assert!(
            rows[0].starts_with("┌OpenDongle · NuPhy · MX Keys"),
            "{}",
            rows[0]
        );
        assert_eq!(
            inside_box(&rows[1]),
            "link      connected   -71 dBm   last packet 12 ms ago"
        );
        assert_eq!(
            inside_box(&rows[2]),
            "channels  16 22 26 30 34 44 48 58 72 78"
        );
        assert_eq!(
            inside_box(&rows[3]),
            format!(
                "dongle    usb live   up {}   pairing open",
                uptime(s.uptime_ms)
            )
        );
        assert!(rows[4].starts_with('└'), "{}", rows[4]);
    }

    #[test]
    fn a_unifying_status_box_names_the_keyboard_in_its_title() {
        let terminal = status_box(&status_of(Protocol::Unifying));
        let rows = screen(&terminal);
        assert!(
            rows[0].starts_with("┌OpenDongle · Unifying · MX Keys"),
            "{}",
            rows[0]
        );
        assert_eq!(
            inside_box(&rows[2]),
            "channels  5 8 17 32 35 41 44 62 65 71 74"
        );
    }

    #[test]
    fn the_title_ends_at_the_protocol_before_the_keyboard_sent_its_name() {
        let mut unnamed = status_of(Protocol::Nuphy);
        nuphy(&mut unnamed).keyboard_name = None;
        let rows = screen(&status_box(&unnamed));
        assert!(rows[0].starts_with("┌OpenDongle · NuPhy─"), "{}", rows[0]);
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
        let terminal = status_box(&status_of(Protocol::Nuphy));
        assert_eq!(cell_at(&terminal, 1, "connected").fg, Color::Green);
        assert_eq!(cell_at(&terminal, 1, "-71 dBm").fg, Color::Yellow);
        let mut weak = status_of(Protocol::Nuphy);
        weak.last_rssi = -80;
        assert_eq!(cell_at(&status_box(&weak), 1, "-80 dBm").fg, Color::Red);
        assert_eq!(cell_at(&terminal, 3, "pairing open").fg, Color::Cyan);
        let mut s = status_of(Protocol::Unifying);
        s.link = Link::Unifying(None);
        let terminal = status_box(&s);
        assert_eq!(cell_at(&terminal, 1, "not paired").fg, Color::Red);
    }

    fn strip_names(protocol: Option<Protocol>) -> String {
        panel_in(protocol, quiet(), history_min_rows(protocol)).join("\n")
    }

    #[test]
    fn the_unifying_panel_leaves_out_the_strips_its_firmware_never_counts() {
        let nuphy = strip_names(Some(Protocol::Nuphy));
        for name in ["losses", "gaps", "dropped", "channel", "busy"] {
            assert!(nuphy.contains(name), "{name} missing from {nuphy}");
        }
        for shared in [strip_names(Some(Protocol::Unifying)), strip_names(None)] {
            for name in ["dropped", "channel", "busy"] {
                assert!(shared.contains(name), "{name} missing from {shared}");
            }
            for name in ["losses", "gaps"] {
                assert!(!shared.contains(name), "{name} shown in {shared}");
            }
        }
    }
}
