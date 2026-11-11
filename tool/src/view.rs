use std::ops::Range;
use std::time::{Duration, Instant};

use anyhow::Result;
use chrono::{DateTime, Duration as TimeDelta, Local};
use ratatui::{
    DefaultTerminal, Frame,
    crossterm::event::{self, Event as TermEvent, KeyCode, KeyEventKind},
    layout::{Constraint, Layout, Rect},
    style::{Color, Style},
    symbols,
    text::Line,
    widgets::{
        Axis, Bar, BarChart, BarGroup, Block, Chart, Dataset, GraphType, List, ListItem, Paragraph,
        Sparkline,
    },
};

use crate::device::{self, Device, Transport};
use crate::format::{
    ChannelRole, NOISE_QUESTION, action_question, channel_role, describe_event, last_packet,
    link_state_name, noise_level, noise_ruler, paired, uptime, usb_state, wall_clock,
};
use crate::protocol::{Action, Event, Minute, NoiseChannel, Rssi, Status};

const POLL_EVERY: Duration = Duration::from_secs(1);
const EVENTS_KEPT: usize = 200;
const EVENT_ROWS: usize = 8;

type MinuteValue = fn(&Minute) -> u64;

fn dataset<'a>(name: Option<&'a str>, color: Color, data: &'a [(f64, f64)]) -> Dataset<'a> {
    let dataset = Dataset::default()
        .marker(symbols::Marker::Braille)
        .graph_type(GraphType::Line)
        .style(Style::default().fg(color))
        .data(data);
    match name {
        Some(name) => dataset.name(name),
        None => dataset,
    }
}

