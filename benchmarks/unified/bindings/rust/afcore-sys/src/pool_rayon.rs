use crate::{AF_ERROR, AF_OK, AfRunCfg, af_worker_enter, af_workers_registered, af_workers_rendezvous, status};
use std::sync::mpsc;
use std::time::{Duration, Instant};

pub struct RayonPool {
    pool: rayon::ThreadPool,
}

impl RayonPool {
    pub fn new(cfg: &AfRunCfg) -> Result<Self, i32> {
        let mut now = libc::timespec { tv_sec: 0, tv_nsec: 0 };
        if unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut now) } != 0 {
            return Err(AF_ERROR);
        }
        let deadline_ns = now.tv_sec as u64 * 1_000_000_000 + now.tv_nsec as u64 + 5_000_000_000;
        let deadline = Instant::now() + Duration::from_secs(5);
        let (sender, receiver) = mpsc::channel();
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(cfg.workers as usize)
            .start_handler(move |index| {
                let result = status(|| unsafe {
                    if af_worker_enter(index as u32) != AF_OK { return AF_ERROR; }
                    af_workers_rendezvous(deadline_ns)
                });
                let _ = sender.send(result);
            })
            .build().map_err(|_| AF_ERROR)?;
        // Rayon primes its internal latch before invoking the start handler.
        for _ in 0..cfg.workers {
            let remaining = deadline.saturating_duration_since(Instant::now());
            match receiver.recv_timeout(remaining) {
                Ok(AF_OK) => (),
                _ => return Err(AF_ERROR),
            }
        }
        if unsafe { af_workers_registered() } != cfg.workers { return Err(AF_ERROR); }
        Ok(Self { pool })
    }

    pub fn run<F, R>(&self, callback: F) -> R
    where F: FnOnce() -> R + Send, R: Send {
        self.pool.install(callback)
    }
}
