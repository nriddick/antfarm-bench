use afcore_sys::{AF_OK, AfRunCfg, GolImpl, pool_rayon::RayonPool};

#[derive(Default)]
struct Stub {
    pool: Option<RayonPool>,
    grid: Vec<u8>,
    next: Vec<u8>,
    w: u32,
    h: u32,
}

impl GolImpl for Stub {
    fn start(&mut self, cfg: &AfRunCfg) -> i32 {
        match RayonPool::new(cfg) {
            Ok(pool) => { self.pool = Some(pool); AF_OK }
            Err(status) => status,
        }
    }
    fn load(&mut self, grid: &[u8], w: u32, h: u32) -> i32 {
        self.w = w;
        self.h = h;
        self.grid = grid.to_vec();
        self.next.resize(grid.len(), 0);
        AF_OK
    }
    fn run(&mut self, generations: u32) -> i32 {
        unsafe {
            afcore_sys::af_gol_ref_run(self.grid.as_ptr(), self.next.as_mut_ptr(), self.w, self.h, generations);
        }
        std::mem::swap(&mut self.grid, &mut self.next);
        AF_OK
    }
    fn store(&mut self, out: &mut [u8]) -> i32 {
        out.copy_from_slice(&self.grid);
        AF_OK
    }
    fn stop(&mut self) -> i32 {
        self.pool = None;
        AF_OK
    }
}

fn main() {
    std::process::exit(afcore_sys::gol_main::<Stub>());
}
