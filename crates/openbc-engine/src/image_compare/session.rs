use std::sync::{Arc, OnceLock, RwLock};

use openbc_compute::image::{
    CancelToken, CompareDone, DecodedImage, ImageCompute, ImageComputeError, PairDecode,
};
use openbc_core::image_compare::{
    AlignedPair, ImageCompareOptions, PixelProbe, RgbaImage, Verdict,
};
use tokio::runtime::Handle;
use tokio::sync::{mpsc, watch};
use tokio::task::JoinHandle;

use crate::common::COMMAND_CHANNEL_CAPACITY;

use super::events::{ImageCompareEvent, ImageFileInfo, Side};
use super::source::{ImageByteSource, SourceRead};

static COMPUTE: OnceLock<Arc<ImageCompute>> = OnceLock::new();

/// Call once at startup (before the first session spawns) to install a
/// configured `ImageCompute` (explicit Rayon pool, GPU executor). Returns
/// false if one was already installed or lazily created. Keeps the GPU/Rayon
/// backend type out of caller crates (e.g. `openbc-ui-qt`).
pub fn install_image_compute(compute: Arc<ImageCompute>) -> bool {
    COMPUTE.set(compute).is_ok()
}

fn compute() -> Arc<ImageCompute> {
    COMPUTE.get_or_init(|| Arc::new(ImageCompute::new())).clone()
}

#[derive(Clone, Copy, Debug)]
pub struct ImageCompareConfig {
    /// Largest single image file accepted (bytes).
    pub max_file_bytes: u64,
    /// Pending-command queue depth (commands are rare; options use a watch channel).
    pub command_queue: usize,
}

