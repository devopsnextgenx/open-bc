//! Channel plumbing, the shared actor loop and the Tokio <-> Rayon bridges.

use async_trait::async_trait;
use tokio::runtime::Handle;
use tokio::sync::{mpsc, oneshot};

/// Events buffered between an actor and the UI bridge.
pub const EVENT_CHANNEL_CAPACITY: usize = 4;
/// Commands buffered between the UI bridge and an actor (UI uses `try_send`).
pub const COMMAND_CHANNEL_CAPACITY: usize = 16;

/// A Rayon task panicked or the pool shut down.
#[derive(Debug, thiserror::Error)]
#[error("background worker stopped unexpectedly")]
pub struct TaskLost;

/// Starts CPU-bound `f` on the Rayon pool right away and returns the receiver for its result.
pub fn rayon_spawn<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> oneshot::Receiver<T> {
    let (tx, rx) = oneshot::channel();
    rayon::spawn(move || {
        let _ = tx.send(f());
    });
    rx
}

/// Runs CPU-bound `f` on the Rayon pool without blocking a Tokio worker.
pub async fn rayon_run<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> Result<T, TaskLost> {
    rayon_spawn(f).await.map_err(|_| TaskLost)
}

/// Sends `event`; `false` means the consumer is gone and the actor should stop.
pub async fn send_event<E: Send>(tx: &mpsc::Sender<E>, event: E) -> bool {
    tx.send(event).await.is_ok()
}

/// Non-blocking command sender shared by every actor (the UI bridge calls it from its thread).
pub struct CommandHandle<C> {
    tx: mpsc::Sender<C>,
}

impl<C> Clone for CommandHandle<C> {
    fn clone(&self) -> Self {
        Self { tx: self.tx.clone() }
    }
}

impl<C> CommandHandle<C> {
    /// Non-blocking; `false` when the actor is busy (queue full) or gone.
    pub fn send(&self, command: C) -> bool {
        self.tx.try_send(command).is_ok()
    }
}

/// The standard pair of bounded channels for one actor:
/// `(handle, command receiver, event sender, event receiver)`.
pub fn channels<C, E>() -> (CommandHandle<C>, mpsc::Receiver<C>, mpsc::Sender<E>, mpsc::Receiver<E>) {
    let (command_tx, command_rx) = mpsc::channel(COMMAND_CHANNEL_CAPACITY);
    let (event_tx, event_rx) = mpsc::channel(EVENT_CHANNEL_CAPACITY);
    (CommandHandle { tx: command_tx }, command_rx, event_tx, event_rx)
}

/// An actor that loads, then repeatedly: drains every queued command, applies them in order,
/// and publishes one fresh result (so a burst of option changes costs a single re-diff).
///
/// Every method returns `false` when the event consumer is gone, which ends the actor.
#[async_trait]
pub trait BatchedActor: Send + Sized + 'static {
    type Command: Send + 'static;

    /// Initial load, before the first result.
    async fn init(&mut self) -> bool;
    /// Applies one command to the actor's state.
    async fn apply(&mut self, command: Self::Command) -> bool;
    /// Computes and emits the result for the current state.
    async fn publish(&mut self) -> bool;
}

/// Drives a [`BatchedActor`] until its commands or its event consumer go away.
pub async fn run_batched<A: BatchedActor>(mut actor: A, mut commands: mpsc::Receiver<A::Command>) {
    if !actor.init().await || !actor.publish().await {
        return;
    }
    while let Some(first) = commands.recv().await {
        // Coalesce: everything queued while we were busy is handled before one re-publish.
        let mut batch = vec![first];
        while let Ok(next) = commands.try_recv() {
            batch.push(next);
        }
        for command in batch {
            if !actor.apply(command).await {
                return;
            }
        }
        if !actor.publish().await {
            return;
        }
    }
}

/// Spawns a [`BatchedActor`] built by `build` (which receives the event sender) and returns
/// the UI-facing handle and event receiver.
pub fn spawn_batched<A, E>(rt: &Handle, build: impl FnOnce(mpsc::Sender<E>) -> A) -> (CommandHandle<A::Command>, mpsc::Receiver<E>)
where
    A: BatchedActor,
    E: Send + 'static,
{
    let (handle, commands, events, event_rx) = channels();
    rt.spawn(run_batched(build(events), commands));
    (handle, event_rx)
}
