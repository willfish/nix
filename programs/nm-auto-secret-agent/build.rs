use std::{env, path::PathBuf, process::Command};
fn main() {
    println!("cargo:rerun-if-changed=native/bridge.c");
    println!("cargo:rerun-if-changed=native/bridge.h");
    println!("cargo:rerun-if-env-changed=NM_AGENT_NMCLI");
    println!("cargo:rerun-if-env-changed=NM_AGENT_C_SANITIZE");
    let output = Command::new("pkg-config")
        .args(["--cflags", "--libs", "libnm", "gio-unix-2.0"])
        .output()
        .expect("pkg-config is required");
    assert!(
        output.status.success(),
        "libnm development libraries are required"
    );
    let flags = String::from_utf8(output.stdout).unwrap();
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=nm_agent_bridge");
    let mut cc = Command::new(env::var("CC").unwrap_or_else(|_| "cc".into()));
    cc.args([
        "-std=c17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
        "-fPIC",
        "-O2",
        "-c",
        "native/bridge.c",
        "-o",
    ])
    .arg(out.join("bridge.o"));
    if env::var_os("NM_AGENT_C_SANITIZE").is_some() {
        assert!(
            env::var_os("CARGO_FEATURE_FIXTURES").is_some(),
            "sanitizers are manual fixtures only"
        );
        cc.args([
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-g",
        ]);
        println!("cargo:rustc-link-lib=asan");
        println!("cargo:rustc-link-lib=ubsan");
        println!("cargo:rustc-link-arg=-no-pie");
    }
    for flag in flags.split_whitespace() {
        if let Some(path) = flag.strip_prefix("-I") {
            cc.arg("-isystem").arg(path);
        }
        if flag.starts_with("-D") || flag == "-pthread" {
            cc.arg(flag);
        }
        if let Some(path) = flag.strip_prefix("-L") {
            println!("cargo:rustc-link-search=native={path}");
        }
        if let Some(name) = flag.strip_prefix("-l") {
            println!("cargo:rustc-link-lib={name}");
        }
    }
    assert!(
        cc.status().unwrap().success(),
        "C bridge compilation failed"
    );
    assert!(
        Command::new(env::var("AR").unwrap_or_else(|_| "ar".into()))
            .arg("crs")
            .arg(out.join("libnm_agent_bridge.a"))
            .arg(out.join("bridge.o"))
            .status()
            .unwrap()
            .success()
    );
    println!(
        "cargo:rustc-env=NM_AGENT_NMCLI={}",
        env::var("NM_AGENT_NMCLI").unwrap_or_else(|_| "nmcli".into())
    );
}
