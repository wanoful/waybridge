//! Ordered, bounded receive-side file I/O. Only completed batches return to the
//! compositor; connection and clipboard identities are checked before publishing.
use crate::{
    file_tree,
    network::{Event, Peer},
    transfer::Transfers,
};
use anyhow::{bail, Context, Result};
use serde_json::{json, Value};
use std::{
    collections::VecDeque,
    path::PathBuf,
    sync::{mpsc::SyncSender, Arc, Condvar, Mutex},
    thread::{self, JoinHandle},
};

const MAX_JOBS: usize = 128;
const MAX_BYTES: usize = 16 * 1024 * 1024;

pub struct Completed {
    pub peer: Arc<Peer>,
    pub message: Value,
    pub uris: Vec<u8>,
    pub clipboard_generation: u64,
    pub epoch: u64,
}

struct Job {
    peer: Arc<Peer>,
    message: Value,
    clipboard_generation: u64,
    epoch: u64,
    bytes: usize,
}

#[derive(Default)]
struct Queue {
    jobs: VecDeque<Job>,
    bytes: usize,
    epoch: u64,
    closed: bool,
}

#[derive(Default)]
struct Shared {
    queue: Mutex<Queue>,
    wake: Condvar,
}

pub struct Worker {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}

fn footprint(value: &Value) -> usize {
    match value {
        Value::String(text) => text.len().saturating_add(32),
        Value::Array(values) => values
            .iter()
            .fold(32usize, |n, v| n.saturating_add(footprint(v))),
        Value::Object(values) => values.iter().fold(32usize, |n, (k, v)| {
            n.saturating_add(k.len()).saturating_add(footprint(v))
        }),
        _ => 32,
    }
}

impl Worker {
    pub fn new(root: PathBuf, events: SyncSender<Event>) -> Result<Self> {
        // Initial filesystem setup happens during startup, not event dispatch.
        let mut transfers = Transfers::new(root)?;
        let shared = Arc::new(Shared::default());
        let state = shared.clone();
        let worker = thread::Builder::new().name("file-receive".into()).spawn(move || {
            let mut epoch = 0;
            let mut legacy_failed = false;
            loop {
                let job = {
                    let mut queue = state.queue.lock().unwrap();
                    while !queue.closed && queue.epoch == epoch && queue.jobs.is_empty() {
                        queue = state.wake.wait(queue).unwrap();
                    }
                    if queue.closed {
                        drop(queue);
                        transfers.cancel();
                        break;
                    }
                    if queue.epoch != epoch {
                        epoch = queue.epoch;
                        drop(queue);
                        transfers.cancel();
                        legacy_failed = false;
                        continue;
                    }
                    let job = queue.jobs.pop_front().unwrap();
                    queue.bytes -= job.bytes;
                    job
                };
                if job.peer.is_closed() { continue; }
                let tree = job.message["type"].as_str().unwrap_or("").starts_with("tree_");
                let kind = job.message["type"].as_str().unwrap_or("");
                if !tree && kind == "upload_begin" { legacy_failed = false; }
                if !tree && legacy_failed { continue; }
                match receive(&mut transfers, &job) {
                    Ok(Some(uris)) => {
                        let valid = {
                            let queue = state.queue.lock().unwrap();
                            !queue.closed && queue.epoch == job.epoch
                        };
                        if valid && !job.peer.is_closed() {
                            let mut event = Event::UploadedFiles(Completed {
                                peer: job.peer, message: job.message, uris,
                                clipboard_generation: job.clipboard_generation, epoch: job.epoch,
                            });
                            loop {
                                match events.try_send(event) {
                                    Ok(()) | Err(std::sync::mpsc::TrySendError::Disconnected(_)) => break,
                                    Err(std::sync::mpsc::TrySendError::Full(pending)) => event = pending,
                                }
                                let queue = state.queue.lock().unwrap();
                                if queue.closed || queue.epoch != epoch { break; }
                                drop(queue);
                                thread::sleep(std::time::Duration::from_millis(2));
                            }
                        }
                    }
                    Ok(None) => {}
                    Err(error) => {
                        if tree {
                            let batch = job.message["batch"].as_u64().unwrap_or(0);
                            transfers.cancel_tree(batch);
                            job.peer.control(json!({"type":"tree_upload_error","batch":batch,"message":error.to_string()}));
                        } else {
                            transfers.cancel();
                            legacy_failed = true;
                            job.peer.control(json!({"type":"error","message":format!("File transfer: {error}")}));
                        }
                    }
                }
            }
        })?;
        Ok(Self {
            shared,
            worker: Some(worker),
        })
    }

