//! Per-tab "is what I loaded still current?" tracking.
//!
//! Division of labour (see also `ARCHITECTURE` notes in docs/DROP_AND_RELOAD.md):
//! * `openbc-vfs::observe` watches resources and publishes *observations* (shared, deduplicated).
//! * [`SideTrack`] (pure, no I/O) holds the version a comparison side loaded and decides, per new
//!   observation, whether that side is stale, fresh again, or just unavailable.
//! * The actor started by [`spawn_tab_watch`] wires N subscriptions of one tab to N `SideTrack`s
//!   and emits typed, tab-aware [`ResourceEvent`]s. It never touches loaded content: replacing
//!   it is the UI's explicit `Rebase` (reload) decision.
//!
//! Reload protocol (race-safe by construction): the UI sends `Rebase` BEFORE it re-reads the
//! file. The baseline is therefore a version that is no newer than the bytes read; if the file
//! changes again while reading, the next observation differs from the baseline and the side
//! goes stale again instead of being marked as current.

use std::path::PathBuf;
use std::sync::Arc;

use openbc_core::resource_version::{assess, Assessment, ChangeKind, Observed, ResourceVersion};
use openbc_core::EntryPath;
use openbc_vfs::observe::{Subscription, WatchHub};
use openbc_vfs::AsyncVfs;
use tokio::runtime::Handle;
use tokio::sync::mpsc;

use crate::common::*;

/// Identifies the tab an event belongs to (assigned by the UI layer).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TabId(pub u64);

/// What to watch for one side of a comparison.
pub enum ResourceTarget {
    LocalFile(PathBuf),
    LocalDir(PathBuf),
    /// A path behind a provider session (remote: polled; see `openbc-vfs::observe` limits).
    Vfs { session_key: String, vfs: Arc<dyn AsyncVfs>, path: EntryPath, directory: bool },
}

