//! Bounded UDP send pacing. Delivery ACKs include receiver/control scheduling;
//! they are not a network RTT measurement or a transport capacity estimate.
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Change {
    Repair,
    Delay,
    Recovery,
}

pub struct Pacing {
    ceiling: u64,
    floor: u64,
    rate: u64,
    next: Instant,
    baseline_us: Option<u64>,
    baseline_since: Instant,
    smoothed_us: u64,
    healthy: u32,
    last_growth: Instant,
    last_backoff: Option<Instant>,
    last_repair: Option<Instant>,
}
impl Pacing {
    pub fn new(mbps: u32, now: Instant) -> Self {
        Self::with_ceiling(u64::from(mbps.max(1)) * 1_000_000, now)
    }
    pub(super) fn with_ceiling(ceiling: u64, now: Instant) -> Self {
        let ceiling = ceiling.max(1);
        Self {
            ceiling,
            floor: ceiling.min(2_000_000),
            rate: ceiling,
            next: now,
            baseline_us: None,
            baseline_since: now,
            smoothed_us: 0,
            healthy: 0,
            last_growth: now,
            last_backoff: None,
            last_repair: None,
        }
    }
    pub fn kbps(&self) -> u64 {
        self.rate / 1000
    }
    pub fn reserve(&mut self, now: Instant, wire_bytes: usize) -> Instant {
        let slot = self.next.max(now);
        let nanos = (wire_bytes as u128 * 8 * 1_000_000_000).div_ceil(u128::from(self.rate));
        self.next = slot + Duration::from_nanos(nanos.min(u128::from(u64::MAX)) as u64);
        slot
    }
    fn reduce(&mut self, now: Instant, reason: Change, cooldown: Duration) -> Option<Change> {
        self.healthy = 0;
        self.last_growth = now;
        if self
            .last_backoff
            .is_some_and(|last| now.duration_since(last) < cooldown)
        {
            return None;
        }
        self.last_backoff = Some(now);
        let next = (self.rate * 3 / 4).max(self.floor);
        if next == self.rate {
            return None;
        }
        self.rate = next;
        Some(reason)
    }
    pub fn repair(&mut self, now: Instant) -> Option<Change> {
        self.last_repair = Some(now);
        self.reduce(now, Change::Repair, Duration::from_millis(250))
    }
    pub fn acknowledge(&mut self, now: Instant, delivery: Duration) -> Option<Change> {
        let us = delivery.as_micros().min(u128::from(u64::MAX / 8)) as u64;
        // Refresh the minimum periodically so a permanent path/receiver delay
        // change does not suppress recovery forever.
        if now.duration_since(self.baseline_since) >= Duration::from_secs(10) {
            self.baseline_us = None;
            self.baseline_since = now;
        }
        let baseline = self.baseline_us.map_or(us, |old| old.min(us));
        self.baseline_us = Some(baseline);
        self.smoothed_us = if self.smoothed_us == 0 {
            us
        } else {
            (self.smoothed_us * 7 + us) / 8
        };
        let allowance = (baseline / 2).max(50_000);
        if self.smoothed_us > baseline.saturating_add(allowance) {
            return self.reduce(now, Change::Delay, Duration::from_secs(1));
        }
        self.healthy = self.healthy.saturating_add(1);
        if self.healthy < 4
            || now.duration_since(self.last_growth) < Duration::from_millis(500)
            || self
                .last_repair
                .is_some_and(|last| now.duration_since(last) < Duration::from_secs(1))
        {
            return None;
        }
        self.last_growth = now;
        self.healthy = 0;
        let next = (self.rate + (self.rate / 20).max(250_000)).min(self.ceiling);
        if next == self.rate {
            return None;
        }
        self.rate = next;
        Some(Change::Recovery)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn repairs_back_off_once_per_cooldown_and_recover_within_ceiling() {
        let start = Instant::now();
        let mut pacing = Pacing::new(80, start);
        assert_eq!(pacing.kbps(), 80_000);
        assert_eq!(pacing.repair(start), Some(Change::Repair));
        assert_eq!(pacing.kbps(), 60_000);
        assert_eq!(pacing.repair(start + Duration::from_millis(100)), None);
        assert_eq!(pacing.kbps(), 60_000);
        for i in 1..100 {
            pacing.repair(start + Duration::from_millis(i * 250));
        }
        assert_eq!(pacing.kbps(), 2_000);
        for i in 1..2000 {
            pacing.acknowledge(
                start + Duration::from_secs(25) + Duration::from_millis(i * 100),
                Duration::from_millis(20),
            );
        }
        assert_eq!(pacing.kbps(), 80_000);
        let mut low = Pacing::new(1, start);
        low.repair(start);
        assert_eq!(low.kbps(), 1_000);
    }

    #[test]
    fn delivery_delay_inflation_defers_growth_and_reduces_rate() {
        let start = Instant::now();
        let mut pacing = Pacing::new(80, start);
        pacing.acknowledge(start, Duration::from_millis(20));
        let mut changed = None;
        for i in 0..4 {
            changed = changed.or(pacing.acknowledge(
                start + Duration::from_millis(1000 + i * 100),
                Duration::from_millis(200),
            ));
        }
        assert_eq!(changed, Some(Change::Delay));
        assert_eq!(pacing.kbps(), 60_000);
        // A new stable path delay eventually establishes a fresh baseline.
        for i in 1..200 {
            pacing.acknowledge(
                start + Duration::from_secs(10) + Duration::from_millis(i * 100),
                Duration::from_millis(200),
            );
        }
        assert!(pacing.kbps() > 60_000);
    }

    #[test]
    fn wire_bytes_are_paced_across_message_boundaries_without_catchup_burst() {
        let start = Instant::now();
        let mut pacing = Pacing::new(1, start);
        assert_eq!(pacing.reserve(start, 1000), start);
        assert_eq!(pacing.reserve(start, 500), start + Duration::from_millis(8));
        assert_eq!(
            pacing.reserve(start, 1000),
            start + Duration::from_millis(12)
        );
        let later = start + Duration::from_secs(1);
        assert_eq!(pacing.reserve(later, 1000), later);
        assert_eq!(
            pacing.reserve(later, 1000),
            later + Duration::from_millis(8)
        );
    }
}
