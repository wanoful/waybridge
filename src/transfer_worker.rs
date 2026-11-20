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
    collections::{BTreeMap, VecDeque},
    path::PathBuf,
    sync::{mpsc::SyncSender, Arc, Condvar, Mutex, Weak},
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
    clipboard_generation: Option<u64>,
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
    ingress: Ingress,
    worker: Option<JoinHandle<()>>,
}

#[derive(Clone, Default)]
pub struct Ingress(Arc<Shared>);

impl Ingress {
    /// Only the independent file receiver may wait for disk queue capacity.
    pub fn submit_data(&self, peer: Arc<Peer>, message: Value) -> bool {
        self.submit(peer, message, None)
    }

    pub fn abort_data(&self, peer: Arc<Peer>) {
        let mut queue = self.0.queue.lock().unwrap();
        if queue.closed || peer.is_closed() {
            return;
        }
        queue.jobs.retain(|job| {
            !Arc::ptr_eq(&job.peer, &peer)
                || !job.message["type"]
                    .as_str()
                    .is_some_and(|name| name.starts_with("tree_upload_"))
        });
        queue.bytes = queue.jobs.iter().map(|job| job.bytes).sum();
        let epoch = queue.epoch;
        queue.jobs.push_front(Job {
            peer,
            message: json!({"type":"file_lane_cleanup"}),
            clipboard_generation: None,
            epoch,
            bytes: 0,
        });
        self.0.wake.notify_all();
    }

    fn submit(&self, peer: Arc<Peer>, message: Value, generation: Option<u64>) -> bool {
        let bytes = footprint(&message);
        if bytes > MAX_BYTES {
            return false;
        }
        let (max_jobs, max_bytes) = if generation.is_some() {
            (MAX_JOBS, MAX_BYTES)
        } else {
            (MAX_JOBS - 8, MAX_BYTES - 2 * 1024 * 1024)
        };
        if bytes > max_bytes {
            return false;
        }
        let mut queue = self.0.queue.lock().unwrap();
        let epoch = queue.epoch;
        while !queue.closed && queue.epoch == epoch && !peer.is_closed() {
            if queue.jobs.len() < max_jobs && bytes <= max_bytes.saturating_sub(queue.bytes) {
                queue.bytes += bytes;
                queue.jobs.push_back(Job {
                    peer,
                    message,
                    clipboard_generation: generation,
                    epoch,
                    bytes,
                });
                self.0.wake.notify_all();
                return true;
            }
            if generation.is_some() {
                return false;
            }
            queue = self
                .0
                .wake
                .wait_timeout(queue, std::time::Duration::from_millis(100))
                .unwrap()
                .0;
        }
        false
    }
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
    pub fn new(root: PathBuf, events: SyncSender<Event>, ingress: Ingress) -> Result<Self> {
        // Initial filesystem setup happens during startup, not event dispatch.
        let mut transfers = Transfers::new(root)?;
        let state = ingress.0.clone();
        let worker = thread::Builder::new().name("file-receive".into()).spawn(move || {
            let mut epoch = 0;
            let mut legacy_failed = false;
            let mut generations: BTreeMap<u64, (u64, Weak<Peer>)> = BTreeMap::new();
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
                        generations.clear();
                        continue;
                    }
                    let job = queue.jobs.pop_front().unwrap();
                    queue.bytes -= job.bytes;
                    state.wake.notify_all();
                    job
                };
                if job.peer.is_closed() { continue; }
                let tree = job.message["type"].as_str().unwrap_or("").starts_with("tree_");
                let kind = job.message["type"].as_str().unwrap_or("");
                if kind == "file_lane_cleanup" {
                    generations.retain(|batch, (_, source)| {
                        if Weak::ptr_eq(source, &Arc::downgrade(&job.peer)) {
                            transfers.cancel_tree(*batch);
                            false
                        } else {
                            true
                        }
                    });
                    continue;
                }
                let batch = job.message["batch"].as_u64().unwrap_or(0);
                if tree && !job.peer.file_upload_current(batch) {
                    transfers.cancel_tree(batch);
                    generations.remove(&batch);
                    continue;
                }
                if !tree && kind == "upload_begin" { legacy_failed = false; }
                if !tree && legacy_failed { continue; }
                match receive(&mut transfers, &job) {
                    Ok(Some(uris)) => {
                        let valid = {
                            let queue = state.queue.lock().unwrap();
                            !queue.closed && queue.epoch == job.epoch
                        };
                        if valid && !job.peer.is_closed() {
                            let clipboard_generation = generations.remove(&batch).map(|(generation, _)| generation)
                                .or(job.clipboard_generation).unwrap_or_default();
                            let mut event = Event::UploadedFiles(Completed {
                                peer: job.peer, message: job.message, uris,
                                clipboard_generation, epoch: job.epoch,
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
                    Ok(None) => {
                        if kind == "tree_upload_begin" && job.peer.has_file_lane() {
                            generations.insert(batch, (job.clipboard_generation.unwrap_or_default(), Arc::downgrade(&job.peer)));
                        } else if kind == "tree_upload_cancel" {
                            generations.remove(&batch);
                        }
                    }
                    Err(error) => {
                        if tree {
                            transfers.cancel_tree(batch);
                            generations.remove(&batch);
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
            ingress,
            worker: Some(worker),
        })
    }

    /// Never waits for disk or queue space on the compositor thread.
    pub fn submit(&self, peer: Arc<Peer>, message: Value, clipboard_generation: u64) -> bool {
        self.ingress
            .submit(peer, message, Some(clipboard_generation))
    }

    pub fn current(&self, epoch: u64) -> bool {
        let queue = self.ingress.0.queue.lock().unwrap();
        !queue.closed && queue.epoch == epoch
    }

    pub fn cancel(&self) {
        let mut queue = self.ingress.0.queue.lock().unwrap();
        queue.epoch = queue.epoch.wrapping_add(1);
        queue.jobs.clear();
        queue.bytes = 0;
        self.ingress.0.wake.notify_all();
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        {
            let mut queue = self.ingress.0.queue.lock().unwrap();
            queue.closed = true;
            queue.jobs.clear();
            queue.bytes = 0;
            self.ingress.0.wake.notify_all();
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
        "tree_upload_barrier" => job
            .peer
            .control(json!({"type":"tree_upload_ready","batch":batch,"stage":"data"})),
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
