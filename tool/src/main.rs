mod board;
mod cli;
mod device;
mod dfu;
mod format;
mod protocol;
mod uf2;
mod view;

use anyhow::Result;
use clap::{Parser, Subcommand};

use protocol::Action;

#[derive(Parser)]
#[command(version, about = "Flash and inspect the OpenDongle firmware")]
struct Args {
    /// Serial number of the board, when more than one is plugged in
    #[arg(long, global = true)]
    serial: Option<String>,
    #[command(subcommand)]
    command: Option<Cmd>,
}

#[derive(clap::Args)]
struct Confirm {
    /// Skip the confirmation
    #[arg(long)]
    yes: bool,
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
    Noise(Confirm),
    /// Restart the dongle firmware
    Reboot(Confirm),
    /// Reboot into the UF2 bootloader for flashing
    Bootloader(Confirm),
    /// Write a firmware image (.uf2) to the board, restarting it into its bootloader first
    Flash {
        /// The firmware file, for example build/app/zephyr/zephyr.uf2
        file: std::path::PathBuf,
        #[command(flatten)]
        confirm: Confirm,
    },
    /// Zero the counters, history and events
    Clear(Confirm),
}

fn main() -> Result<()> {
    // Rust starts with SIGPIPE ignored, which turns a closed output pipe into a println! panic.
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_DFL);
    }
    let args = Args::parse();
    let Some(command) = args.command else {
        return view::run(args.serial);
    };
    let dev = || device::open(args.serial.as_deref());
    match command {
        Cmd::Flash { file, confirm } => cli::flash(&file, args.serial.as_deref(), confirm.yes),
        Cmd::Status => cli::status(&mut dev()?),
        Cmd::History { minutes } => cli::history(&mut dev()?, minutes),
        Cmd::Events { count } => cli::events(&mut dev()?, count),
        Cmd::Noise(c) => cli::noise(&mut dev()?, c.yes),
        Cmd::Reboot(c) => cli::action(&mut dev()?, Action::Reboot, c.yes),
        Cmd::Bootloader(c) => cli::action(&mut dev()?, Action::Bootloader, c.yes),
        Cmd::Clear(c) => cli::action(&mut dev()?, Action::Clear, c.yes),
    }
}
