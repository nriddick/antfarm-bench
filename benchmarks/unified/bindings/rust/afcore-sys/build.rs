use std::{env, path::PathBuf};

fn main() {
    let directory = PathBuf::from(env::var_os("AFCORE_LIB_DIR").expect("AFCORE_LIB_DIR is required"));
    assert!(directory.is_absolute(), "AFCORE_LIB_DIR must be absolute");
    let name = if env::var_os("CARGO_FEATURE_AUDIT").is_some() {
        "afcore-audit"
    } else {
        "afcore"
    };
    let archive = directory.join(format!("lib{name}.a"));
    println!("cargo:rerun-if-env-changed=AFCORE_LIB_DIR");
    println!("cargo:rerun-if-changed={}", archive.display());
    println!("cargo:rustc-link-search=native={}", directory.display());
    println!("cargo:rustc-link-lib=static={name}");
    println!("cargo:rustc-link-lib=pthread");
}
