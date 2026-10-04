//! One actor per 3D Compare tab (same shape as `image_compare`).
//!
//! * file bytes arrive through `ModelByteSource` (local FS now, `AsyncVfs` adapters later),
//!   both sides are read concurrently on Tokio;
//! * parsing and every compare stage run on Rayon (`openbc_compute::model3d::Session`), never
//!   on a Tokio worker; the actor only awaits a oneshot;
//! * option changes travel on a latest-wins `watch` channel; a newer value cancels the pass in
//!   flight (cooperatively) and the actor restarts from the earliest stale stage - the Session
//!   caches everything that is still valid, so a tolerance tick re-runs only the analysis;
//! * `Model3dEvent`s flow to the UI over a bounded `mpsc` channel (back-pressure by design).
//!
//! The UI never depends on `openbc-compute`: everything it needs (`AlignCommand`, the opaque
//! `Model3dEnv` carrying backends + format registry) is exposed from this module.
use openbc_compute::model3d::{builtin_registry, Backends, ComputeError, ProbeData, RawModel, Session};
pub use openbc_compute::model3d::AlignCommand;
use openbc_core::model3d::mesh::{add, mul, sub};
use openbc_core::model3d::*;
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, OnceLock, RwLock};
use tokio::sync::{mpsc, oneshot, watch};

pub const EVENT_CAPACITY: usize = 4;

/// Process-wide compute environment (format registry + CPU/GPU backends). Opaque to the UI:
/// it never has to name a compute type.
#[derive(Clone)]
pub struct Model3dEnv {
    backends: Arc<Backends>,
    registry: Arc<LoaderRegistry>,
}

impl Model3dEnv {
    /// Shared instance. GPU bring-up is expensive and process-wide, so it happens once.
    pub fn shared() -> Self {
        static S: OnceLock<Model3dEnv> = OnceLock::new();
        S.get_or_init(|| Model3dEnv { backends: Arc::new(Backends::detect()), registry: Arc::new(builtin_registry()) }).clone()
    }

    /// CPU-only environment (tests, headless embedders).
    pub fn cpu_only() -> Self {
        Self { backends: Arc::new(Backends::cpu_only()), registry: Arc::new(builtin_registry()) }
    }

    /// Name of the GPU adapter in use, `None` when running on the CPU only.
    pub fn gpu_name(&self) -> Option<String> { self.backends.gpu_name() }

    /// Registered file extensions, ';'-separated ("stl;obj;ply;...").
    pub fn extensions(&self) -> String { self.registry.extensions().join(";") }
}

pub type ByteFuture<'a> = Pin<Box<dyn Future<Output = Result<RawModel, String>> + Send + 'a>>;
pub trait ModelByteSource: Send + Sync {
    fn read<'a>(&'a self, path: &'a str) -> ByteFuture<'a>;
}

/// Local file system source with a per-file size cap (rejected per path, other side unaffected).
pub struct LocalFs { pub max_file_bytes: u64 }
impl ModelByteSource for LocalFs {
    fn read<'a>(&'a self, path: &'a str) -> ByteFuture<'a> {
        Box::pin(async move {
            let meta = tokio::fs::metadata(path).await.map_err(|e| format!("Cannot read file: {e}"))?;
            if meta.len() > self.max_file_bytes { return Err(format!("File is {} bytes, limit is {}", meta.len(), self.max_file_bytes)); }
            let bytes = tokio::fs::read(path).await.map_err(|e| format!("Cannot read file: {e}"))?;
            let modified = meta.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_millis() as i64);
            Ok(RawModel { info: FileInfo { path: path.to_string(), format: String::new(), size_bytes: meta.len(), modified_unix_ms: modified }, bytes })
        })
    }
}

#[derive(Debug)]
pub enum Model3dEvent {
    Loading,
    /// A side that failed is `None` (a `PathError` for it was sent just before).
    Loaded { left: Option<LoadedSidePayload>, right: Option<LoadedSidePayload> },
    Result(Box<Model3dOutput>),
    PathError { side: u8, path: String, message: String },
    Notice(String),
    Failed(String),
}

