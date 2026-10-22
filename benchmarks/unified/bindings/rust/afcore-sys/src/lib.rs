use std::ffi::{CString, c_char, c_int, c_void};
use std::mem::{align_of, offset_of, size_of};
use std::panic::{AssertUnwindSafe, catch_unwind};

pub mod pool_rayon;

pub const AF_ABI_VERSION: u32 = 1;
pub const AUDIT: bool = cfg!(feature = "audit");
pub const AF_OK: c_int = 0;
pub const AF_UNSUPPORTED: c_int = 1;
pub const AF_ERROR: c_int = 2;
pub const AF_WAIT_SPIN: u32 = 0;
pub const AF_WAIT_PARK: u32 = 1;
pub const AF_WAIT_FOREVER: u32 = 2;
pub const AF_WAIT_NATIVE: u32 = 3;
pub const AF_WORKER_AUTO: u32 = u32::MAX;

#[repr(C)]
pub struct AfRunCfg {
    pub workers: u32,
    pub worker_cpus: *const c_int,
    pub control_cpu: c_int,
    pub wait_mode: u32,
    pub knobs_json: *const c_char,
}

#[repr(C)]
pub struct AfAudit {
    pub cell_callbacks: u64,
    pub aux_callbacks: u64,
    pub foreign_callbacks: u64,
    pub nested: u64,
    pub duplicate_or_order: u64,
    pub missing: u64,
    pub value_mismatch: u64,
}

#[repr(C)]
pub struct AfGolImpl {
    pub abi_version: u32,
    pub self_: *mut c_void,
    pub start: Option<unsafe extern "C" fn(*mut c_void, *const AfRunCfg) -> c_int>,
    pub load: Option<unsafe extern "C" fn(*mut c_void, *const u8, u32, u32) -> c_int>,
    pub run: Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
    pub store: Option<unsafe extern "C" fn(*mut c_void, *mut u8) -> c_int>,
    pub telemetry: Option<unsafe extern "C" fn(*mut c_void, *mut c_char, usize) -> c_int>,
    pub stop: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
}

macro_rules! layout {
    ($ty:ty, $size:expr, $( $field:ident: $field_ty:ty => $offset:expr ),+ $(,)?) => {
        const _: () = {
            assert!(size_of::<$ty>() == $size);
            assert!(align_of::<$ty>() == 8);
            $(assert!(offset_of!($ty, $field) == $offset);
              assert!(size_of::<$field_ty>() == size_of_val_field!($ty, $field));)+
        };
    };
}
macro_rules! size_of_val_field {
    ($ty:ty, $field:ident) => {{
        let value = std::mem::MaybeUninit::<$ty>::uninit();
        let field = unsafe { &raw const (*value.as_ptr()).$field };
        // Pointers preserve the field type without reading uninitialized storage.
        const fn pointed_size<T>(_: *const T) -> usize { size_of::<T>() }
        pointed_size(field)
    }};
}
layout!(AfRunCfg, 32, workers: u32 => 0, worker_cpus: *const c_int => 8,
        control_cpu: c_int => 16, wait_mode: u32 => 20, knobs_json: *const c_char => 24);
layout!(AfAudit, 56, cell_callbacks: u64 => 0, aux_callbacks: u64 => 8,
        foreign_callbacks: u64 => 16, nested: u64 => 24, duplicate_or_order: u64 => 32,
        missing: u64 => 40, value_mismatch: u64 => 48);
layout!(AfGolImpl, 64, abi_version: u32 => 0, self_: *mut c_void => 8,
        start: usize => 16, load: usize => 24, run: usize => 32, store: usize => 40,
        telemetry: usize => 48, stop: usize => 56);

