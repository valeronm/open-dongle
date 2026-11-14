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

use protocol::{Action, Protocol};

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

#[derive(Clone, Copy, clap::ValueEnum)]
enum ProtocolName {
    Nuphy,
    Unifying,
}

impl From<ProtocolName> for Protocol {
    fn from(name: ProtocolName) -> Self {
        match name {
            ProtocolName::Nuphy => Protocol::Nuphy,
            ProtocolName::Unifying => Protocol::Unifying,
        }
    }
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
    /// Set the three channels every pairing offers, or `auto` to pick them by survey
    Channels {
        /// Three channels from 2 to 80, for example 62 52 44, or auto
        #[arg(required = true, num_args = 1..=3)]
        channels: Vec<String>,
    },
    /// Show the protocol the dongle runs, or switch it; the dongle reboots into the new one
    Protocol {
        #[arg(value_enum)]
        name: Option<ProtocolName>,
    },
    /// Open pairing for 30 s
    Pair,
    /// Move the link to one of the keyboard's channels until the dongle moves it again or reboots
    Move {
        /// One of the keyboard's channels
        channel: u8,
    },
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
    let serial = args.serial.as_deref();
    match command {
        Cmd::Flash { file, confirm } => cli::flash(&file, serial, confirm.yes),
        Cmd::Status => cli::status(&device::open(serial)?.1),
        Cmd::History { minutes } => {
            let (mut dev, s) = device::open(serial)?;
            cli::history(&mut dev, &s, minutes)
        }
        Cmd::Events { count } => {
            let (mut dev, s) = device::open(serial)?;
            cli::events(&mut dev, &s, count)
        }
        Cmd::Noise(c) => {
            let (mut dev, s) = device::open(serial)?;
            cli::noise(&mut dev, &s, c.yes)
        }
        Cmd::Channels { channels } => {
            let forced = cli::parse_channels(&channels)?;
            cli::channels(&mut cli::open_nuphy(serial, "forced channels")?.0, forced)
        }
        Cmd::Protocol { name } => cli::protocol(serial, name.map(Into::into)),
        Cmd::Pair => {
            let (mut dev, s) = device::open(serial)?;
            cli::pair(&mut dev, s.protocol)
        }
        Cmd::Move { channel } => {
            let (mut dev, s) = device::open(serial)?;
            println!("{}", cli::move_link(&mut dev, &s, channel)?);
            Ok(())
        }
        Cmd::Reboot(c) => cli::action(&mut device::open(serial)?.0, Action::Reboot, c.yes),
        Cmd::Bootloader(c) => cli::action(
            &mut device::open_unchecked(serial)?,
            Action::Bootloader,
            c.yes,
        ),
        Cmd::Clear(c) => cli::action(&mut device::open(serial)?.0, Action::Clear, c.yes),
    }
}
