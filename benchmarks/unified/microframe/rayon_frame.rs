// Neutral producer stub. Scheduling implementation belongs here.
use afcore_sys::{AfRunCfg,AF_ERROR};
use crate::MfFrame;
pub unsafe fn start(_cfg:&AfRunCfg)->i32 {AF_ERROR}
pub unsafe fn frame(_frame:*mut MfFrame)->i32 {AF_ERROR}
pub unsafe fn stop() {}
