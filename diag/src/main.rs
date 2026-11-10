mod cli;
mod device;
mod format;
mod protocol;

use anyhow::Result;
use clap::{Parser, Subcommand};

use protocol::Action;

#[derive(Parser)]
#[command(version, about = "Diagnostics for the nuphy-dongle firmware")]
struct Args {
    /// Serial number of the dongle, when more than one is plugged in
    #[arg(long, global = true)]
    serial: Option<String>,
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Link, signal, USB state and counter totals
    Status,
    /// Per-minute counters and signal, oldest first
    History {
        /// How many minutes back to show
        #[arg(long, default_value_t = 60)]
        minutes: u32,
    },
    /// The latest events, oldest first
    Events {
        /// How many events to show
        #[arg(long, default_value_t = 50)]
        count: u32,
    },
    /// Measure the radio noise on every channel
    Noise {
        #[arg(long)]
        yes: bool,
    },
    /// Restart the dongle firmware
    Reboot {
        #[arg(long)]
        yes: bool,
    },
    /// Reboot into the UF2 bootloader for flashing
    Bootloader {
        #[arg(long)]
        yes: bool,
    },
    /// Zero the counters, history and events
    Clear {
        #[arg(long)]
        yes: bool,
    },
}

fn main() -> Result<()> {
    let args = Args::parse();
    let mut dev = device::open(args.serial.as_deref())?;
    match args.command {
        Cmd::Status => cli::status(&mut dev),
        Cmd::History { minutes } => cli::history(&mut dev, minutes),
        Cmd::Events { count } => cli::events(&mut dev, count),
        Cmd::Noise { yes } => cli::noise(&mut dev, yes),
        Cmd::Reboot { yes } => cli::action(&mut dev, Action::Reboot, yes),
        Cmd::Bootloader { yes } => cli::action(&mut dev, Action::Bootloader, yes),
        Cmd::Clear { yes } => cli::action(&mut dev, Action::Clear, yes),
    }
}
