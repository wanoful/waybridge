//! Bounded same-clock diagnostics; these are not physical input-to-display timings.
use std::{
    collections::BTreeMap,
    sync::{Mutex, OnceLock},
    time::Duration,
};

// Sixteen subdivisions per power of two, in microseconds. Percentiles report
// bucket upper bounds (at most 6.25% rounding above a sample, exact below 32 us).
// Constant memory regardless of frame rate, window count or a stalled reporter.
const BUCKETS: usize = 16 + 60 * 16;
struct Stage {
    count: u64,
    total: u128,
    max: u64,
    histogram: [u64; BUCKETS],
}
impl Default for Stage {
    fn default() -> Self {
        Self {
            count: 0,
            total: 0,
            max: 0,
            histogram: [0; BUCKETS],
        }
    }
}
impl Stage {
    fn record(&mut self, duration: Duration) {
        let us = duration.as_micros().min(u64::MAX as u128) as u64;
        let bucket = if us < 16 {
            us as usize
        } else {
            let exponent = 63 - us.leading_zeros();
            16 + (exponent as usize - 4) * 16 + ((us >> (exponent - 4)) - 16) as usize
        };
        self.count += 1;
        self.total += u128::from(us);
        self.max = self.max.max(us);
        self.histogram[bucket] += 1;
    }
    fn percentile(&self, percent: u64) -> u64 {
        let rank = (u128::from(self.count) * u128::from(percent)).div_ceil(100);
        let mut seen = 0u128;
        for (index, count) in self.histogram.iter().enumerate() {
            seen += u128::from(*count);
            if seen >= rank {
                let upper = if index < 16 {
                    index as u128
                } else {
                    let shift = (index - 16) / 16;
                    let mantissa = (index - 16) % 16;
                    // u128 also represents the upper edge of the final bucket.
                    (((17 + mantissa) as u128) << shift) - 1
                };
                return upper.min(u128::from(self.max)) as u64;
            }
        }
        self.max
    }
}
#[derive(Default)]
struct Gauge {
    last: usize,
    max: usize,
}
#[derive(Default)]
struct Interval {
    stages: BTreeMap<&'static str, Stage>,
    counters: BTreeMap<&'static str, u64>,
    gauges: BTreeMap<&'static str, Gauge>,
}
static METRICS: OnceLock<Mutex<Interval>> = OnceLock::new();
pub fn init(enabled: bool) {
    if !enabled || METRICS.set(Mutex::default()).is_err() {
        return;
    }
    std::thread::spawn(|| loop {
        std::thread::sleep(Duration::from_secs(2));
        let interval = std::mem::take(&mut *METRICS.get().unwrap().lock().unwrap());
        for (name, stage) in interval.stages {
            tracing::info!(
                stage = name,
                samples = stage.count,
                avg_ms = stage.total as f64 / stage.count as f64 / 1000.,
                max_ms = stage.max as f64 / 1000.,
                p50_ms = stage.percentile(50) as f64 / 1000.,
                p95_ms = stage.percentile(95) as f64 / 1000.,
                p99_ms = stage.percentile(99) as f64 / 1000.,
                "pipeline timing"
            );
        }
        for (name, count) in interval.counters {
            tracing::info!(counter = name, count, "pipeline counter");
        }
        for (name, gauge) in interval.gauges {
            tracing::info!(
                gauge = name,
                value = gauge.last,
                max = gauge.max,
                "pipeline gauge"
            );
        }
    });
}
pub fn record(name: &'static str, duration: Duration) {
    if let Some(metrics) = METRICS.get() {
        metrics
            .lock()
            .unwrap()
            .stages
            .entry(name)
            .or_default()
            .record(duration);
    }
}
pub fn increment(name: &'static str) {
    if let Some(metrics) = METRICS.get() {
        *metrics.lock().unwrap().counters.entry(name).or_default() += 1;
    }
}
pub fn gauge(name: &'static str, value: usize) {
    if let Some(metrics) = METRICS.get() {
        let mut metrics = metrics.lock().unwrap();
        let gauge = metrics.gauges.entry(name).or_default();
        gauge.last = value;
        gauge.max = gauge.max.max(value);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn percentiles_keep_tail_latency_and_bound_rounding() {
        let mut stage = Stage::default();
        for _ in 0..94 {
            stage.record(Duration::from_micros(100));
        }
        for _ in 0..5 {
            stage.record(Duration::from_millis(100));
        }
        stage.record(Duration::from_secs(1));
        assert!((100..=106).contains(&stage.percentile(50)));
        assert!((100_000..=106_250).contains(&stage.percentile(95)));
        assert!((100_000..=106_250).contains(&stage.percentile(99)));
        assert_eq!(stage.percentile(100), 1_000_000);
        for us in [
            0,
            1,
            15,
            16,
            17,
            31,
            32,
            33,
            255,
            256,
            100_000,
            u64::MAX / 2,
            u64::MAX,
        ] {
            let mut sample = Stage::default();
            sample.record(Duration::from_micros(us));
            assert_eq!(sample.percentile(95), us); // clamp to the real maximum
        }
        let mut huge = Stage::default();
        huge.record(Duration::MAX);
        assert_eq!(huge.percentile(99), u64::MAX);
    }
}
