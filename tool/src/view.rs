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
    },
};

use crate::device::{self, Device, Lost, Transport};
use crate::format::{
    ChannelRole, LinkHealth, NOISE_QUESTION, action_question, channel_role, describe_event,
    last_packet, link_health, noise_level, noise_ruler, paired, pairing_open, share, state_name,
    switch_question, uptime, usb_state, wall_clock, wpid,
};
use crate::protocol::{
    Action, BUSY_DBM, Counter, Event, Minute, NoiseChannel, Protocol, Status, Step, step_channel,
};

const POLL_EVERY: Duration = Duration::from_secs(1);
const EVENTS_KEPT: usize = 200;
const EVENT_ROWS: usize = 8;
const CHART_MIN_ROWS: u16 = 5;
const TIME_AXIS_ROWS: u16 = 1;
const STATUS_ROWS: u16 = 3;
const STATUS_LABEL_WIDTH: usize = 10;
const SEPARATOR: &str = "   ";
/// A link that averaged -70 dBm had no gaps, and gaps followed packets at -78 dBm or weaker.
const SIGNAL_GOOD_DBM: i8 = -70;
const SIGNAL_WEAK_DBM: i8 = -78;
const CLOCK: &str = "%H:%M:%S";

/// Wide enough for a strip's name beside its scale at 100.00%.
const GUTTER: u16 = 12;
const DBM_FLOOR: i8 = -100;
const DBM_CEILING: i8 = -20;
const DBM_LABELS: [i8; 3] = [DBM_FLOOR, -60, DBM_CEILING];

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

