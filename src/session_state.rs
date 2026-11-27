//! Non-blocking session status publisher for out-of-band inspection without an
//! attached viewer. All file I/O runs on a dedicated coalescing writer thread.
use anyhow::{Context, Result};
use serde::Serialize;
use std::{
    fs,
    path::{Path, PathBuf},
    sync::{Arc, Condvar, Mutex},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

const MAX_WINDOWS: usize = 128;
const MAX_BACKGROUND_APPS: usize = 64;
const COALESCE_INTERVAL: Duration = Duration::from_millis(250);

#[derive(Clone, PartialEq, Eq, Serialize)]
pub struct WindowSummary {
    pub id: u32,
    pub title: String,
    pub app_id: String,
    pub x11: bool,
}

#[derive(Clone, PartialEq, Eq, Serialize)]
pub struct BackgroundAppSummary {
    pub id: String,
    pub app_id: String,
    pub title: String,
    pub status: String,
}

#[derive(Serialize)]
struct Snapshot<'a> {
    version: u8,
    pid: u32,
    started: &'a str,
    viewer_connected: bool,
    windows: &'a [WindowSummary],
    background_apps: &'a [BackgroundAppSummary],
}

struct SharedState {
    viewer_connected: bool,
    windows: Vec<WindowSummary>,
    background_apps: Vec<BackgroundAppSummary>,
    dirty: bool,
    closed: bool,
}

pub struct Publisher {
    path: PathBuf,
    shared: Arc<(Mutex<SharedState>, Condvar)>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

pub fn sanitize(text: &str, limit: usize) -> String {
    text.chars()
        .filter(|c| !c.is_control())
        .take(limit)
        .collect()
}

fn process_started(pid: u32) -> Result<String> {
    let stat = fs::read_to_string(format!("/proc/{pid}/stat"))?;
    stat.rsplit_once(')')
        .and_then(|(_, tail)| tail.split_whitespace().nth(19))
        .map(str::to_owned)
        .context("reading session process start time")
}

fn write_snapshot(
    runtime: &Path,
    path: &Path,
    pid: u32,
    started: &str,
    viewer_connected: bool,
    windows: &[WindowSummary],
    background_apps: &[BackgroundAppSummary],
) -> Result<()> {
    let snapshot = Snapshot {
        version: 1,
        pid,
        started,
        viewer_connected,
        windows,
        background_apps,
    };
    let mut temp = tempfile::NamedTempFile::new_in(runtime)?;
    serde_json::to_writer(temp.as_file_mut(), &snapshot)?;
    temp.persist(path).map_err(|e| e.error)?;
    Ok(())
}

impl Publisher {
    pub fn start(runtime: &Path) -> Result<Arc<Self>> {
        let pid = std::process::id();
        let started = process_started(pid)?;
        let runtime_dir = runtime.to_path_buf();
        let path = runtime_dir.join("session-state.json");
        let shared = Arc::new((
            Mutex::new(SharedState {
                viewer_connected: false,
                windows: Vec::new(),
                background_apps: Vec::new(),
                dirty: true,
                closed: false,
            }),
            Condvar::new(),
        ));
        let worker_shared = shared.clone();
        let worker_path = path.clone();
        let handle = thread::spawn(move || {
            let (lock, wake) = &*worker_shared;
            let mut last_write: Option<Instant> = None;
            loop {
                let (viewer_connected, windows, background_apps) = {
                    let mut state = lock.lock().unwrap();
                    loop {
                        if state.closed {
                            return;
                        }
                        if state.dirty {
                            if let Some(last) = last_write {
                                let elapsed = last.elapsed();
                                if elapsed < COALESCE_INTERVAL {
                                    state = wake
                                        .wait_timeout(state, COALESCE_INTERVAL - elapsed)
                                        .unwrap()
                                        .0;
                                    continue;
                                }
                            }
                            state.dirty = false;
                            break (
                                state.viewer_connected,
                                state.windows.clone(),
                                state.background_apps.clone(),
                            );
                        }
                        state = wake.wait(state).unwrap();
                    }
                };
                if let Err(error) = write_snapshot(
                    &runtime_dir,
                    &worker_path,
                    pid,
                    &started,
                    viewer_connected,
                    &windows,
                    &background_apps,
                ) {
                    tracing::debug!(%error, "could not publish session state snapshot");
                }
                last_write = Some(Instant::now());
            }
        });
        Ok(Arc::new(Self {
            path,
            shared,
            worker: Mutex::new(Some(handle)),
        }))
    }