enum Command { Open(String, String), Reload, Swap, Align(AlignCommand), Shutdown }

#[derive(Clone, Copy, Debug)]
pub struct ProbeHit {
    /// Display space (includes the display shift).
    pub point: V3,
    /// Signed deviation of the picked surface point to the other model.
    pub deviation: f32,
    pub side: u8,
    pub triangle: u32,
}

pub struct Model3dCompareHandle {
    cmd: mpsc::Sender<Command>,
    opts: Arc<watch::Sender<Model3dCompareOptions>>,
    probe: Arc<RwLock<Option<Arc<ProbeData>>>>,
    task: Option<tokio::task::JoinHandle<()>>,
}

pub struct Model3dCompareConfig {
    pub env: Model3dEnv,
    pub source: Arc<dyn ModelByteSource>,
}

pub fn spawn(rt: &tokio::runtime::Handle, cfg: Model3dCompareConfig, events: mpsc::Sender<Model3dEvent>, initial: Model3dCompareOptions) -> Model3dCompareHandle {
    let (cmd_tx, cmd_rx) = mpsc::channel(16);
    let opts = Arc::new(watch::channel(initial.sanitized()).0);
    let probe = Arc::new(RwLock::new(None));
    let actor = Actor {
        session: Some(Session::new(cfg.env.registry, cfg.env.backends)), source: cfg.source, paths: (String::new(), String::new()),
        opts_tx: opts.clone(), opts_rx: opts.subscribe(), cmd_rx, events, probe: probe.clone(),
    };
    let task = rt.spawn(actor.run());
    Model3dCompareHandle { cmd: cmd_tx, opts, probe, task: Some(task) }
}

impl Model3dCompareHandle {
    /// Non-blocking, latest wins.
    pub fn set_options(&self, o: Model3dCompareOptions) { self.opts.send_replace(o.sanitized()); }
    pub fn open(&self, left: String, right: String) { let _ = self.cmd.try_send(Command::Open(left, right)); }
    pub fn reload(&self) { let _ = self.cmd.try_send(Command::Reload); }
    pub fn swap(&self) { let _ = self.cmd.try_send(Command::Swap); }
    pub fn align(&self, c: AlignCommand) { let _ = self.cmd.try_send(Command::Align(c)); }

    /// Synchronous click / hover probe against the newest finished compare. `dir` must be
    /// normalised; `origin`/`dir` are in display space. Never touches the pipeline.
    pub fn probe(&self, side: u8, origin: V3, dir: V3) -> Option<ProbeHit> {
        let snap = self.probe.read().ok()?.clone()?;
        let o = sub(origin, snap.display_shift);
        let (hit_bvh, other) = if side == 0 { (&snap.a, &snap.b) } else { (&snap.b, &snap.a) };
        let (t, tri) = hit_bvh.raycast(o, dir)?;
        let p = add(o, mul(dir, t));
        let deviation = other.closest(p).map(|h| h.signed).unwrap_or(f32::INFINITY);
        Some(ProbeHit { point: add(p, snap.display_shift), deviation, side, triangle: tri })
    }

    pub async fn shutdown(&mut self) {
        let _ = self.cmd.send(Command::Shutdown).await;
        if let Some(t) = self.task.take() { let _ = t.await; }
    }
}

struct Actor {
    session: Option<Session>,
    source: Arc<dyn ModelByteSource>,
    paths: (String, String),
    opts_tx: Arc<watch::Sender<Model3dCompareOptions>>,
    opts_rx: watch::Receiver<Model3dCompareOptions>,
    cmd_rx: mpsc::Receiver<Command>,
    events: mpsc::Sender<Model3dEvent>,
    probe: Arc<RwLock<Option<Arc<ProbeData>>>>,
}

impl Actor {
    async fn send(&self, e: Model3dEvent) -> bool { self.events.send(e).await.is_ok() }

