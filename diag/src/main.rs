mod cli;
mod device;
mod format;
mod protocol;

use anyhow::Result;
use clap::{Parser, Subcommand};

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
}

fn main() -> Result<()> {
    let args = Args::parse();
    let mut dev = device::open(args.serial.as_deref())?;
    match args.command {
        Cmd::Status => cli::status(&mut dev),
        Cmd::History { minutes } => cli::history(&mut dev, minutes),
        Cmd::Events { count } => cli::events(&mut dev, count),
    }
}