    pub fn set_viewer_connected(&self, connected: bool) {
        let (lock, wake) = &*self.shared;
        let mut state = lock.lock().unwrap();
        if state.viewer_connected != connected {
            state.viewer_connected = connected;
            state.dirty = true;
            wake.notify_one();
        }
    }

    pub fn set_windows(&self, mut windows: Vec<WindowSummary>) {
        windows.truncate(MAX_WINDOWS);
        let (lock, wake) = &*self.shared;
        let mut state = lock.lock().unwrap();
        if state.windows != windows {
            state.windows = windows;
            state.dirty = true;
            wake.notify_one();
        }
    }

    pub fn set_background_apps(&self, mut apps: Vec<BackgroundAppSummary>) {
        apps.truncate(MAX_BACKGROUND_APPS);
        let (lock, wake) = &*self.shared;
        let mut state = lock.lock().unwrap();
        if state.background_apps != apps {
            state.background_apps = apps;
            state.dirty = true;
            wake.notify_one();
        }
    }

    pub fn stop(&self) {
        let (lock, wake) = &*self.shared;
        {
            let mut state = lock.lock().unwrap();
            state.closed = true;
            wake.notify_all();
        }
        if let Some(handle) = self.worker.lock().unwrap().take() {
            let _ = handle.join();
        }
        let _ = fs::remove_file(&self.path);
    }
}

impl Drop for Publisher {
    fn drop(&mut self) {
        self.stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    #[test]
    fn publishes_private_snapshot_and_cleans_up() {
        let dir = tempfile::tempdir().unwrap();
        let publisher = Publisher::start(dir.path()).unwrap();
        publisher.set_viewer_connected(true);
        publisher.set_windows(vec![WindowSummary {
            id: 7,
            title: sanitize("Editor\nTitle", 256),
            app_id: "org.example.Editor".into(),
            x11: false,
        }]);
        publisher.set_background_apps(vec![BackgroundAppSummary {
            id: "org.example.Tray/StatusNotifierItem".into(),
            app_id: "org.example.Tray".into(),
            title: "Tray App".into(),
            status: "Active".into(),
        }]);

        let snapshot_path = dir.path().join("session-state.json");
        let deadline = Instant::now() + Duration::from_secs(3);
        let parsed = loop {
            if let Ok(text) = fs::read_to_string(&snapshot_path) {
                if let Ok(value) = serde_json::from_str::<serde_json::Value>(&text) {
                    if value["viewer_connected"] == true
                        && value["windows"].as_array().is_some_and(|w| w.len() == 1)
                        && value["background_apps"]
                            .as_array()
                            .is_some_and(|b| b.len() == 1)
                    {
                        break value;
                    }
                }
            }
            assert!(Instant::now() < deadline, "timed out waiting for snapshot");
            thread::sleep(Duration::from_millis(20));
        };
        let mode = fs::metadata(&snapshot_path).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
        assert_eq!(parsed["version"], 1);
        assert_eq!(parsed["pid"], std::process::id());
        assert_eq!(parsed["windows"][0]["title"], "EditorTitle");
        assert_eq!(parsed["background_apps"][0]["title"], "Tray App");

        publisher.stop();
        assert!(!snapshot_path.exists());
    }
}
