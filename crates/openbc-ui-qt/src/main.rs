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
    fn openbc_run_gui() -> i32;
}

fn main() -> Result<()> {
    openbc_observability::init().map_err(anyhow::Error::msg)?;
    #[cfg(feature = "qt")]
    {
        let exit_code = unsafe { openbc_run_gui() };
        anyhow::ensure!(
            exit_code == 0,
            "Qt application exited with code {exit_code}"
        );
        return Ok(());
    }

    #[cfg(not(feature = "qt"))]
    anyhow::bail!("OpenBC was built without Qt support; enable the `qt` feature")
}