    /// Never waits for disk or queue space on the compositor thread.
    pub fn submit(&self, peer: Arc<Peer>, message: Value, clipboard_generation: u64) -> bool {
        let bytes = footprint(&message);
        let mut queue = self.shared.queue.lock().unwrap();
        if queue.closed
            || queue.jobs.len() >= MAX_JOBS
            || bytes > MAX_BYTES.saturating_sub(queue.bytes)
        {
            return false;
        }
        let epoch = queue.epoch;
        queue.bytes += bytes;
        queue.jobs.push_back(Job {
            peer,
            message,
            clipboard_generation,
            epoch,
            bytes,
        });
        self.shared.wake.notify_one();
        true
    }

    pub fn current(&self, epoch: u64) -> bool {
        let queue = self.shared.queue.lock().unwrap();
        !queue.closed && queue.epoch == epoch
    }

    pub fn cancel(&self) {
        let mut queue = self.shared.queue.lock().unwrap();
        queue.epoch = queue.epoch.wrapping_add(1);
        queue.jobs.clear();
        queue.bytes = 0;
        self.shared.wake.notify_one();
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        {
            let mut queue = self.shared.queue.lock().unwrap();
            queue.closed = true;
            queue.jobs.clear();
            queue.bytes = 0;
            self.shared.wake.notify_all();
        }
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

fn receive(transfers: &mut Transfers, job: &Job) -> Result<Option<Vec<u8>>> {
    let value = &job.message;
    let kind = value["type"].as_str().unwrap_or("");
    let batch = value["batch"].as_u64().unwrap_or(0);
    if kind.starts_with("tree_") {
        if !job.peer.file_tree() {
            bail!("file_tree capability required");
        }
        if !matches!(kind, "tree_upload_begin" | "tree_upload_cancel") && !transfers.has_tree(batch)
        {
            return Ok(None);
        }
    }
    match kind {
        "tree_upload_begin" => {
            let metadata = value["entries"]
                .as_array()
                .context("missing tree entries")?;
            if metadata.is_empty() || metadata.len() > file_tree::MAX_ENTRIES {
                bail!("invalid tree entry count");
            }
            let entries: Vec<file_tree::Entry> = serde_json::from_value(value["entries"].clone())?;
            let epoch = if value["drop"].as_bool() == Some(true) {
                None
            } else {
                Some(
                    value["epoch"]
                        .as_u64()
                        .unwrap_or_else(|| job.peer.file_epoch()),
                )
            };
            if epoch.is_some_and(|epoch| epoch != job.peer.file_epoch()) {
                bail!("clipboard upload superseded");
            }
            transfers.begin_tree(batch, entries, epoch)?;
            job.peer
                .control(json!({"type":"tree_upload_ready","batch":batch,"stage":"begin"}));
        }
        "tree_upload_chunk" => transfers.tree_chunk(
            batch,
            usize::try_from(value["file"].as_u64().context("missing file")?)?,
            value["data"].as_str().context("missing chunk")?,
        )?,
        "tree_upload_end" => transfers.tree_end(
            batch,
            usize::try_from(value["file"].as_u64().context("missing file")?)?,
        )?,
        "tree_upload_commit" => {
            return transfers
                .finish_tree(
                    batch,
                    job.peer.file_epoch(),
                    value["drop"].as_bool() == Some(true),
                )
                .map(Some)
        }
        "tree_upload_cancel" => transfers.cancel_tree(batch),
        "upload_begin" => transfers.begin(
            value["transfer"].as_u64().unwrap_or(0),
            value["name"].as_str().context("missing name")?,
            value["size"].as_u64().context("missing size")?,
        )?,
        "upload_chunk" => transfers.chunk(
            value["transfer"].as_u64().unwrap_or(0),
            value["data"].as_str().context("missing chunk")?,
        )?,
        "upload_end" => transfers.end(value["transfer"].as_u64().unwrap_or(0))?,
        "upload_commit" => {
            let ids: Vec<u64> = serde_json::from_value(value["transfers"].clone())?;
            return transfers.finish(&ids).map(Some);
        }
        _ => bail!("unknown upload operation"),
    }
    Ok(None)
}
