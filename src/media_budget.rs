//! Admission for owned media workspaces, GL targets and estimated codec/CUDA resources.
//! Tickets follow resources and caches; no graphics/codec operation runs under this lock.
use crate::metrics;
use std::{
    collections::BTreeMap,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Cost {
    pub cpu: usize,
    pub gpu: usize,
    pub encoders: usize,
}
impl Cost {
    pub fn cpu(cpu: usize) -> Self {
        Self {
            cpu,
            ..Self::default()
        }
    }
    pub fn gpu(gpu: usize) -> Self {
        Self {
            gpu,
            ..Self::default()
        }
    }
    pub fn add(self, other: Self) -> Option<Self> {
        Some(Self {
            cpu: self.cpu.checked_add(other.cpu)?,
            gpu: self.gpu.checked_add(other.gpu)?,
            encoders: self.encoders.checked_add(other.encoders)?,
        })
    }
    fn fits(self, limit: Self) -> bool {
        self.cpu <= limit.cpu && self.gpu <= limit.gpu && self.encoders <= limit.encoders
    }
    fn subtract(&mut self, other: Self) {
        self.cpu -= other.cpu;
        self.gpu -= other.gpu;
        self.encoders -= other.encoders;
    }
}
#[derive(Clone, Copy)]
pub struct Pressure {
    pub window: u32,
    cost: Cost,
    pub global: bool,
    pub gpu: bool,
    pub reclaim: bool,
}
#[derive(Default)]
struct State {
    used: Cost,
    windows: BTreeMap<u32, Cost>,
    focused: Option<u32>,
    pressure: Option<Pressure>,
    last_reclaim: Option<Instant>,
}
struct Inner {
    state: Mutex<State>,
    global: Cost,
    window: Cost,
}
#[derive(Clone)]
pub struct Budget(Arc<Inner>);
pub struct Lease {
    owner: Budget,
    window: u32,
    cost: Cost,
}
impl Budget {
    pub fn new(config: &waybridge_config::MediaBudget) -> Self {
        let mib = 1024 * 1024;
        Self(Arc::new(Inner {
            state: Mutex::default(),
            global: Cost {
                cpu: config.cpu_mib as usize * mib,
                gpu: config.gpu_mib as usize * mib,
                encoders: config.encoders as usize,
            },
            window: Cost {
                cpu: config.window_cpu_mib as usize * mib,
                gpu: config.window_gpu_mib as usize * mib,
                encoders: 2,
            },
        }))
    }
    pub fn fits_limits(&self, cost: Cost) -> bool {
        cost.fits(self.0.global) && cost.fits(self.0.window)
    }
    pub fn acquire(&self, window: u32, cost: Cost) -> Option<Lease> {
        let mut state = self.0.state.lock().unwrap();
        let total = state.used.add(cost)?;
        let local = state
            .windows
            .get(&window)
            .copied()
            .unwrap_or_default()
            .add(cost)?;
        if !cost.fits(self.0.global) || !cost.fits(self.0.window) {
            metrics::increment("media_budget_request_exceeds_limit");
            return None;
        }
        if !total.fits(self.0.global) || !local.fits(self.0.window) {
            if state.pressure.is_none() || state.focused == Some(window) {
                state.pressure = Some(Pressure {
                    window,
                    cost,
                    global: !total.fits(self.0.global),
                    gpu: total.gpu > self.0.global.gpu || local.gpu > self.0.window.gpu,
                    reclaim: true,
                });
            }
            metrics::increment("media_budget_wait");
            return None;
        }
        if state
            .pressure
            .is_some_and(|p| p.window == window && p.cost == cost)
        {
            state.pressure = None;
        }
        state.used = total;
        state.windows.insert(window, local);
        observe(total);
        Some(Lease {
            owner: self.clone(),
            window,
            cost,
        })
    }
    pub fn focus(&self, window: Option<u32>) {
        self.0.state.lock().unwrap().focused = window;
    }
    pub fn focused(&self) -> Option<u32> {
        self.0.state.lock().unwrap().focused
    }
    pub fn take_pressure(&self) -> Option<Pressure> {
        let mut state = self.0.state.lock().unwrap();
        if state
            .last_reclaim
            .is_some_and(|t| t.elapsed() < Duration::from_millis(250))
        {
            return None;
        }
        let mut request = state.pressure.take()?;
        let local = state
            .windows
            .get(&request.window)
            .copied()
            .unwrap_or_default();
        if state
            .used
            .add(request.cost)
            .is_some_and(|c| c.fits(self.0.global))
            && local
                .add(request.cost)
                .is_some_and(|c| c.fits(self.0.window))
        {
            // Capacity has returned: wake the static window, without evicting
            // any healthy target or encoder for stale pressure.
            request.reclaim = false;
            request.gpu = false;
            request.global = false;
        }
        state.last_reclaim = Some(Instant::now());
        Some(request)
    }
    #[cfg(test)]
    fn used(&self) -> Cost {
        self.0.state.lock().unwrap().used
    }
}
fn observe(cost: Cost) {
    metrics::gauge("media_cpu_reserved_bytes", cost.cpu);
    metrics::gauge("media_gpu_estimated_bytes", cost.gpu);
    metrics::gauge("media_encoder_slots", cost.encoders);
}
impl Lease {
    // Transfer already-reserved ownership without a second admission race.
    pub fn split(&mut self, cost: Cost) -> Self {
        self.cost.subtract(cost);
        Self {
            owner: self.owner.clone(),
            window: self.window,
            cost,
        }
    }
}
impl Drop for Lease {
    fn drop(&mut self) {
        if self.cost == Cost::default() {
            return;
        }
        let mut state = self.owner.0.state.lock().unwrap();
        state.used.subtract(self.cost);
        let local = state.windows.get_mut(&self.window).unwrap();
        local.subtract(self.cost);
        if *local == Cost::default() {
            state.windows.remove(&self.window);
        }
        observe(state.used);
    }
}

/// Estimates include padding and workspace margin, not opaque driver VRAM measurements.
pub fn cuda_pool(width: u32, height: u32, hdr: bool) -> Cost {
    let coded_width = (width as usize + 1) & !1;
    let rows = (height as usize + 1) & !1;
    let rgba_row = (coded_width * 4).div_ceil(256) * 256;
    let p010_row = (coded_width * 2).div_ceil(256) * 256;
    let pixels = width as usize * height as usize;
    let planes = if hdr {
        rgba_row * rows + p010_row * rows * 3 / 2
    } else {
        rgba_row * rows
    };
    Cost::gpu(planes * 4 + pixels * (if hdr { 2 } else { 1 }) + 4096)
}
pub fn encoder(width: u32, height: u32, hdr: bool, gpu_input: bool) -> Cost {
    let coded_width = (width as usize + 1) & !1;
    let coded_height = (height as usize + 1) & !1;
    let pixels = coded_width * coded_height;
    Cost {
        cpu: if gpu_input {
            0
        } else {
            (coded_width * 4).div_ceil(256) * 256 * coded_height.div_ceil(32) * 32 + 4096
        },
        gpu: pixels * (if hdr { 12 } else { 8 }) + 32 * 1024 * 1024,
        encoders: 1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn leases_cover_queues_workers_caches_and_cross_thread_final_release() {
        let config = waybridge_config::MediaBudget {
            cpu_mib: 4,
            window_cpu_mib: 3,
            gpu_mib: 4,
            window_gpu_mib: 3,
            encoders: 2,
        };
        let budget = Budget::new(&config);
        let frame = Arc::new(budget.acquire(1, Cost::cpu(2 * 1024 * 1024)).unwrap());
        let worker = frame.clone();
        drop(frame); // dequeue must not return the worker's allocation
        assert!(budget.acquire(1, Cost::cpu(2 * 1024 * 1024)).is_none());
        let cache = budget
            .acquire(
                2,
                Cost {
                    gpu: 1024,
                    encoders: 2,
                    ..Cost::default()
                },
            )
            .unwrap();
        assert!(budget
            .acquire(
                3,
                Cost {
                    encoders: 1,
                    ..Cost::default()
                }
            )
            .is_none());
        std::thread::spawn(move || drop(worker)).join().unwrap();
        assert_eq!(budget.used().cpu, 0);
        drop(cache);
        assert_eq!(budget.used(), Cost::default());
        assert!(budget.0.state.lock().unwrap().windows.is_empty());
    }
    #[test]
    fn impossible_requests_do_not_evict_and_recovered_capacity_only_wakes() {
        let config = waybridge_config::MediaBudget {
            cpu_mib: 4,
            window_cpu_mib: 3,
            gpu_mib: 4,
            window_gpu_mib: 3,
            encoders: 2,
        };
        let budget = Budget::new(&config);
        assert!(budget.acquire(9, Cost::cpu(5 * 1024 * 1024)).is_none());
        assert!(budget.take_pressure().is_none());
        let old = budget.acquire(1, Cost::cpu(2 * 1024 * 1024)).unwrap();
        assert!(budget.acquire(1, Cost::cpu(2 * 1024 * 1024)).is_none());
        drop(old);
        let wake = budget.take_pressure().unwrap();
        assert_eq!(wake.window, 1);
        assert!(!wake.reclaim);
        let lease = budget
            .acquire(
                1,
                Cost {
                    cpu: 1024,
                    gpu: 2048,
                    encoders: 1,
                },
            )
            .unwrap();
        let mut bundle = lease;
        let zero = bundle.split(Cost::default());
        let target = bundle.split(Cost::gpu(2048));
        assert_eq!(budget.used().gpu, 2048);
        drop(bundle);
        assert_eq!(budget.used().cpu, 0);
        assert_eq!(budget.used().gpu, 2048);
        drop(target);
        drop(zero);
        assert_eq!(budget.used(), Cost::default());
    }
}