unsafe extern "C" {
    pub fn af_gol_main(argc: c_int, argv: *mut *mut c_char, implementation: *const AfGolImpl) -> c_int;
    pub fn af_pin_self(cpu: c_int) -> c_int;
    pub fn af_worker_enter(index: u32) -> c_int;
    pub fn af_worker_adopt() -> c_int;
    /// Open-hosting runs only; see afcore.h.
    pub fn af_worker_register(index: u32) -> c_int;
    pub fn af_workers_rendezvous(deadline_ns: u64) -> c_int;
    pub fn af_workers_registered() -> u32;
    pub fn af_audit_cell(generation: u32, cell: u32, next: u8);
    pub fn af_audit_cell_begin(generation: u32, cell: u32);
    pub fn af_audit_cell_end(next: u8);
    pub fn af_audit_aux();
    pub fn af_gol_init(seed: u64, density_pct: u32, w: u32, h: u32, out: *mut u8);
    pub fn af_gol_ref_run(input: *const u8, out: *mut u8, w: u32, h: u32, generations: u32);
    pub fn af_gol_hash(grid: *const u8, n: usize) -> u64;
}

#[inline(always)]
pub fn audit_cell(generation: u32, cell: u32, next: u8) {
    #[cfg(feature = "audit")]
    unsafe { af_audit_cell(generation, cell, next); }
    #[cfg(not(feature = "audit"))]
    let _ = (generation, cell, next);
}

#[inline(always)]
pub fn audit_cell_begin(generation: u32, cell: u32) {
    #[cfg(feature = "audit")]
    unsafe { af_audit_cell_begin(generation, cell); }
    #[cfg(not(feature = "audit"))]
    let _ = (generation, cell);
}

#[inline(always)]
pub fn audit_cell_end(next: u8) {
    #[cfg(feature = "audit")]
    unsafe { af_audit_cell_end(next); }
    #[cfg(not(feature = "audit"))]
    let _ = next;
}

#[inline(always)]
pub fn audit_aux() {
    #[cfg(feature = "audit")]
    unsafe { af_audit_aux(); }
}

/// Implementors must also implement `Default`, which the bridge uses to construct them.
pub trait GolImpl: Default {
    fn start(&mut self, cfg: &AfRunCfg) -> c_int;
    fn load(&mut self, grid: &[u8], w: u32, h: u32) -> c_int;
    fn run(&mut self, generations: u32) -> c_int;
    fn store(&mut self, out: &mut [u8]) -> c_int;
    fn telemetry(&mut self, json: &mut [u8]) -> c_int {
        if json.len() < 3 { return AF_ERROR; }
        json[..3].copy_from_slice(b"{}\0");
        AF_OK
    }
    fn stop(&mut self) -> c_int;
}

fn status(callback: impl FnOnce() -> c_int) -> c_int {
    match catch_unwind(AssertUnwindSafe(callback)) {
        Ok(value) => value,
        Err(payload) => {
            // The payload's Drop could panic again and unwind across the C trampoline.
            std::mem::forget(payload);
            AF_ERROR
        }
    }
}

struct Bridge<T> {
    value: T,
    cells: usize,
}

impl<T: GolImpl> Bridge<T> {
    unsafe extern "C" fn start(this: *mut c_void, cfg: *const AfRunCfg) -> c_int {
        status(|| unsafe { (&mut *this.cast::<Self>()).value.start(&*cfg) })
    }
    unsafe extern "C" fn load(this: *mut c_void, grid: *const u8, w: u32, h: u32) -> c_int {
        status(|| unsafe {
            let this = &mut *this.cast::<Self>();
            let cells = w as usize * h as usize;
            let result = this.value.load(std::slice::from_raw_parts(grid, cells), w, h);
            if result == AF_OK { this.cells = cells; }
            result
        })
    }
    unsafe extern "C" fn run(this: *mut c_void, generations: u32) -> c_int {
        status(|| unsafe { (&mut *this.cast::<Self>()).value.run(generations) })
    }
    unsafe extern "C" fn store(this: *mut c_void, out: *mut u8) -> c_int {
        status(|| unsafe {
            let this = &mut *this.cast::<Self>();
            this.value.store(std::slice::from_raw_parts_mut(out, this.cells))
        })
    }
    unsafe extern "C" fn telemetry(this: *mut c_void, json: *mut c_char, cap: usize) -> c_int {
        status(|| unsafe {
            (&mut *this.cast::<Self>()).value.telemetry(std::slice::from_raw_parts_mut(json.cast(), cap))
        })
    }
    unsafe extern "C" fn stop(this: *mut c_void) -> c_int {
        status(|| unsafe { (&mut *this.cast::<Self>()).value.stop() })
    }
    fn table(&mut self) -> AfGolImpl {
        AfGolImpl {
            abi_version: AF_ABI_VERSION, self_: (self as *mut Self).cast(),
            start: Some(Self::start), load: Some(Self::load), run: Some(Self::run),
            store: Some(Self::store), telemetry: Some(Self::telemetry), stop: Some(Self::stop),
        }
    }
}

