use anyhow::{bail, Context, Result};
use std::process::Command;

fn run(command: &str, args: &[&str]) -> Result<()> {
    let status = Command::new(command)
        .args(args)
        .status()
        .with_context(|| format!("failed to start {command}"))?;
    if !status.success() {
        bail!("{command} {:?} exited with {status}", args);
    }
    Ok(())
}

fn main() -> Result<()> {
    match std::env::args().nth(1).as_deref() {
        Some("setup") => {
            println!("Dependencies are installed by ./setup.sh; validating the workspace.");
            run("cargo", &["check", "--workspace"])
        }
        Some("build") => run("cargo", &["build", "--workspace"]),
        Some("run") => run("cargo", &["run", "-p", "openbc-ui-qt"]),
        Some(command) => bail!("unknown xtask command: {command}; use setup, build, or run"),
        None => bail!("missing command; use setup, build, or run"),
    }
}
