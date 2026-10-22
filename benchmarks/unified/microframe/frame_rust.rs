// Protected sidecar ABI v2 and entry; the producer implements only rayon_frame.rs.
// See microframe.h for the frame contract.
use afcore_sys::AfRunCfg;
use std::ffi::{CString,c_char};
use std::sync::atomic::{AtomicBool,Ordering};
static STOP_FAILED:AtomicBool=AtomicBool::new(false);
#[repr(C)] pub struct MfFrame { _opaque: [u8;0] }
#[allow(dead_code)] pub const MF_KIND_M1:u32=1;
#[allow(dead_code)] pub const MF_KIND_M4:u32=2;
#[allow(dead_code)] pub const MF_KIND_M6:u32=3;
#[allow(dead_code)] pub const MF_KIND_GOL:u32=4;
#[repr(C)] #[derive(Clone,Copy,Default,Debug)]
pub struct MfBatchShape { pub kind:u32, pub items:u32, pub waves:u32, pub grain:u32, pub width:u32 }
#[repr(C)] struct MfAdapter {
    name:*const c_char,
    start:unsafe extern "C" fn(*const AfRunCfg)->i32,
    frame:unsafe extern "C" fn(*mut MfFrame)->i32,
    stop:unsafe extern "C" fn(),
}
#[allow(dead_code)]
unsafe extern "C" {
    pub fn mf_batch_count(frame:*const MfFrame)->u32;
    pub fn mf_batch_shape(frame:*const MfFrame,batch:u32,out:*mut MfBatchShape);
    pub fn mf_execute(frame:*mut MfFrame,batch:u32,wave:u32,begin:u32,end:u32);
    fn mf_main(argc:i32,argv:*mut *mut c_char,adapter:*const MfAdapter)->i32;
}
#[path="rayon_frame.rs"] mod implementation;
unsafe extern "C" fn start(cfg:*const AfRunCfg)->i32 {
    std::panic::catch_unwind(||unsafe{implementation::start(&*cfg)}).unwrap_or(2)
}
unsafe extern "C" fn frame(f:*mut MfFrame)->i32 {
    std::panic::catch_unwind(||unsafe{implementation::frame(f)}).unwrap_or(2)
}
unsafe extern "C" fn stop() { if std::panic::catch_unwind(||unsafe{implementation::stop()}).is_err(){STOP_FAILED.store(true,Ordering::Relaxed);} }
fn main() {
    let args:Vec<CString>=std::env::args().map(|s|CString::new(s).unwrap()).collect();
    let mut argv:Vec<*mut c_char>=args.iter().map(|s|s.as_ptr() as *mut c_char).collect();
    let adapter=MfAdapter{name:c"rayon".as_ptr(),start,frame,stop};
    let code=unsafe{mf_main(argv.len() as i32,argv.as_mut_ptr(),&adapter)};
    std::process::exit(if STOP_FAILED.load(Ordering::Relaxed){2}else{code});
}