pub fn gol_main<T: GolImpl>() -> c_int {
    status(|| {
        use std::os::unix::ffi::OsStrExt;
        let args: Vec<_> = std::env::args_os()
            .map(|arg| CString::new(arg.as_bytes()).expect("argument contains NUL")).collect();
        let mut argv: Vec<_> = args.iter().map(|arg| arg.as_ptr().cast_mut()).collect();
        argv.push(std::ptr::null_mut());
        let mut bridge = Bridge { value: T::default(), cells: 0 };
        let table = bridge.table();
        unsafe { af_gol_main(args.len() as c_int, argv.as_mut_ptr(), &table) }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg() -> AfRunCfg {
        AfRunCfg { workers: 1, worker_cpus: std::ptr::null(), control_cpu: 15,
                   wait_mode: AF_WAIT_NATIVE, knobs_json: c"{}".as_ptr() }
    }

    #[derive(Default)]
    struct Panics;
    impl GolImpl for Panics {
        fn start(&mut self, _: &AfRunCfg) -> i32 { panic!("start sentinel") }
        fn load(&mut self, _: &[u8], _: u32, _: u32) -> i32 { panic!("load sentinel") }
        fn run(&mut self, _: u32) -> i32 { panic!("run sentinel") }
        fn store(&mut self, _: &mut [u8]) -> i32 { panic!("store sentinel") }
        fn telemetry(&mut self, _: &mut [u8]) -> i32 { panic!("telemetry sentinel") }
        fn stop(&mut self) -> i32 { panic!("stop sentinel") }
    }
    fn panics() -> Bridge<Panics> { Bridge { value: Panics, cells: 1 } }

    #[test]
    fn start_trampoline_contains_panic() {
        let mut bridge = panics();
        let table = bridge.table();
        assert_eq!(unsafe { table.start.unwrap()(table.self_, &cfg()) }, AF_ERROR);
    }
    #[test]
    fn start_trampoline_contains_panicking_payload_drop() {
        struct Payload;
        impl Drop for Payload {
            fn drop(&mut self) { panic!("panic payload destructor"); }
        }
        #[derive(Default)]
        struct PanickingPayload;
        impl GolImpl for PanickingPayload {
            fn start(&mut self, _: &AfRunCfg) -> i32 { std::panic::panic_any(Payload) }
            fn load(&mut self, _: &[u8], _: u32, _: u32) -> i32 { AF_OK }
            fn run(&mut self, _: u32) -> i32 { AF_OK }
            fn store(&mut self, _: &mut [u8]) -> i32 { AF_OK }
            fn stop(&mut self) -> i32 { AF_OK }
        }
        let worker_cpus = [0];
        let cfg = AfRunCfg { worker_cpus: worker_cpus.as_ptr(), ..cfg() };
        let mut bridge = Bridge { value: PanickingPayload::default(), cells: 0 };
        let table = bridge.table();
        assert_eq!(unsafe { table.start.unwrap()(table.self_, &cfg) }, AF_ERROR);
    }
    #[test]
    fn load_trampoline_contains_panic() {
        let mut bridge = panics();
        let table = bridge.table();
        assert_eq!(unsafe { table.load.unwrap()(table.self_, [0].as_ptr(), 1, 1) }, AF_ERROR);
    }
    #[test]
    fn run_trampoline_contains_panic() {
        let mut bridge = panics();
        let table = bridge.table();
        assert_eq!(unsafe { table.run.unwrap()(table.self_, 0) }, AF_ERROR);
    }
    #[test]
    fn store_trampoline_contains_panic() {
        let mut bridge = panics();
        let table = bridge.table();
        assert_eq!(unsafe { table.store.unwrap()(table.self_, [0].as_mut_ptr()) }, AF_ERROR);
    }
    #[test]
    fn telemetry_trampoline_contains_panic() {
        let mut bridge = panics();
        let table = bridge.table();
        assert_eq!(unsafe { table.telemetry.unwrap()(table.self_, [0u8; 8].as_mut_ptr().cast(), 8) },
                   AF_ERROR);
    }
    #[test]
    fn stop_trampoline_contains_panic() {
        let mut bridge = panics();
        let table = bridge.table();
        assert_eq!(unsafe { table.stop.unwrap()(table.self_) }, AF_ERROR);
    }

    #[derive(Default)]
    struct Bytes(Vec<u8>);
    impl GolImpl for Bytes {
        fn start(&mut self, _: &AfRunCfg) -> i32 { AF_OK }
        fn load(&mut self, grid: &[u8], _: u32, _: u32) -> i32 {
            self.0 = grid.to_vec(); AF_OK
        }
        fn run(&mut self, generations: u32) -> i32 { assert_eq!(generations, 0); AF_OK }
        fn store(&mut self, out: &mut [u8]) -> i32 { out.copy_from_slice(&self.0); AF_OK }
        fn stop(&mut self) -> i32 { AF_OK }
    }
    #[test]
    fn c_table_preserves_initialized_bytes_after_zero_generations() {
        let mut bridge = Bridge { value: Bytes::default(), cells: 0 };
        let table = bridge.table();
        assert_eq!(table.abi_version, AF_ABI_VERSION);
        assert_eq!(unsafe { table.start.unwrap()(table.self_, &cfg()) }, AF_OK);
        for (w, h) in [(8, 8), (256, 256), (1, 1)] {
            for seed in [0x5eed, 1, 2] {
                let mut initial = vec![0; (w * h) as usize];
                unsafe { af_gol_init(seed, 35, w, h, initial.as_mut_ptr()); }
                let mut actual = vec![0xff; initial.len() + 2];
                assert_eq!(unsafe { table.load.unwrap()(table.self_, initial.as_ptr(), w, h) }, AF_OK);
                assert_eq!(unsafe { table.run.unwrap()(table.self_, 0) }, AF_OK);
                assert_eq!(unsafe { table.store.unwrap()(table.self_, actual[1..].as_mut_ptr()) }, AF_OK);
                assert_eq!(&actual[1..actual.len() - 1], initial);
                assert_eq!((actual[0], actual[actual.len() - 1]), (0xff, 0xff));
            }
        }
        assert_eq!(unsafe { table.stop.unwrap()(table.self_) }, AF_OK);
    }
    #[test]
    fn default_telemetry_is_terminated_and_rejects_short_buffers() {
        let mut bridge = Bridge { value: Bytes::default(), cells: 0 };
        let table = bridge.table();
        for cap in 0..3 {
            let mut out = [0xffu8; 4];
            assert_eq!(unsafe { table.telemetry.unwrap()(table.self_, out.as_mut_ptr().cast(), cap) },
                       AF_ERROR);
            assert_eq!(out, [0xff; 4]);
        }
        let mut out = [0xffu8; 4];
        assert_eq!(unsafe { table.telemetry.unwrap()(table.self_, out.as_mut_ptr().cast(), 3) }, AF_OK);
        assert_eq!(&out, b"{}\0\xff");
    }
}
