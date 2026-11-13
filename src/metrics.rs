//! Same-clock stage durations only; never subtract unsynchronized Linux/Windows timestamps.
use std::{
    collections::BTreeMap,
    sync::{Mutex, OnceLock},
    time::Duration,
};
#[derive(Default)]
struct Stage {
    count: u64,
    total: u128,
    max: u128,
}
static STAGES: OnceLock<Mutex<BTreeMap<&'static str, Stage>>> = OnceLock::new();
pub fn init(enabled: bool) {
    if !enabled {
        return;
    }
    STAGES.get_or_init(Mutex::default);
    std::thread::spawn(|| loop {
        std::thread::sleep(Duration::from_secs(2));
        let stages = std::mem::take(&mut *STAGES.get().unwrap().lock().unwrap());
        for (name, stage) in stages {
            tracing::info!(
                stage = name,
                samples = stage.count,
                avg_ms = stage.total as f64 / stage.count as f64 / 1000.,
                max_ms = stage.max as f64 / 1000.,
                "pipeline timing"
            );
        }
    });
}
pub fn record(name: &'static str, duration: Duration) {
    if let Some(stages) = STAGES.get() {
        let mut stages = stages.lock().unwrap();
        let stage = stages.entry(name).or_default();
        let us = duration.as_micros();
        stage.count += 1;
        stage.total += us;
        stage.max = stage.max.max(us);
    }
}