/// A chart line joins neighboring points, so each minute without packets ends a run.
fn signal_runs(shown: &[(u32, Minute)], pick: fn(&Rssi) -> i8) -> Vec<Vec<(f64, f64)>> {
    let indexed: Vec<_> = shown.iter().enumerate().collect();
    indexed
        .split(|(_, (_, minute))| minute.rssi.is_none())
        .filter(|run| !run.is_empty())
        .map(|run| {
            run.iter()
                .filter_map(|(x, (_, minute))| {
                    minute
                        .rssi
                        .as_ref()
                        .map(|r| (*x as f64, f64::from(pick(r))))
                })
                .collect()
        })
        .collect()
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
    pub(crate) fn refresh<T: Transport>(&mut self, dev: &mut Device<T>) -> Result<()> {
        let s = dev.status()?;
        if self
            .status
            .as_ref()
            .is_some_and(|old| s.uptime_ms < old.uptime_ms)
        {
            self.minutes.clear();
            self.events.clear();
            self.events_top = None;
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
}

impl Prompt {
    fn question(self) -> String {
        let question = match self {
            Prompt::Noise => NOISE_QUESTION,
            Prompt::Action(action) => action_question(action),
        };
        format!("{question} y/n")
    }
}

struct App {
    serial: Option<String>,
    device: Option<Device<device::HidTransport>>,
    history: History,
    noise: Option<Vec<NoiseChannel>>,
    prompt: Option<Prompt>,
    message: String,
    scroll: usize,
    event_rows: usize,
}

pub fn run(serial: Option<String>) -> Result<()> {
    let mut terminal = ratatui::init();
    let mut app = App {
        serial,
        device: None,
        history: History::default(),
        noise: None,
        prompt: None,
        message: String::new(),
        scroll: 0,
        event_rows: EVENT_ROWS,
    };
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
                KeyCode::Char('n') => self.prompt = Some(Prompt::Noise),
                KeyCode::Char('r') => self.prompt = Some(Prompt::Action(Action::Reboot)),
                KeyCode::Char('b') => self.prompt = Some(Prompt::Action(Action::Bootloader)),
                KeyCode::Char('c') => self.prompt = Some(Prompt::Action(Action::Clear)),
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

    fn scroll_events(&mut self, step: EventsMove) {
        self.history.events_top = scrolled_top(
            &self.history.events,
            self.history.events_top,
            self.event_rows,
            step,
        );
    }

    fn poll(&mut self) {
        if self.device.is_none() {
            match device::open(self.serial.as_deref()) {
                Ok(d) => {
                    self.device = Some(d);
                    self.message.clear();
                }
                Err(e) => {
                    self.disconnect(format!("disconnected: {e}"));
                    return;
                }
            }
        }
        let Some(dev) = self.device.as_mut() else {
            return;
        };
        if self.history.refresh(dev).is_err() {
            self.disconnect("disconnected, waiting for the dongle".into());
        }
    }

    fn disconnect(&mut self, message: String) {
        self.device = None;
        self.history = History::default();
        self.message = message;
    }

    fn answer(&mut self, prompt: Prompt) {
        let Some(dev) = self.device.as_mut() else {
            self.message = "disconnected".into();
            return;
        };
        let result = match prompt {
            Prompt::Noise => dev.noise().map(|n| self.noise = Some(n)),
            Prompt::Action(c) => dev.action(c),
        };
        self.message = match result {
            Ok(()) => "done".into(),
            Err(e) => format!("failed: {e}"),
        };
    }

    fn draw(&mut self, f: &mut Frame) {
        let [top, middle, bottom, footer] = Layout::vertical([
            Constraint::Length(4),
            Constraint::Min(9),
            Constraint::Length(EVENT_ROWS as u16 + 2),
            Constraint::Length(1),
        ])
        .areas(f.area());
        f.render_widget(
            Paragraph::new(self.status_lines()).block(Block::bordered().title("OpenDongle")),
            top,
        );
        match &self.noise {
            Some(noise) => self.draw_noise(f, middle, noise),
            None => self.draw_history(f, middle),
        }
        self.draw_events(f, bottom);
        let help = match self.prompt {
            Some(p) => p.question(),
            None if !self.message.is_empty() => self.message.clone(),
            None => "n noise  r reboot  b bootloader  c clear  ←/→ history  ↑/↓ events  esc back  q quit".into(),
        };
        f.render_widget(Paragraph::new(help), footer);
    }

    fn status_lines(&self) -> Vec<Line<'static>> {
        let Some(s) = &self.history.status else {
            return vec![Line::from("disconnected")];
        };
        vec![
            Line::from(format!(
                "{} on {} ({})   signal {} dBm, {}",
                link_state_name(s.link_state),
                s.channel,
                paired(s.own),
                s.last_rssi,
                last_packet(s.last_rx_age_ms)
            )),
            Line::from(format!(
                "usb {}   up {}   losses {}   gaps {}   dropped {}",
                usb_state(s.usb_live, s.usb_suspended),
                uptime(s.uptime_ms),
                s.counters.link_losses,
                s.counters.gaps,
                s.counters.dropped
            )),
        ]
    }

    fn draw_history(&mut self, f: &mut Frame, area: Rect) {
        let [losses_row, gaps_row, dropped_row, signal_row] =
            Layout::vertical([Constraint::Ratio(1, 4); 4]).areas(area);
        let width = area.width.saturating_sub(2) as usize;
        self.scroll = self
            .scroll
            .min(self.history.minutes.len().saturating_sub(width));
        let end = self.history.minutes.len().saturating_sub(self.scroll);
        let shown = &self.history.minutes[end.saturating_sub(width)..end];
        let series: [(&str, Rect, MinuteValue); 3] = [
            ("link losses per minute", losses_row, |m| {
                u64::from(m.link_losses)
            }),
            ("gaps per minute", gaps_row, |m| u64::from(m.gaps)),
            ("dropped packets per minute", dropped_row, |m| {
                u64::from(m.dropped)
            }),
        ];
        for (title, row, value) in series {
            let data: Vec<u64> = shown.iter().map(|(_, m)| value(m)).collect();
            f.render_widget(
                Sparkline::default()
                    .block(Block::bordered().title(title))
                    .data(&data),
                row,
            );
        }
        let bands = [
            ("min", Color::Red, signal_runs(shown, |r| r.min)),
            ("avg", Color::Yellow, signal_runs(shown, |r| r.avg)),
            ("max", Color::Green, signal_runs(shown, |r| r.max)),
        ];
        let datasets: Vec<Dataset> = bands
            .iter()
            .flat_map(|(name, color, runs)| {
                runs.iter()
                    .enumerate()
                    .map(move |(i, run)| dataset((i == 0).then_some(*name), *color, run))
            })
            .collect();
        f.render_widget(
            Chart::new(datasets)
                .block(Block::bordered().title("signal dBm, min/avg/max"))
                .x_axis(Axis::default().bounds([0.0, width.max(1) as f64]))
                .y_axis(Axis::default().bounds([-100.0, -20.0])),
            signal_row,
        );
    }

    fn draw_noise(&self, f: &mut Frame, area: Rect, noise: &[NoiseChannel]) {
        let s = self.history.status.as_ref();
        let bars: Vec<Bar> = noise
            .iter()
            .map(|c| {
                let role = s.map_or(ChannelRole::Other, |s| channel_role(c.channel, s));
                let color = match role {
                    ChannelRole::Current => Color::Green,
                    ChannelRole::Paired => Color::Yellow,
                    ChannelRole::Other => Color::Gray,
                };
                Bar::default()
                    .value(u64::from(noise_level(c.loudest)))
                    .style(Style::default().fg(color))
                    .text_value(String::new())
            })
            .collect();
        let [chart, ruler] =
            Layout::vertical([Constraint::Min(3), Constraint::Length(1)]).areas(area);
        let block = Block::bordered().title(
            "loudest per channel (green current, yellow paired)  ▲ BLE advertising, ├─┤ Wi-Fi 1/6/11",
        );
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
                    ListItem::new(format!("{}  {}", at.format("%H:%M:%S"), describe_event(e)))
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
    use crate::device::tests::{Fake, parts};
    use crate::protocol::EventKind;
    use crate::protocol::tests::fixture;

    fn events(indexes: Range<u32>) -> Vec<(u32, Event)> {
        let event = Event {
            time_ms: 0,
            kind: EventKind::LinkLost,
            a: 0,
            b: 0,
            c: 0,
            value: 0,
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
    fn minutes_without_packets_split_the_signal_into_runs() {
        let with = |avg: i8| Minute {
            packets: 1,
            invalid: 0,
            dropped: 0,
            channel_changes: 0,
            link_losses: 0,
            usb_retries: 0,
            gaps: 0,
            rssi: Some(Rssi {
                min: avg,
                avg,
                max: avg,
            }),
        };
        let without = Minute {
            packets: 0,
            rssi: None,
            ..with(0)
        };
        let shown = [
            (10, with(-60)),
            (11, with(-61)),
            (12, without),
            (13, with(-70)),
        ];
        assert_eq!(
            signal_runs(&shown, |r| r.avg),
            vec![vec![(0.0, -60.0), (1.0, -61.0)], vec![(3.0, -70.0)]]
        );
    }

    fn status_payload(uptime_ms: u64, minute_next: u32, minute_oldest: u32) -> Vec<u8> {
        let mut p = fixture("status.hex");
        p[4..12].copy_from_slice(&uptime_ms.to_le_bytes());
        let n = p.len();
        p[n - 16..n - 12].copy_from_slice(&minute_next.to_le_bytes());
        p[n - 12..n - 8].copy_from_slice(&minute_oldest.to_le_bytes());
        p[n - 8..n - 4].copy_from_slice(&0u32.to_le_bytes());
        p[n - 4..].copy_from_slice(&0u32.to_le_bytes());
        p
    }

    fn minutes_payload(first: u32, count: u8, boot: u8) -> Vec<u8> {
        let mut p = first.to_le_bytes().to_vec();
        p.push(count);
        for _ in 0..count {
            p.extend([boot, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
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
                        let n = next.saturating_sub(from).min(60) as u8;
                        minutes_payload(from, n, boot)
                    }
                    _ => vec![0, 0, 0, 0, 0],
                };
                parts(req[0], req[1], &payload)
            }),
            0,
        )
    }

    #[test]
    fn fetches_all_held_minutes_page_by_page() {
        let mut h = History::default();
        h.refresh(&mut dongle(1, 200 * 60_000, 130, 0)).unwrap();
        assert_eq!(h.minutes.len(), 130);
        assert_eq!(h.minutes.first().map(|m| m.0), Some(0));
        assert_eq!(h.minutes.last().map(|m| m.0), Some(129));
    }

    #[test]
    fn a_clear_drops_minutes_before_the_new_oldest() {
        let mut h = History::default();
        h.refresh(&mut dongle(1, 200 * 60_000, 130, 0)).unwrap();
        h.refresh(&mut dongle(1, 201 * 60_000, 131, 130)).unwrap();
        assert_eq!(h.minutes.iter().map(|m| m.0).collect::<Vec<_>>(), vec![130]);
    }

    #[test]
    fn the_boot_time_holds_across_refreshes() {
        let mut h = History::default();
        h.refresh(&mut dongle(1, 200 * 60_000, 130, 0)).unwrap();
        let boot = h.boot;
        assert!(boot.is_some());
        h.refresh(&mut dongle(1, 200 * 60_000 + 5_000, 130, 0))
            .unwrap();
        assert_eq!(h.boot, boot);
    }

    #[test]
    fn a_reboot_replaces_the_boot_time() {
        let mut h = History::default();
        h.refresh(&mut dongle(1, 200 * 60_000, 130, 0)).unwrap();
        let boot = h.boot;
        h.refresh(&mut dongle(2, 3 * 60_000, 2, 0)).unwrap();
        assert!(h.boot.is_some());
        assert_ne!(h.boot, boot);
    }

    #[test]
    fn a_reboot_starts_the_history_over() {
        let mut h = History::default();
        h.refresh(&mut dongle(1, 200 * 60_000, 130, 0)).unwrap();
        h.events_top = Some(0);
        h.refresh(&mut dongle(2, 3 * 60_000, 2, 0)).unwrap();
        assert_eq!(h.events_top, None);
        assert_eq!(
            h.minutes
                .iter()
                .map(|m| (m.0, m.1.packets))
                .collect::<Vec<_>>(),
            vec![(0, 2), (1, 2)]
        );
    }
}
