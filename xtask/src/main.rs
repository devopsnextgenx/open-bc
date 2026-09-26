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

fn cmake_build() -> Result<()> {
    run(
        "cmake",
        &["-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"],
    )?;
    run("cmake", &["--build", "build", "--target", "openbc"])
}

fn cmake_executable() -> &'static str {
    if cfg!(target_os = "windows") {
        "build/openbc.exe"
    } else {
        "build/openbc"
    }
}

fn main() -> Result<()> {
    match std::env::args().nth(1).as_deref() {
        Some("setup") => {
            println!("Dependencies are installed by ./setup.sh; validating the workspace.");
            run("cargo", &["check", "--workspace"])
        }
        Some("build") => cmake_build(),
        Some("run") => run(cmake_executable(), &[]),
        Some(command) => bail!("unknown xtask command: {command}; use setup, build, or run"),
        None => bail!("missing command; use setup, build, or run"),
    }
}