fn strips(protocol: Protocol) -> impl Iterator<Item = &'static Strip> {
    STRIPS.iter().filter(move |strip| match strip.source {
        Source::Counter(counter) => protocol.counters().contains(&counter),
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

fn history_min_rows(protocol: Protocol) -> u16 {
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
    Line::from_iter(
        std::iter::once(Span::raw(format!("{label:<STATUS_LABEL_WIDTH$}"))).chain(spans),
    )
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
    let mut spans = Vec::new();
    for channel in s.keyboard_channels() {
        if !spans.is_empty() {
            spans.push(Span::raw(" "));
        }
        spans.push(Span::styled(
            channel.to_string(),
            role_style(channel_role(channel, s)),
        ));
    }
    if s.never_paired() {
        spans.push(Span::raw(format!("{SEPARATOR}{}", paired(s.own))));
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

/// The chart row a reading lands on, by the braille canvas's own scaling of four dots to a row.
fn dbm_row(dbm: i8, rows: u16) -> usize {
    let dots = f64::from(rows) * 4.0 - 1.0;
    let span = f64::from(DBM_CEILING) - f64::from(DBM_FLOOR);
    ((f64::from(DBM_CEILING) - f64::from(dbm)) * dots / span) as usize / 4
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

/// The dongle's history as last fetched, kept in step with its indexes.
#[derive(Default)]
pub(crate) struct History {
    pub(crate) status: Option<Status>,
    pub(crate) boot: Option<DateTime<Local>>,
    pub(crate) minutes: Vec<(u32, Minute)>,
    pub(crate) events: Vec<(u32, Event)>,
    /// The number of the event shown at the top of the events list, or `None` to follow the newest.
    pub(crate) events_top: Option<u32>,
}

impl History {
    fn forget_history(&mut self) {
        self.minutes.clear();
        self.events.clear();
        self.events_top = None;
    }

    pub(crate) fn refresh<T: Transport>(&mut self, dev: &mut Device<T>, s: Status) -> Result<()> {
        if self
            .status
            .as_ref()
            .is_some_and(|old| s.uptime_ms < old.uptime_ms)
        {
            self.forget_history();
            self.boot = None;
        }
        self.boot
            .get_or_insert_with(|| wall_clock(Local::now(), s.uptime_ms, 0));
        self.minutes
            .retain(|(i, _)| *i >= s.minute_oldest && *i < s.minute_next);
        self.events
            .retain(|(i, _)| *i >= s.events_oldest && *i < s.events_total);
        let from = self.minutes.last().map_or(s.minute_oldest, |(i, _)| i + 1);
        self.minutes
            .extend(dev.minutes_between(from, s.minute_next)?);
        let from = self.events.last().map_or(
            s.events_total.saturating_sub(EVENTS_KEPT as u32),
            |(i, _)| i + 1,
        );
        self.events
            .extend(dev.events_between(from, s.events_total)?);
        let excess = self.events.len().saturating_sub(EVENTS_KEPT);
        self.events.drain(..excess);
        self.status = Some(s);
        Ok(())
    }
}

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
    Pair,
}

fn action_keys(protocol: Protocol) -> [(char, &'static str, Key); 6] {
    const NOISE: (char, &str, Key) = ('n', "noise", Key::Ask(Prompt::Noise));
    const PAIR: (char, &str, Key) = ('a', "pair", Key::Pair);
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
    let switch = ('p', label, Key::Ask(Prompt::Switch(other)));
    [NOISE, PAIR, switch, REBOOT, BOOTLOADER, CLEAR]
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

struct App<T: Transport = device::HidTransport> {
    serial: Option<String>,
    device: Option<Device<T>>,
    history: History,
    noise: Option<Vec<NoiseChannel>>,
    prompt: Option<Prompt>,
    message: String,
    /// The last refresh's error, while the dongle answers but its reply failed.
    refresh_error: Option<String>,
    scroll: usize,
    event_rows: usize,
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
            if let Some(prompt) = self.prompt.take() {
                if key.code == KeyCode::Char('y') {
                    self.message = "working…".into();
                    terminal.draw(|f| self.draw(f))?;
                    self.answer(prompt);
                } else {
                    self.message.clear();
                }
                continue;
            }
            if self.device.is_some() {
                self.message.clear();
            }
            match key.code {
                KeyCode::Char('q') => return Ok(()),
                KeyCode::Char(c) => self.press(c),
                KeyCode::Esc => self.noise = None,
                KeyCode::Left => self.scroll += 10,
                KeyCode::Right => self.scroll = self.scroll.saturating_sub(10),
                KeyCode::Up => self.scroll_events(EventsMove::Newer(1)),
                KeyCode::Down => self.scroll_events(EventsMove::Older(1)),
                KeyCode::PageUp => self.scroll_events(EventsMove::Newer(self.event_rows)),
                KeyCode::PageDown => self.scroll_events(EventsMove::Older(self.event_rows)),
                KeyCode::Home => self.scroll_events(EventsMove::Newest),
                KeyCode::End => self.scroll_events(EventsMove::Oldest),
                _ => {}
            }
        }
    }

    fn poll(&mut self) {
        let status = match self.device.as_mut() {
            Some(dev) => dev.status(),
            None => match device::open(self.serial.as_deref()) {
                Ok((d, status)) => {
                    self.device = Some(d);
                    self.message.clear();
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

impl<T: Transport> App<T> {
    fn new(serial: Option<String>) -> Self {
        App {
            serial,
            device: None,
            history: History::default(),
            noise: None,
            prompt: None,
            message: String::new(),
            refresh_error: None,
            scroll: 0,
            event_rows: EVENT_ROWS,
        }
    }

    fn protocol(&self) -> Protocol {
        self.history
            .status
            .as_ref()
            .map_or(Protocol::Nuphy, |s| s.protocol)
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
            Some(Key::Pair) => self.pair(),
            None => {}
        }
    }

    fn pair(&mut self) {
        let Some(dev) = self.device.as_mut() else {
            self.message = "disconnected".into();
            return;
        };
        self.message = match dev.pair() {
            Ok(()) => pairing_open(self.protocol()).into(),
            Err(e) => format!("failed: {}", error_text(&e)),
        };
    }

    fn step(&mut self, step: Step) {
        let (Some(dev), Some(status)) = (self.device.as_mut(), self.history.status.as_mut()) else {
            self.message = "disconnected".into();
            return;
        };
        let Some(to) = step_channel(&status.keyboard_channels(), status.channel, step) else {
            return;
        };
        self.message = match crate::cli::move_link(dev, status, to) {
            Ok(line) => {
                status.channel = to;
                line
            }
            Err(e) => format!("failed: {}", error_text(&e)),
        };
    }

    fn scroll_events(&mut self, step: EventsMove) {
        self.history.events_top = scrolled_top(
            &self.history.events,
            self.history.events_top,
            self.event_rows,
            step,
        );
    }

    fn refresh(&mut self, status: Result<Status>) {
        let Some(dev) = self.device.as_mut() else {
            return;
        };
        match status.and_then(|s| self.history.refresh(dev, s)) {
            Ok(()) => {
                self.refresh_error = None;
                self.drop_pairing_message_once_closed();
            }
            Err(e) if e.is::<Lost>() => self.disconnect(&e),
            Err(e) => self.refresh_error = Some(error_text(&e)),
        }
    }

    /// The dongle closes its pairing window on its own, so the message outlives the window.
    fn drop_pairing_message_once_closed(&mut self) {
        let pairing = self.history.status.as_ref().is_some_and(|s| s.pairing_open);
        if !pairing && self.message == pairing_open(self.protocol()) {
            self.message.clear();
        }
    }

    fn disconnect(&mut self, reason: &anyhow::Error) {
        self.device = None;
        self.history = History::default();
        self.refresh_error = None;
        self.message = format!("disconnected: {}", error_text(reason));
    }

    fn answer(&mut self, prompt: Prompt) {
        let Some(dev) = self.device.as_mut() else {
            self.message = "disconnected".into();
            return;
        };
        let result = match prompt {
            Prompt::Noise => dev.noise().map(|n| {
                self.noise = Some(n);
                "done".to_string()
            }),
            Prompt::Action(c) => dev.action(c).map(|()| "done".to_string()),
            Prompt::Switch(to) => dev.protocol(Some(to)).map(|(_, rebooting)| {
                if rebooting {
                    format!("switching to {}; the dongle reboots", to.name())
                } else {
                    format!("the dongle already runs {}", to.name())
                }
            }),
        };
        self.message = result.unwrap_or_else(|e| format!("failed: {}", error_text(&e)));
    }

    fn draw(&mut self, f: &mut Frame) {
        let title = match &self.history.status {
            Some(s) => format!("OpenDongle · {}", s.protocol.name()),
            None => "OpenDongle".into(),
        };
        let status = Paragraph::new(self.status_lines()).block(Block::bordered().title(title));
        let help = self.help(f.area().width as usize);
        let [top, middle, bottom, footer] = Layout::vertical([
            Constraint::Length(STATUS_ROWS + 2),
            Constraint::Min(history_min_rows(self.protocol())),
            Constraint::Max(EVENT_ROWS as u16 + 2),
            Constraint::Length(help.len() as u16),
        ])
        .areas(f.area());
        f.render_widget(status, top);
        match &self.noise {
            Some(noise) => self.draw_noise(f, middle, noise),
            None => self.draw_history(f, middle),
        }
        self.draw_events(f, bottom);
        f.render_widget(Paragraph::new(help.join("\n")), footer);
    }

    fn help(&self, width: usize) -> Vec<String> {
        match self.prompt {
            Some(p) => flow(p.question().split(' '), " ", width),
            None if !self.message.is_empty() => flow(self.message.split(' '), " ", width),
            None => match &self.refresh_error {
                Some(e) => flow(e.split(' '), " ", width),
                None => {
                    let actions = action_keys(self.protocol())
                        .into_iter()
                        .map(|(key, label, _)| format!("{key} {label}"));
                    let hints: Vec<_> = actions
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

    fn status_lines(&self) -> Vec<Line<'static>> {
        let Some(s) = &self.history.status else {
            return vec![Line::from("disconnected")];
        };
        let keyboard = match s.protocol {
            Protocol::Unifying => format!(", keyboard {}", wpid(s.keyboard_id)),
            Protocol::Nuphy => String::new(),
        };
        let signal_style = match s.last_rx_age_ms {
            Some(_) => Style::new().fg(signal_color(s.last_rssi)),
            None => Style::new(),
        };
        let link = vec![
            Span::styled(state_name(s), Style::new().fg(link_color(link_health(s)))),
            Span::raw(format!(" on {}{keyboard}{SEPARATOR}", s.channel)),
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

    fn draw_history(&mut self, f: &mut Frame, area: Rect) {
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
        let kept: Vec<&Strip> = strips(self.protocol()).collect();
        let rows = Layout::vertical(panel_rows(kept.len()));
        let [dbm_labels, strip_labels, _] = rows.areas(gutter);
        let [chart_axis, _, _] = rows.areas(axis_line);
        let [chart, strips, axis] = rows.areas(plot);

        let width = usize::from(plot.width);
        self.scroll = self
            .scroll
            .min(self.history.minutes.len().saturating_sub(width));
        let end = self.history.minutes.len().saturating_sub(self.scroll);
        let shown = &self.history.minutes[end.saturating_sub(width)..end];

        let runs: Vec<_> = SIGNAL_LINES
            .iter()
            .map(|(_, color, pick)| (*color, dbm_runs(shown, *pick)))
            .collect();
        let datasets: Vec<Dataset> = runs
            .iter()
            .flat_map(|(color, runs)| {
                runs.iter().map(|run| {
                    Dataset::default()
                        .marker(symbols::Marker::Braille)
                        .graph_type(GraphType::Line)
                        .style(Style::default().fg(*color))
                        .data(run)
                })
            })
            .collect();
        f.render_widget(
            Chart::new(datasets)
                .x_axis(Axis::default().bounds([0.0, width.saturating_sub(1).max(1) as f64]))
                .y_axis(Axis::default().bounds([f64::from(DBM_FLOOR), f64::from(DBM_CEILING)])),
            chart,
        );
        f.render_widget(Block::new().borders(Borders::LEFT), chart_axis);
        let mut label_rows = vec![Line::default(); usize::from(dbm_labels.height)];
        for dbm in DBM_LABELS {
            if let Some(row) = label_rows.get_mut(dbm_row(dbm, dbm_labels.height)) {
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

        if let Some(boot) = self.history.boot {
            f.render_widget(Paragraph::new(time_axis(boot, shown)), axis);
        }
    }

    fn draw_noise(&self, f: &mut Frame, area: Rect, noise: &[NoiseChannel]) {
        let s = self.history.status.as_ref();
        let bars: Vec<Bar> = noise
            .iter()
            .map(|c| {
                let role = s.map_or(ChannelRole::Other, |s| channel_role(c.channel, s));
                Bar::default()
                    .value(u64::from(noise_level(c.loudest)))
                    .style(Style::default().fg(role_color(role)))
                    .text_value(String::new())
            })
            .collect();
        let [chart, ruler] =
            Layout::vertical([Constraint::Min(3), Constraint::Length(1)]).areas(area);
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

    fn draw_events(&mut self, f: &mut Frame, area: Rect) {
        self.event_rows = usize::from(area.height.saturating_sub(2));
        let protocol = self.protocol();
        let events = &self.history.events;
        let shown = events_window(events, self.history.events_top, self.event_rows);
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
        let items: Vec<ListItem> = match self.history.boot {
            Some(boot) => events[shown]
                .iter()
                .rev()
                .map(|(_, e)| {
                    let at = boot + TimeDelta::milliseconds(e.time_ms as i64);
                    ListItem::new(format!(
                        "{}  {}",
                        at.format(CLOCK),
                        describe_event(e, protocol)
                    ))
                })
                .collect(),
            None => Vec::new(),
        };
        f.render_widget(List::new(items).block(Block::bordered().title(title)), area);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::device::tests::{Fake, error_part, parts};
    use crate::protocol::REPORT_LEN;
    use crate::protocol::tests::fixture;
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
            link_losses: 0,
            usb_retries: 0,
            gaps: 0,
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
        panel_in(Protocol::Nuphy, marked, height)
    }

    fn panel_in(protocol: Protocol, marked: Minute, height: u16) -> Vec<String> {
        let mut app: App = App::new(None);
        app.history.status = Some(Status {
            protocol,
            ..nuphy_status()
        });
        app.history.boot = Some(boot());
        app.history.minutes = (0..10).map(|i| (i, quiet())).collect();
        app.history.minutes[4].1 = marked;
        let mut terminal = Terminal::new(TestBackend::new(60, height)).unwrap();
        terminal.draw(|f| app.draw_history(f, f.area())).unwrap();
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
        rows[rows.len() - app.help(80).len()..]
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
        let mut app: App = App::new(None);
        app.history.status = Some(unifying_status());
        app.history.boot = Some(boot());
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
        let mut app = App::new(None);
        app.device = Some(Device::new(
            Fake::new(|req| vec![error_part(req[0], req[1], ErrorCode::Radio as u8)]),
            0,
        ));
        app.history.minutes = vec![(0, quiet())];
        let status = app.device.as_mut().unwrap().status();
        app.refresh(status);
        assert_eq!(
            footer(&mut app),
            "the dongle refused Status: the radio failed"
        );
        assert!(app.device.is_some());
        assert_eq!(app.history.minutes.len(), 1);
    }

    fn unifying_payload() -> Vec<u8> {
        crate::protocol::tests::status_running(Protocol::Unifying as u8)
    }

    fn nuphy_status() -> Status {
        crate::protocol::read_status(&fixture("status.hex"))
            .unwrap()
            .unwrap()
    }

    fn unifying_status() -> Status {
        let mut s = crate::protocol::read_status(&unifying_payload())
            .unwrap()
            .unwrap();
        s.link_state = 1;
        s
    }

    #[test]
    fn a_unifying_dongle_fetches_its_history() {
        let mut dev = dongle(1, 200 * 60_000, 130, 0);
        let mut s = dev.status().unwrap();
        s.protocol = Protocol::Unifying;
        let mut h = History::default();
        h.refresh(&mut dev, s).unwrap();
        assert_eq!(h.minutes.len(), 130);
    }

    #[test]
    fn a_unifying_dongle_draws_the_full_layout_and_its_keys() {
        let mut app: App = App::new(None);
        app.history.status = Some(unifying_status());
        app.history.boot = Some(boot());
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
        let mut app: App = App::new(None);
        app.history.status = Some(unifying_status());
        app.history.boot = Some(boot());
        let mut terminal = Terminal::new(TestBackend::new(80, 10)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        assert_eq!(screen(&terminal).len(), 10);
    }

    #[test]
    fn the_nuphy_footer_is_one_line_of_hints() {
        let mut app: App = App::new(None);
        app.history.status = Some(nuphy_status());
        assert_eq!(app.protocol(), Protocol::Nuphy);
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
        assert_eq!(app.help(120), [hints.join("  ")]);
    }

    #[test]
    fn each_mode_offers_switching_to_the_other() {
        let mut app: App = App::new(None);
        app.history.status = Some(unifying_status());
        assert!(matches!(
            app.key('p'),
            Some(Key::Ask(Prompt::Switch(Protocol::Nuphy)))
        ));
        assert!(matches!(app.key('a'), Some(Key::Pair)));
        app.history.status.as_mut().unwrap().protocol = Protocol::Nuphy;
        assert!(matches!(
            app.key('p'),
            Some(Key::Ask(Prompt::Switch(Protocol::Unifying)))
        ));
        assert!(matches!(app.key('a'), Some(Key::Pair)));
        assert!(matches!(app.key('n'), Some(Key::Ask(Prompt::Noise))));
    }

    #[test]
    fn switching_asks_the_dongle_for_the_other_protocol() {
        let sent = std::rc::Rc::new(std::cell::Cell::new(None));
        let seen = sent.clone();
        let mut app = App::new(None);
        app.device = Some(Device::new(
            Fake::new(move |req| {
                seen.set(Some((
                    req[0],
                    u32::from_le_bytes(req[2..6].try_into().unwrap()),
                )));
                parts(req[0], req[1], &[2, 1])
            }),
            0,
        ));
        app.answer(Prompt::Switch(Protocol::Unifying));
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
        let mut app = App::new(None);
        app.history.status = Some(unifying_status());
        app.device = Some(Device::new(
            Fake::new(move |req| {
                seen.set(Some(req[0]));
                parts(req[0], req[1], &[])
            }),
            0,
        ));
        app.press('a');
        assert!(app.prompt.is_none());
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
        let mut app = App::new(None);
        app.history.status = Some(status);
        app.device = Some(Device::new(
            Fake::new(move |req| {
                assert_eq!(req[0], Command::Move as u8);
                seen.set(Some(u32::from_le_bytes(req[2..6].try_into().unwrap())));
                parts(req[0], req[1], &[from])
            }),
            0,
        ));
        (app, sent)
    }

    #[test]
    fn the_bracket_keys_move_to_the_neighbouring_channel() {
        let mut status = unifying_status();
        status.channel = 62;
        let (mut app, sent) = app_moving_from(status);
        app.press(']');
        assert_eq!(sent.get(), Some(65));
        assert_eq!(
            footer(&mut app),
            "moved from channel 62 to channel 65; the keyboard follows on its next send"
        );
        assert_eq!(app.history.status.as_ref().unwrap().channel, 65);
        let mut status = nuphy_status();
        status.own = Some([26, 30, 34]);
        status.channel = 16;
        let (mut app, sent) = app_moving_from(status);
        app.press('[');
        assert_eq!(sent.get(), Some(78));
    }

    #[test]
    fn a_refused_move_shows_why() {
        let mut app = App::new(None);
        app.history.status = Some(unifying_status());
        app.device = Some(Device::new(
            Fake::new(|req| vec![error_part(req[0], req[1], ErrorCode::NotMovable as u8)]),
            0,
        ));
        app.press(']');
        assert!(footer(&mut app).starts_with(
            "failed: the dongle refused Move: it moves the link only with a keyboard paired"
        ));
    }

    #[test]
    fn the_status_shows_pairing_open_in_either_mode() {
        let mut app: App = App::new(None);
        app.history.status = Some(unifying_status());
        assert!(
            app.status_lines()[2]
                .to_string()
                .ends_with("   pairing open")
        );
        app.history.status = Some(nuphy_status());
        assert!(
            app.status_lines()[2]
                .to_string()
                .ends_with("   pairing open")
        );
        app.history.status.as_mut().unwrap().pairing_open = false;
        assert!(!app.status_lines()[2].to_string().contains("pairing"));
    }

    #[test]
    fn the_pair_key_in_nuphy_mode_says_to_hold_fn_4() {
        let mut app = app_showing("");
        app.history.status = Some(nuphy_status());
        app.press('a');
        assert_eq!(
            footer(&mut app),
            "pairing is open for 30 s; hold Fn+4 on the keyboard for 3 s"
        );
    }

    fn refreshed_with_pairing(app: &mut App<Fake>, protocol: Protocol, open: bool) {
        let mut s = unifying_status();
        s.protocol = protocol;
        s.pairing_open = open;
        s.minute_next = s.minute_oldest;
        s.events_total = s.events_oldest;
        app.refresh(Ok(s));
    }

    fn app_showing(message: &str) -> App<Fake> {
        let mut app = App::new(None);
        app.device = Some(Device::new(Fake::new(|req| parts(req[0], req[1], &[])), 0));
        app.message = message.into();
        app
    }

    #[test]
    fn the_pairing_message_stays_while_the_status_reads_pairing() {
        for protocol in [Protocol::Nuphy, Protocol::Unifying] {
            let mut app = app_showing(pairing_open(protocol));
            refreshed_with_pairing(&mut app, protocol, true);
            assert_eq!(app.message, pairing_open(protocol));
        }
    }

    #[test]
    fn the_pairing_message_goes_once_the_status_no_longer_reads_pairing() {
        let mut app = app_showing(pairing_open(Protocol::Unifying));
        refreshed_with_pairing(&mut app, Protocol::Unifying, false);
        assert_eq!(app.message, "");
        assert_eq!(
            footer(&mut app),
            "n noise  a pair  p nuphy  r reboot  b bootloader  c clear  [/] channel\n←/→ history  ↑/↓ events  esc back  q quit"
        );
    }

    #[test]
    fn another_message_stays_when_the_status_no_longer_reads_pairing() {
        let mut app = app_showing("done");
        refreshed_with_pairing(&mut app, Protocol::Unifying, false);
        assert_eq!(app.message, "done");
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
        let mut app = App::new(None);
        app.device = Some(Device::new(Unplugged, 0));
        let status = app.device.as_mut().unwrap().status();
        app.refresh(status);
        assert_eq!(
            footer(&mut app),
            "disconnected: writing to the dongle: hidapi error: device disconnected"
        );
        assert!(app.device.is_none());
    }

    #[test]
    fn a_failed_action_shows_its_cause() {
        let mut app = App::new(None);
        app.device = Some(Device::new(Unplugged, 0));
        app.answer(Prompt::Action(Action::Reboot));
        assert_eq!(
            footer(&mut app),
            "failed: writing to the dongle: hidapi error: device disconnected"
        );
    }

    #[test]
    fn the_noise_legend_fits_an_80_column_terminal() {
        let app: App = App::new(None);
        let noise: Vec<NoiseChannel> = (2..=80)
            .map(|channel| NoiseChannel {
                channel,
                loudest: -90,
                average: -95,
            })
            .collect();
        let mut terminal = Terminal::new(TestBackend::new(80, 12)).unwrap();
        terminal
            .draw(|f| app.draw_noise(f, f.area(), &noise))
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
            gaps: 3,
            ..with_signal(-60)
        };
        let rows = panel_with(marked, history_min_rows(Protocol::Nuphy));
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
            for height in history_min_rows(Protocol::Nuphy)..history_min_rows(Protocol::Nuphy) + 6 {
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
        let rows = panel_with(marked, history_min_rows(Protocol::Nuphy));
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

    fn status_payload(uptime_ms: u64, minute_next: u32, minute_oldest: u32) -> Vec<u8> {
        let mut p = fixture("status.hex");
        p[4..12].copy_from_slice(&uptime_ms.to_le_bytes());
        // The protocol and pairing bytes follow the event counts.
        let n = p.len() - 2;
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
        Device::new(
            Fake::new(move |req| {
                let payload = match req[0] {
                    1 => status_payload(uptime_ms, next, oldest),
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

    fn catch_up(h: &mut History, mut dev: Device<Fake>) -> Result<()> {
        let s = dev.status()?;
        h.refresh(&mut dev, s)
    }

    #[test]
    fn fetches_all_held_minutes_page_by_page() {
        let mut h = History::default();
        catch_up(&mut h, dongle(1, 200 * 60_000, 130, 0)).unwrap();
        assert_eq!(h.minutes.len(), 130);
        assert_eq!(h.minutes.first().map(|m| m.0), Some(0));
        assert_eq!(h.minutes.last().map(|m| m.0), Some(129));
    }

    #[test]
    fn a_clear_drops_minutes_before_the_new_oldest() {
        let mut h = History::default();
        catch_up(&mut h, dongle(1, 200 * 60_000, 130, 0)).unwrap();
        catch_up(&mut h, dongle(1, 201 * 60_000, 131, 130)).unwrap();
        assert_eq!(h.minutes.iter().map(|m| m.0).collect::<Vec<_>>(), vec![130]);
    }

    #[test]
    fn the_boot_time_holds_across_refreshes() {
        let mut h = History::default();
        catch_up(&mut h, dongle(1, 200 * 60_000, 130, 0)).unwrap();
        let boot = h.boot;
        assert!(boot.is_some());
        catch_up(&mut h, dongle(1, 200 * 60_000 + 5_000, 130, 0)).unwrap();
        assert_eq!(h.boot, boot);
    }

    #[test]
    fn a_reboot_replaces_the_boot_time() {
        let mut h = History::default();
        catch_up(&mut h, dongle(1, 200 * 60_000, 130, 0)).unwrap();
        let boot = h.boot;
        catch_up(&mut h, dongle(2, 3 * 60_000, 2, 0)).unwrap();
        assert!(h.boot.is_some());
        assert_ne!(h.boot, boot);
    }

    #[test]
    fn a_reboot_starts_the_history_over() {
        let mut h = History::default();
        catch_up(&mut h, dongle(1, 200 * 60_000, 130, 0)).unwrap();
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
        let mut app: App = App::new(None);
        app.history.status = Some(status.clone());
        let mut terminal = Terminal::new(TestBackend::new(80, 24)).unwrap();
        terminal.draw(|f| app.draw(f)).unwrap();
        terminal
    }

    #[test]
    fn the_status_box_holds_a_row_each_for_link_channels_and_dongle() {
        let s = nuphy_status();
        let terminal = status_box(&s);
        let rows = screen(&terminal);
        assert!(rows[0].starts_with("┌OpenDongle · NuPhy"), "{}", rows[0]);
        assert_eq!(
            inside_box(&rows[1]),
            "link      connected on 26   -71 dBm   last packet 12 ms ago"
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
    fn a_unifying_link_row_names_the_keyboard() {
        let s = unifying_status();
        let terminal = status_box(&s);
        let rows = screen(&terminal);
        assert!(rows[0].starts_with("┌OpenDongle · Unifying"), "{}", rows[0]);
        assert!(
            inside_box(&rows[1]).starts_with(&format!(
                "link      connected on {}, keyboard {}   {} dBm",
                s.channel,
                wpid(s.keyboard_id),
                s.last_rssi
            )),
            "{}",
            rows[1]
        );
        assert_eq!(
            inside_box(&rows[2]),
            "channels  5 8 17 32 35 41 44 62 65 71 74"
        );
    }

    #[test]
    fn a_never_paired_nuphy_dongle_says_so_on_the_channels_row() {
        let mut s = nuphy_status();
        s.own = None;
        let terminal = status_box(&s);
        assert_eq!(
            inside_box(&screen(&terminal)[2]),
            "channels  16 22 44 48 58 72 78   never paired"
        );
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
        let terminal = status_box(&nuphy_status());
        assert_eq!(cell_at(&terminal, 2, "26").modifier, Modifier::REVERSED);
        assert_eq!(cell_at(&terminal, 2, "30").modifier, Modifier::BOLD);
        assert_eq!(cell_at(&terminal, 2, "16").modifier, Modifier::empty());
    }

    #[test]
    fn the_link_state_and_signal_are_colored_by_how_good_they_are() {
        let terminal = status_box(&nuphy_status());
        assert_eq!(cell_at(&terminal, 1, "connected").fg, Color::Green);
        assert_eq!(cell_at(&terminal, 1, "-71 dBm").fg, Color::Yellow);
        let mut weak = nuphy_status();
        weak.last_rssi = -80;
        assert_eq!(cell_at(&status_box(&weak), 1, "-80 dBm").fg, Color::Red);
        assert_eq!(cell_at(&terminal, 3, "pairing open").fg, Color::Cyan);
        let mut s = unifying_status();
        s.link_state = 0;
        let terminal = status_box(&s);
        assert_eq!(cell_at(&terminal, 1, "not paired").fg, Color::Red);
    }

    fn strip_names(protocol: Protocol) -> String {
        panel_in(protocol, quiet(), history_min_rows(protocol)).join("\n")
    }

    #[test]
    fn the_unifying_panel_leaves_out_the_strips_its_firmware_never_counts() {
        let nuphy = strip_names(Protocol::Nuphy);
        for name in ["losses", "gaps", "dropped", "channel", "busy"] {
            assert!(nuphy.contains(name), "{name} missing from {nuphy}");
        }
        let unifying = strip_names(Protocol::Unifying);
        for name in ["dropped", "channel", "busy"] {
            assert!(unifying.contains(name), "{name} missing from {unifying}");
        }
        for name in ["losses", "gaps"] {
            assert!(!unifying.contains(name), "{name} shown in {unifying}");
        }
    }
}