    async fn run(mut self) {
        loop {
            tokio::select! {
                biased;
                cmd = self.cmd_rx.recv() => {
                    let Some(cmd) = cmd else { break };
                    match cmd {
                        Command::Shutdown => break,
                        Command::Open(l, r) => { self.paths = (l, r); if !self.load().await { break; } if !self.compare(None).await { break; } }
                        Command::Reload => { if !self.load().await { break; } if !self.compare(None).await { break; } }
                        Command::Swap => {
                            std::mem::swap(&mut self.paths.0, &mut self.paths.1);
                            if let Some(s) = self.session.as_mut() { s.swap(); }
                            // Local (un-aligned) meshes change sides, so resend both payloads.
                            let (left, right) = self.session.as_ref().map(|s| s.payloads()).unwrap_or((None, None));
                            if !self.send(Model3dEvent::Loaded { left, right }).await { break; }
                            self.opts_tx.send_modify(|o| for v in &mut o.offset { *v = -*v; });
                            if !self.compare(None).await { break; }
                        }
                        Command::Align(c) => { if !self.compare(Some(c)).await { break; } }
                    }
                }
                changed = self.opts_rx.changed() => {
                    if changed.is_err() { break; }
                    if !self.compare(None).await { break; }
                }
            }
        }
    }

    /// Returns false when the UI side is gone.
    async fn load(&mut self) -> bool {
        if self.paths.0.trim().is_empty() || self.paths.1.trim().is_empty() { return true; }
        if !self.send(Model3dEvent::Loading).await { return false; }
        let (l, r) = tokio::join!(self.source.read(&self.paths.0), self.source.read(&self.paths.1));
        let mut session = self.session.take().expect("session present");
        let (tx, rx) = oneshot::channel();
        rayon::spawn(move || {
            let outcome = session.load_pair(l, r, &CancelToken::new());
            let _ = tx.send((session, outcome));
        });
        let Ok((session, outcome)) = rx.await else { return false };
        self.session = Some(session);
        let mut payload = [None, None];
        for (i, res) in [outcome.left, outcome.right].into_iter().enumerate() {
            match res {
                Ok(s) => payload[i] = Some(s.payload()),
                Err(message) => {
                    let path = if i == 0 { self.paths.0.clone() } else { self.paths.1.clone() };
                    if !self.send(Model3dEvent::PathError { side: i as u8, path, message }).await { return false; }
                }
            }
        }
        let [left, right] = payload;
        self.send(Model3dEvent::Loaded { left, right }).await
    }

    /// Runs passes until one finishes un-superseded.
    async fn compare(&mut self, cmd: Option<AlignCommand>) -> bool {
        if !self.session.as_ref().map(|s| s.is_ready()).unwrap_or(false) { return true; }
        loop {
            let opts = *self.opts_rx.borrow_and_update();
            let cancel = CancelToken::new();
            let mut session = self.session.take().expect("session present");
            let (tx, rx) = oneshot::channel();
            let c2 = cancel.clone();
            rayon::spawn(move || {
                let r = session.compare(&opts, cmd, &c2);
                let _ = tx.send((session, r));
            });
            tokio::pin!(rx);
            tokio::select! {
                res = &mut rx => {
                    let Ok((session, r)) = res else { return false };
                    *self.probe.write().unwrap() = session.probe_data();
                    self.session = Some(session);
                    return match r {
                        Ok(out) => {
                            let notice = out.notice.clone();
                            let ok = self.send(Model3dEvent::Result(Box::new(out))).await;
                            if let (true, Some(n)) = (ok, notice) { self.send(Model3dEvent::Notice(n)).await } else { ok }
                        }
                        Err(ComputeError::Cancelled) => true,
                        Err(e) => self.send(Model3dEvent::Failed(e.to_string())).await,
                    };
                }
                _ = self.opts_rx.changed() => {
                    // Newer options: cancel, wait for the worker to hand the session back, restart.
                    cancel.cancel();
                    let Ok((session, _)) = (&mut rx).await else { return false };
                    self.session = Some(session);
                }
            }
        }
    }
}