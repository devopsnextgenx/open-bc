//! Qt edge adapter and initial desktop shell.

use anyhow::Result;
use openbc_engine::ScanEvent;
use std::collections::VecDeque;
use tokio::sync::mpsc;

/// Thread-safe handoff owned by the Qt integration layer.
pub struct ScanEventBridge {
    receiver: mpsc::Receiver<ScanEvent>,
    pending: VecDeque<ScanEvent>,
}

impl ScanEventBridge {
    /// Create a bridge from the engine's bounded event receiver.
    pub fn new(receiver: mpsc::Receiver<ScanEvent>) -> Self {
        Self {
            receiver,
            pending: VecDeque::new(),
        }
    }

    /// Drain currently available events; invoke this from a Qt timer or queued slot.
    pub fn drain_ready(&mut self) -> Vec<ScanEvent> {
        while let Ok(event) = self.receiver.try_recv() {
            self.pending.push_back(event);
        }
        self.pending.drain(..).collect()
    }
}

#[cfg(feature = "qt")]
unsafe extern "C" {
    fn openbc_run_gui(argc: i32, argv: *const *const std::os::raw::c_char) -> i32;
}

fn main() -> Result<()> {
    openbc_observability::init().map_err(anyhow::Error::msg)?;
    #[cfg(feature = "qt")]
    {
        // Forward the real command line (paths, --compare, --dependency-check ...) to the Qt side.
        let args: Vec<std::ffi::CString> = std::env::args_os()
            .map(|a| std::ffi::CString::new(a.to_string_lossy().as_bytes()).unwrap_or_default())
            .collect();
        let mut argv: Vec<*const std::os::raw::c_char> = args.iter().map(|a| a.as_ptr()).collect();
        argv.push(std::ptr::null());
        let exit_code = unsafe { openbc_run_gui((argv.len() - 1) as i32, argv.as_ptr()) };
        anyhow::ensure!(
            exit_code == 0,
            "Qt application exited with code {exit_code}"
        );
        return Ok(());
    }

    #[cfg(not(feature = "qt"))]
    anyhow::bail!("OpenBC was built without Qt support; enable the `qt` feature")
}
