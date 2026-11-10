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
}

fn main() -> Result<()> {
    let args = Args::parse();
    let mut dev = device::open(args.serial.as_deref())?;
    match args.command {
        Cmd::Status => cli::status(&mut dev),
    }
}