impl Default for ImageCompareConfig {
    fn default() -> Self {
        Self { max_file_bytes: 512 << 20, command_queue: COMMAND_CHANNEL_CAPACITY }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HandleError {
    /// Command queue is full; retry.
    Busy,
    /// The session has shut down.
    Closed,
}

enum Command {
    Open { left: String, right: String },
    Reload,
    Swap,
    Close,
}

/// Non-blocking control surface, safe to call from the GUI thread.
#[derive(Clone)]
pub struct ImageCompareHandle {
    cmd_tx: mpsc::Sender<Command>,
    options: Arc<watch::Sender<ImageCompareOptions>>,
    probe_slot: Arc<RwLock<Option<Arc<AlignedPair>>>>,
}

impl ImageCompareHandle {
    fn send(&self, c: Command) -> Result<(), HandleError> {
        self.cmd_tx.try_send(c).map_err(|e| match e {
            mpsc::error::TrySendError::Full(_) => HandleError::Busy,
            mpsc::error::TrySendError::Closed(_) => HandleError::Closed,
        })
    }

    /// Load both files and compare with the current options. Both sides are
    /// reloaded so the binary-identical check stays exact.
    pub fn open(&self, left: impl Into<String>, right: impl Into<String>) -> Result<(), HandleError> {
        self.send(Command::Open { left: left.into(), right: right.into() })
    }
    pub fn reload(&self) -> Result<(), HandleError> {
        self.send(Command::Reload)
    }
    /// Swap sides without re-decoding; the offset is negated to match.
    pub fn swap(&self) -> Result<(), HandleError> {
        self.send(Command::Swap)
    }
    pub fn close(&self) {
        let _ = self.cmd_tx.try_send(Command::Close);
    }

    /// Latest-wins; never blocks. Re-runs only the compare phase.
    pub fn set_options(&self, options: ImageCompareOptions) {
        let options = options.sanitized();
        let _ = self.options.send_if_modified(|cur| {
            if *cur == options {
                false
            } else {
                *cur = options;
                true
            }
        });
    }
    pub fn options(&self) -> ImageCompareOptions {
        *self.options.borrow()
    }

    /// 9x9 neighbourhood at a canvas coordinate from the newest finished compare.
    /// A short read-lock plus 162 pixel reads; fine on the GUI thread.
    pub fn probe(&self, x: i32, y: i32) -> Option<PixelProbe> {
        let guard = self.probe_slot.read().ok()?;
        guard.as_ref().map(|pair| pair.probe(x, y))
    }
}

/// Start a session on `runtime`. Events are delivered through `events`; because
/// it is bounded, a slow consumer pauses the session instead of growing memory.
pub fn spawn_image_compare(
    runtime: &Handle,
    source: Arc<dyn ImageByteSource>,
    events: mpsc::Sender<ImageCompareEvent>,
    initial: ImageCompareOptions,
    config: ImageCompareConfig,
) -> ImageCompareHandle {
    let (cmd_tx, cmd_rx) = mpsc::channel(config.command_queue.max(1));
    let (opts_tx, opts_rx) = watch::channel(initial.sanitized());
    let opts_tx = Arc::new(opts_tx);
    let probe_slot = Arc::new(RwLock::new(None));

    let actor = Actor {
        compute: compute(),
        source,
        events,
        config,
        cmd_rx,
        opts_tx: opts_tx.clone(),
        opts_rx,
        probe_slot: probe_slot.clone(),
        seq: 0,
        cancel: CancelToken::new(),
        paths: (String::new(), String::new()),
        loaded: None,
        inflight: None,
    };
    runtime.spawn(actor.run());
    ImageCompareHandle { cmd_tx, options: opts_tx, probe_slot }
}

struct LoadedSide {
    info: ImageFileInfo,
    image: Arc<RgbaImage>,
}

struct LoadedPair {
    left: LoadedSide,
    right: LoadedSide,
    binary_identical: bool,
}

enum LoadOutcome {
    Ready(LoadedPair),
    Failed { left: Option<String>, right: Option<String> },
}

enum Stage {
    Loaded { seq: u64, outcome: LoadOutcome },
    Compared { seq: u64, result: Result<CompareDone, ImageComputeError> },
}

struct Actor {
    compute: Arc<ImageCompute>,
    source: Arc<dyn ImageByteSource>,
    events: mpsc::Sender<ImageCompareEvent>,
    config: ImageCompareConfig,
    cmd_rx: mpsc::Receiver<Command>,
    opts_tx: Arc<watch::Sender<ImageCompareOptions>>,
    opts_rx: watch::Receiver<ImageCompareOptions>,
    probe_slot: Arc<RwLock<Option<Arc<AlignedPair>>>>,
    seq: u64,
    cancel: CancelToken,
    paths: (String, String),
    loaded: Option<LoadedPair>,
    inflight: Option<JoinHandle<Stage>>,
}

/// Event channel closed: the consumer is gone, so the session ends (fatal by design).
type Fatal = ();

async fn wait_stage(slot: &mut Option<JoinHandle<Stage>>) -> Option<Stage> {
    match slot.as_mut() {
        Some(handle) => handle.await.ok(),
        None => std::future::pending().await,
    }
}

impl Actor {
    async fn run(mut self) {
        loop {
            let keep_going = tokio::select! {
                cmd = self.cmd_rx.recv() => match cmd {
                    None | Some(Command::Close) => false,
                    Some(c) => self.on_command(c).await.is_ok(),
                },
                changed = self.opts_rx.changed() => {
                    changed.is_ok() && self.on_options_changed().await.is_ok()
                },
                stage = wait_stage(&mut self.inflight), if self.inflight.is_some() => {
                    self.inflight = None;
                    self.on_stage(stage).await.is_ok()
                },
            };
            if !keep_going {
                break;
            }
        }
        self.cancel.cancel();
        if let Some(task) = self.inflight.take() {
            task.abort();
        }
    }

    async fn emit(&self, event: ImageCompareEvent) -> Result<(), Fatal> {
        self.events.send(event).await.map_err(|_| ())
    }

    fn supersede(&mut self) -> u64 {
        self.cancel.cancel();
        self.cancel = CancelToken::new();
        if let Some(task) = self.inflight.take() {
            task.abort();
        }
        self.seq += 1;
        self.seq
    }

    async fn on_command(&mut self, command: Command) -> Result<(), Fatal> {
        match command {
            Command::Open { left, right } => {
                self.paths = (left, right);
                self.start_load().await
            }
            Command::Reload => self.start_load().await,
            Command::Swap => {
                std::mem::swap(&mut self.paths.0, &mut self.paths.1);
                match self.loaded.as_mut() {
                    Some(pair) => {
                        std::mem::swap(&mut pair.left, &mut pair.right);
                        self.opts_tx.send_modify(|o| o.offset = o.offset.negated());
                        let _ = self.opts_rx.borrow_and_update(); // handled here, not as a separate change
                        self.start_compare().await
                    }
                    None => self.start_load().await,
                }
            }
            Command::Close => Ok(()),
        }
    }

    async fn on_options_changed(&mut self) -> Result<(), Fatal> {
        let _ = self.opts_rx.borrow_and_update();
        self.start_compare().await
    }

    async fn start_load(&mut self) -> Result<(), Fatal> {
        let seq = self.supersede();
        self.loaded = None;
        self.emit(ImageCompareEvent::Loading { seq }).await?;

        let (compute, source, max) = (self.compute.clone(), self.source.clone(), self.config.max_file_bytes);
        let (left_path, right_path) = self.paths.clone();
        self.inflight = Some(tokio::spawn(async move {
            let (l, r) = tokio::join!(source.read(&left_path, max), source.read(&right_path, max));
            Stage::Loaded { seq, outcome: load_pair(compute, left_path, right_path, l, r).await }
        }));
        Ok(())
    }

    async fn start_compare(&mut self) -> Result<(), Fatal> {
        let Some(pair) = self.loaded.as_ref() else { return Ok(()) };
        let (left, right) = (pair.left.image.clone(), pair.right.image.clone());
        let options = *self.opts_rx.borrow();
        let seq = self.supersede();
        let rx = self.compute.compare_async(left, right, options, self.cancel.clone());
        self.inflight = Some(tokio::spawn(async move {
            let result = rx.await.unwrap_or(Err(ImageComputeError::WorkerDropped));
            Stage::Compared { seq, result }
        }));
        Ok(())
    }

    async fn on_stage(&mut self, stage: Option<Stage>) -> Result<(), Fatal> {
        let Some(stage) = stage else {
            return self.emit(ImageCompareEvent::Failed { seq: self.seq, message: "worker task failed".into() }).await;
        };
        match stage {
            Stage::Loaded { seq, .. } | Stage::Compared { seq, .. } if seq != self.seq => Ok(()),
            Stage::Loaded { seq, outcome: LoadOutcome::Ready(pair) } => {
                let event = ImageCompareEvent::Loaded {
                    seq,
                    left: pair.left.info.clone(),
                    right: pair.right.info.clone(),
                    left_image: pair.left.image.clone(),
                    right_image: pair.right.image.clone(),
                    binary_identical: pair.binary_identical,
                };
                self.loaded = Some(pair);
                self.emit(event).await?;
                self.start_compare().await
            }
            Stage::Loaded { seq, outcome: LoadOutcome::Failed { left, right } } => {
                self.clear_probe();
                let failures = [(Side::Left, self.paths.0.clone(), left), (Side::Right, self.paths.1.clone(), right)];
                for (side, path, message) in failures {
                    if let Some(message) = message {
                        self.emit(ImageCompareEvent::PathError { seq, side, path, message }).await?;
                    }
                }
                Ok(())
            }
            Stage::Compared { seq, result: Ok(done) } => {
                let binary_identical = self.loaded.as_ref().is_some_and(|p| p.binary_identical);
                let mut output = done.output;
                if binary_identical {
                    output.verdict = Verdict::BinarySame;
                }
                if let Ok(mut slot) = self.probe_slot.write() {
                    *slot = Some(done.pair);
                }
                self.emit(ImageCompareEvent::Result { seq, output, binary_identical }).await
            }
            Stage::Compared { result: Err(ImageComputeError::Cancelled), .. } => Ok(()),
            Stage::Compared { seq, result: Err(e) } => {
                self.emit(ImageCompareEvent::Failed { seq, message: e.to_string() }).await
            }
        }
    }

    fn clear_probe(&self) {
        if let Ok(mut slot) = self.probe_slot.write() {
            *slot = None;
        }
    }
}

fn loaded_side(path: String, size_bytes: u64, modified_unix_ms: Option<i64>, d: &DecodedImage) -> LoadedSide {
    LoadedSide {
        info: ImageFileInfo {
            path,
            size_bytes,
            modified_unix_ms,
            format: d.meta.format.clone(),
            width: d.image.width(),
            height: d.image.height(),
            bits_per_pixel: d.meta.bits_per_pixel,
        },
        image: d.image.clone(),
    }
}

async fn load_pair(
    compute: Arc<ImageCompute>,
    left_path: String,
    right_path: String,
    left: std::io::Result<SourceRead>,
    right: std::io::Result<SourceRead>,
) -> LoadOutcome {
    let (l, r) = match (left, right) {
        (Ok(l), Ok(r)) => (l, r),
        (l, r) => return failure(l, r),
    };
    let SourceRead { bytes: left_bytes, size_bytes: left_size, modified_unix_ms: left_mtime } = l;
    let SourceRead { bytes: right_bytes, size_bytes: right_size, modified_unix_ms: right_mtime } = r;

    // CPU-bound: byte compare + decode run on Rayon, never on this Tokio task.
    let Ok(PairDecode { left: dl, right: dr, bytes_identical }) =
        compute.decode_pair_async(left_bytes, right_bytes).await
    else {
        let msg = ImageComputeError::WorkerDropped.to_string();
        return LoadOutcome::Failed { left: Some(msg.clone()), right: Some(msg) };
    };
    match (dl, dr) {
        (Ok(l), Ok(r)) => LoadOutcome::Ready(LoadedPair {
            left: loaded_side(left_path, left_size, left_mtime, &l),
            right: loaded_side(right_path, right_size, right_mtime, &r),
            binary_identical: bytes_identical,
        }),
        (l, r) => failure(l, r),
    }
}

/// Reports whichever sides of a load/decode pair failed.
fn failure<A, B>(left: Result<A, impl std::fmt::Display>, right: Result<B, impl std::fmt::Display>) -> LoadOutcome {
    LoadOutcome::Failed {
        left: left.err().map(|e| e.to_string()),
        right: right.err().map(|e| e.to_string()),
    }
}