pub enum WatchCommand {
    /// Start (or replace) the watch of a side; the baseline is the state right now.
    Set { side: Side, target: ResourceTarget },
    /// Stop watching a side.
    Clear { side: Side },
    /// "Reload Latest": adopt the current state as the loaded version. Send BEFORE re-reading.
    Rebase { side: Side },
    /// "Keep Current": stay on the loaded version; no repeat notice until a NEWER version appears.
    Keep { side: Side },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ResourceEvent {
    Stale { tab: TabId, side: Side, change: ChangeKind },
    /// The side matches what was loaded again (reverted, or rebased).
    Fresh { tab: TabId, side: Side },
    Unavailable { tab: TabId, side: Side, message: String },
}

pub type TabWatchHandle = CommandHandle<WatchCommand>;

// ------------------------------------------------------------------------------ pure state

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    Nothing,
    Stale(ChangeKind),
    Fresh,
    Unavailable(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Step {
    Done(Outcome),
    /// Metadata alone is inconclusive: probe with a content hash and call again with the result.
    NeedsContentCheck,
}

/// Staleness bookkeeping for one side.
#[derive(Debug)]
pub struct SideTrack {
    loaded: Option<ResourceVersion>,
    /// The baseline probe failed transiently; adopt the first real observation as the baseline.
    baseline_unknown: bool,
    stale: bool,
    /// Observed state the user chose to keep (suppresses a repeat notice for that exact state).
    kept: Option<Observed>,
    /// Last state a notice was emitted for.
    notified: Option<Observed>,
    unavailable_notified: Option<String>,
}

impl SideTrack {
    #[must_use]
    pub fn new(baseline: &Observed) -> Self {
        let (loaded, baseline_unknown) = match baseline {
            Observed::Present(v) => (Some(v.clone()), false),
            Observed::Missing => (None, false),
            Observed::Unavailable(_) => (None, true),
        };
        Self { loaded, baseline_unknown, stale: false, kept: None, notified: None, unavailable_notified: None }
    }

    #[must_use]
    pub fn is_stale(&self) -> bool {
        self.stale
    }

    /// Adopts `baseline` as the loaded version. Returns whether the side had been stale.
    pub fn rebase(&mut self, baseline: &Observed) -> bool {
        let was_stale = self.stale;
        *self = Self::new(baseline);
        was_stale
    }

    /// "Keep Current": remember the state the user declined to reload. Stays stale.
    pub fn keep(&mut self, current: &Observed) {
        self.kept = Some(current.clone());
    }

    /// Feeds one published observation. `confirm` is the content-hash probe, only after this
    /// returned [`Step::NeedsContentCheck`] for the same `current`.
    pub fn on_observed(&mut self, current: &Observed, confirm: Option<&Observed>) -> Step {
        if self.baseline_unknown {
            match current {
                Observed::Present(v) => {
                    self.loaded = Some(v.clone());
                    self.baseline_unknown = false;
                    return Step::Done(Outcome::Nothing);
                }
                Observed::Missing => {
                    self.baseline_unknown = false;
                    return Step::Done(Outcome::Nothing);
                }
                Observed::Unavailable(_) => {}
            }
        }
        match assess(self.loaded.as_ref(), current) {
            Assessment::Unchanged => Step::Done(self.back_to_fresh()),
            Assessment::Unknown => {
                let message = match current {
                    Observed::Unavailable(m) => m.clone(),
                    _ => String::new(),
                };
                if self.unavailable_notified.as_deref() == Some(message.as_str()) {
                    Step::Done(Outcome::Nothing)
                } else {
                    self.unavailable_notified = Some(message.clone());
                    Step::Done(Outcome::Unavailable(message))
                }
            }
            Assessment::NeedsContentCheck => match confirm {
                None => Step::NeedsContentCheck,
                Some(Observed::Present(v)) if self.loaded.as_ref().is_some_and(|l| l.same_content(v)) => {
                    Step::Done(self.back_to_fresh()) // touched, not changed
                }
                Some(Observed::Present(_)) => Step::Done(self.changed(ChangeKind::Modified, current)),
                Some(_) => Step::Done(Outcome::Nothing), // could not confirm: change nothing
            },
            Assessment::Changed(kind) => Step::Done(self.changed(kind, current)),
        }
    }

    fn back_to_fresh(&mut self) -> Outcome {
        self.unavailable_notified = None;
        if self.stale {
            self.stale = false;
            self.kept = None;
            self.notified = None;
            Outcome::Fresh
        } else {
            Outcome::Nothing
        }
    }

    fn changed(&mut self, kind: ChangeKind, current: &Observed) -> Outcome {
        self.unavailable_notified = None;
        self.stale = true;
        if self.kept.as_ref() == Some(current) || self.notified.as_ref() == Some(current) {
            return Outcome::Nothing; // same version already announced / deliberately kept
        }
        self.notified = Some(current.clone());
        Outcome::Stale(kind)
    }
}

// ----------------------------------------------------------------------------------- actor

struct SideSlot {
    sub: Subscription,
    track: SideTrack,
}

/// Starts the watcher for one tab. Dropping the returned handle (tab closed) ends the actor,
/// which drops its subscriptions; the hub then releases any watcher nobody else uses.
pub fn spawn_tab_watch(rt: &Handle, hub: Arc<WatchHub>, tab: TabId) -> (TabWatchHandle, mpsc::Receiver<ResourceEvent>) {
    let (handle, commands, events, event_rx) = channels();
    rt.spawn(run(hub, tab, commands, events));
    (handle, event_rx)
}

async fn wait_change(slot: &mut Option<SideSlot>) {
    match slot {
        Some(s) => {
            if !s.sub.changed().await {
                std::future::pending::<()>().await;
            }
        }
        None => std::future::pending::<()>().await,
    }
}

fn pick<'a>(left: &'a mut Option<SideSlot>, right: &'a mut Option<SideSlot>, side: Side) -> &'a mut Option<SideSlot> {
    if side == Side::Left { left } else { right }
}

async fn run(hub: Arc<WatchHub>, tab: TabId, mut commands: mpsc::Receiver<WatchCommand>, events: mpsc::Sender<ResourceEvent>) {
    let mut left: Option<SideSlot> = None;
    let mut right: Option<SideSlot> = None;
    loop {
        let alive = tokio::select! {
            command = commands.recv() => match command {
                None => return,
                Some(command) => apply(&hub, tab, &events, &mut left, &mut right, command).await,
            },
            () = wait_change(&mut left) => evaluate(tab, &events, &mut left, Side::Left).await,
            () = wait_change(&mut right) => evaluate(tab, &events, &mut right, Side::Right).await,
        };
        if !alive {
            return;
        }
    }
}

async fn apply(
    hub: &WatchHub,
    tab: TabId,
    events: &mpsc::Sender<ResourceEvent>,
    left: &mut Option<SideSlot>,
    right: &mut Option<SideSlot>,
    command: WatchCommand,
) -> bool {
    match command {
        WatchCommand::Set { side, target } => {
            let sub = match target {
                ResourceTarget::LocalFile(p) => hub.watch_local(&p, false),
                ResourceTarget::LocalDir(p) => hub.watch_local(&p, true),
                ResourceTarget::Vfs { session_key, vfs, path, directory } => hub.watch_vfs(&session_key, vfs, path, directory),
            };
            let baseline = sub.probe_now(true).await;
            *pick(left, right, side) = Some(SideSlot { track: SideTrack::new(&baseline), sub });
            true
        }
        WatchCommand::Clear { side } => {
            *pick(left, right, side) = None;
            true
        }
        WatchCommand::Rebase { side } => {
            if let Some(slot) = pick(left, right, side) {
                let baseline = slot.sub.probe_now(true).await;
                if slot.track.rebase(&baseline) {
                    return send_event(events, ResourceEvent::Fresh { tab, side }).await;
                }
            }
            true
        }
        WatchCommand::Keep { side } => {
            if let Some(slot) = pick(left, right, side) {
                let current = slot.sub.current().state;
                slot.track.keep(&current);
            }
            true
        }
    }
}

async fn evaluate(tab: TabId, events: &mpsc::Sender<ResourceEvent>, slot: &mut Option<SideSlot>, side: Side) -> bool {
    let Some(slot) = slot else { return true };
    let current = slot.sub.current().state;
    let mut step = slot.track.on_observed(&current, None);
    if step == Step::NeedsContentCheck {
        let confirm = slot.sub.probe_now(true).await;
        step = slot.track.on_observed(&current, Some(&confirm));
    }
    let event = match step {
        Step::Done(Outcome::Stale(change)) => ResourceEvent::Stale { tab, side, change },
        Step::Done(Outcome::Fresh) => ResourceEvent::Fresh { tab, side },
        Step::Done(Outcome::Unavailable(message)) => ResourceEvent::Unavailable { tab, side, message },
        Step::Done(Outcome::Nothing) | Step::NeedsContentCheck => return true,
    };
    send_event(events, event).await
}

#[cfg(test)]
mod tests {
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    use openbc_core::resource_version::ResourceKind;
    use openbc_vfs::observe::WatchConfig;

    use super::*;

    fn ver(size: u64, mtime: u128, hash: Option<u64>) -> Observed {
        Observed::Present(ResourceVersion { kind: ResourceKind::File, size, modified_ns: Some(mtime), file_id: Some(1), content_hash: hash })
    }

    fn done(step: Step) -> Outcome {
        match step {
            Step::Done(o) => o,
            Step::NeedsContentCheck => panic!("unexpected content check"),
        }
    }

    // ---- pure state machine

    #[test]
    fn unchanged_never_notifies() {
        let base = ver(10, 1, None);
        let mut t = SideTrack::new(&base);
        for _ in 0..5 {
            assert_eq!(done(t.on_observed(&base, None)), Outcome::Nothing);
        }
        assert!(!t.is_stale());
    }

    #[test]
    fn change_notifies_once_per_version_and_again_for_a_newer_one() {
        let mut t = SideTrack::new(&ver(10, 1, None));
        let v2 = ver(11, 2, None);
        assert_eq!(done(t.on_observed(&v2, None)), Outcome::Stale(ChangeKind::Modified));
        assert_eq!(done(t.on_observed(&v2, None)), Outcome::Nothing, "same version: no spam");
        assert!(t.is_stale());
        assert_eq!(done(t.on_observed(&ver(12, 3, None), None)), Outcome::Stale(ChangeKind::Modified), "newer version notifies again");
    }

    #[test]
    fn keep_current_suppresses_that_version_only_and_stays_stale() {
        let mut t = SideTrack::new(&ver(10, 1, None));
        let v2 = ver(11, 2, None);
        assert_eq!(done(t.on_observed(&v2, None)), Outcome::Stale(ChangeKind::Modified));
        t.keep(&v2);
        assert_eq!(done(t.on_observed(&v2, None)), Outcome::Nothing);
        assert!(t.is_stale(), "indicator stays; loaded content is still the old version");
        assert_eq!(done(t.on_observed(&ver(13, 4, None), None)), Outcome::Stale(ChangeKind::Modified));
    }

    #[test]
    fn reverting_the_file_makes_it_fresh() {
        let base = ver(10, 1, None);
        let mut t = SideTrack::new(&base);
        t.on_observed(&ver(11, 2, None), None);
        assert_eq!(done(t.on_observed(&base, None)), Outcome::Fresh);
        assert!(!t.is_stale());
    }

    #[test]
    fn rebase_adopts_current_and_a_change_during_reload_is_not_lost() {
        let mut t = SideTrack::new(&ver(10, 1, None));
        let v2 = ver(11, 2, None);
        t.on_observed(&v2, None);
        assert!(t.rebase(&v2), "was stale");
        assert_eq!(done(t.on_observed(&v2, None)), Outcome::Nothing);
        // The file changes again while the reload is reading: baseline v2, next observation v3.
        assert_eq!(done(t.on_observed(&ver(12, 3, None), None)), Outcome::Stale(ChangeKind::Modified));
    }

    #[test]
    fn touch_is_confirmed_by_hash_and_not_reported() {
        let mut t = SideTrack::new(&ver(10, 1, Some(77)));
        let touched = ver(10, 9, None);
        assert_eq!(t.on_observed(&touched, None), Step::NeedsContentCheck);
        assert_eq!(done(t.on_observed(&touched, Some(&ver(10, 9, Some(77))))), Outcome::Nothing);
        assert!(!t.is_stale());
        // A real same-size rewrite is caught.
        assert_eq!(t.on_observed(&touched, None), Step::NeedsContentCheck);
        assert_eq!(done(t.on_observed(&touched, Some(&ver(10, 9, Some(78))))), Outcome::Stale(ChangeKind::Modified));
    }

    #[test]
    fn deletion_unavailability_and_restoration() {
        let base = ver(10, 1, None);
        let mut t = SideTrack::new(&base);
        assert_eq!(done(t.on_observed(&Observed::Missing, None)), Outcome::Stale(ChangeKind::Deleted));
        let denied = Observed::Unavailable("permission denied".into());
        assert_eq!(done(t.on_observed(&denied, None)), Outcome::Unavailable("permission denied".into()));
        assert_eq!(done(t.on_observed(&denied, None)), Outcome::Nothing, "same error not repeated");
        assert!(t.is_stale(), "unavailability does not clear staleness");
        assert_eq!(done(t.on_observed(&base, None)), Outcome::Fresh);
    }

    #[test]
    fn transient_baseline_failure_adopts_first_real_observation() {
        let mut t = SideTrack::new(&Observed::Unavailable("network down".into()));
        assert_eq!(done(t.on_observed(&ver(5, 5, None), None)), Outcome::Nothing);
        assert!(!t.is_stale());
        assert_eq!(done(t.on_observed(&ver(6, 6, None), None)), Outcome::Stale(ChangeKind::Modified));
    }

    // ---- actor over real files

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "openbc-tabwatch-{tag}-{}-{}",
            std::process::id(),
            SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn hub() -> Arc<WatchHub> {
        Arc::new(WatchHub::new(
            Handle::current(),
            WatchConfig {
                local_poll: Duration::from_millis(20),
                local_poll_with_native: Duration::from_millis(20),
                remote_poll: Duration::from_millis(20),
                max_backoff: Duration::from_millis(80),
                debounce: Duration::from_millis(5),
            },
        ))
    }

    async fn next(rx: &mut mpsc::Receiver<ResourceEvent>) -> Option<ResourceEvent> {
        tokio::time::timeout(Duration::from_secs(5), rx.recv()).await.ok().flatten()
    }

    async fn quiet(rx: &mut mpsc::Receiver<ResourceEvent>) -> bool {
        tokio::time::timeout(Duration::from_millis(300), rx.recv()).await.is_err()
    }

    #[tokio::test]
    async fn edit_keep_reload_cycle() {
        let dir = temp_dir("cycle");
        let file = dir.join("a.txt");
        std::fs::write(&file, "first").unwrap();
        let (handle, mut rx) = spawn_tab_watch(&Handle::current(), hub(), TabId(1));
        assert!(handle.send(WatchCommand::Set { side: Side::Left, target: ResourceTarget::LocalFile(file.clone()) }));
        assert!(quiet(&mut rx).await, "an untouched file produces no event");

        std::fs::write(&file, "second version").unwrap();
        assert_eq!(next(&mut rx).await, Some(ResourceEvent::Stale { tab: TabId(1), side: Side::Left, change: ChangeKind::Modified }));
        assert!(quiet(&mut rx).await, "no repeat notice for the same version");

        assert!(handle.send(WatchCommand::Keep { side: Side::Left }));
        assert!(quiet(&mut rx).await, "Keep Current stays silent");
        std::fs::write(&file, "third version!!").unwrap();
        assert_eq!(next(&mut rx).await, Some(ResourceEvent::Stale { tab: TabId(1), side: Side::Left, change: ChangeKind::Modified }), "a newer version notifies again");

        assert!(handle.send(WatchCommand::Rebase { side: Side::Left }));
        assert_eq!(next(&mut rx).await, Some(ResourceEvent::Fresh { tab: TabId(1), side: Side::Left }));
        assert!(quiet(&mut rx).await);
        std::fs::remove_dir_all(dir).ok();
    }

    #[tokio::test]
    async fn deleted_file_is_reported_and_loaded_content_is_not_touched() {
        let dir = temp_dir("delete");
        let file = dir.join("a.txt");
        std::fs::write(&file, "x").unwrap();
        let (handle, mut rx) = spawn_tab_watch(&Handle::current(), hub(), TabId(2));
        handle.send(WatchCommand::Set { side: Side::Right, target: ResourceTarget::LocalFile(file.clone()) });
        assert!(quiet(&mut rx).await);
        std::fs::remove_file(&file).unwrap();
        assert_eq!(next(&mut rx).await, Some(ResourceEvent::Stale { tab: TabId(2), side: Side::Right, change: ChangeKind::Deleted }));
        std::fs::remove_dir_all(dir).ok();
    }

    #[tokio::test]
    async fn tabs_sharing_a_file_share_one_watcher_and_notify_independently() {
        let dir = temp_dir("shared");
        let file = dir.join("a.txt");
        std::fs::write(&file, "x").unwrap();
        let hub = hub();
        let (h1, mut r1) = spawn_tab_watch(&Handle::current(), hub.clone(), TabId(1));
        let (h2, mut r2) = spawn_tab_watch(&Handle::current(), hub.clone(), TabId(2));
        h1.send(WatchCommand::Set { side: Side::Left, target: ResourceTarget::LocalFile(file.clone()) });
        h2.send(WatchCommand::Set { side: Side::Right, target: ResourceTarget::LocalFile(file.clone()) });
        assert!(quiet(&mut r1).await && quiet(&mut r2).await);
        assert_eq!(hub.active_watchers(), 1);

        std::fs::write(&file, "changed content").unwrap();
        assert_eq!(next(&mut r1).await, Some(ResourceEvent::Stale { tab: TabId(1), side: Side::Left, change: ChangeKind::Modified }));
        assert_eq!(next(&mut r2).await, Some(ResourceEvent::Stale { tab: TabId(2), side: Side::Right, change: ChangeKind::Modified }));

        // Tab 1 reloads; tab 2 is unaffected and keeps its own stale state.
        h1.send(WatchCommand::Rebase { side: Side::Left });
        assert_eq!(next(&mut r1).await, Some(ResourceEvent::Fresh { tab: TabId(1), side: Side::Left }));
        assert!(quiet(&mut r2).await);

        // Closing a tab (dropping its handle) unregisters it; the last close frees the watcher.
        drop(h1);
        tokio::time::sleep(Duration::from_millis(100)).await;
        assert_eq!(hub.active_watchers(), 1);
        drop(h2);
        tokio::time::sleep(Duration::from_millis(100)).await;
        assert_eq!(hub.active_watchers(), 0, "no watcher leak after both tabs closed");
        std::fs::remove_dir_all(dir).ok();
    }
}
