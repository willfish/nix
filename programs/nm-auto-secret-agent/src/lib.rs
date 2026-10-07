use std::{
    ffi::{CStr, CString, c_char, c_int, c_void},
    io::{Read, Write},
    os::{fd::AsRawFd, unix::process::ExitStatusExt},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    thread,
    time::Duration,
};

#[repr(C)]
struct Callbacks {
    get: unsafe extern "C" fn(
        *mut c_void,
        *const c_char,
        *const c_char,
        *const c_char,
        *const c_char,
        u32,
        *mut *mut c_char,
        *mut *mut c_char,
    ),
    cancel: unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char),
    registered: unsafe extern "C" fn(*mut c_void, c_int, *const c_char),
    release: unsafe extern "C" fn(*mut c_char),
}
unsafe extern "C" {
    fn bridge_new(
        state: *mut c_void,
        callbacks: *const Callbacks,
        message: *mut *mut c_char,
    ) -> *mut c_void;
    fn bridge_start(bridge: *mut c_void);
    fn bridge_step();
    fn bridge_destroy(bridge: *mut c_void);
    fn bridge_free_error(message: *mut c_char);
    fn bridge_signal() -> c_int;
    fn bridge_nonblock(fd: c_int) -> c_int;
    fn bridge_printable(ch: u32) -> c_int;
}

fn log(message: impl std::fmt::Display) {
    let mut stderr = std::io::stderr().lock();
    let _ = writeln!(stderr, "nm-auto-secret-agent: {message}");
    let _ = stderr.flush();
}
fn quoted(value: &str) -> String {
    let quote = if value.contains('\'') && !value.contains('"') {
        '"'
    } else {
        '\''
    };
    let mut result = String::from(quote);
    for ch in value.chars() {
        match ch {
            '\\' => result.push_str("\\\\"),
            '\n' => result.push_str("\\n"),
            '\r' => result.push_str("\\r"),
            '\t' => result.push_str("\\t"),
            c if c == quote => {
                result.push('\\');
                result.push(c);
            }
            c if unsafe { bridge_printable(c as u32) } == 0 => {
                use std::fmt::Write;
                let n = c as u32;
                if n <= 255 {
                    let _ = write!(result, "\\x{n:02x}");
                } else if n <= 65535 {
                    let _ = write!(result, "\\u{n:04x}");
                } else {
                    let _ = write!(result, "\\U{n:08x}");
                }
            }
            c => result.push(c),
        }
    }
    result.push(quote);
    result
}
fn text(pointer: *const c_char) -> String {
    if pointer.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(pointer) }
            .to_string_lossy()
            .into_owned()
    }
}

fn lookup(program: &Path, key: &str) -> Result<String, String> {
    let mut child = Command::new(program)
        .args([
            "-s",
            "-g",
            "802-11-wireless-security.psk",
            "connection",
            "show",
            key,
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|error| format!("could not start system secret lookup ({:?})", error.kind()))?;
    let mut stdout = child.stdout.take().expect("piped stdout");
    let mut bytes = Vec::new();
    let result = (|| {
        if unsafe { bridge_nonblock(stdout.as_raw_fd()) } != 0 {
            return Err("could not read system secret lookup".to_owned());
        }
        let mut status = None;
        let mut eof = false;
        loop {
            if unsafe { bridge_signal() } != 0 {
                return Err("system secret lookup interrupted".to_owned());
            }
            let mut buffer = [0_u8; 8192];
            match stdout.read(&mut buffer) {
                Ok(0) => eof = true,
                Ok(n) => {
                    bytes.extend_from_slice(&buffer[..n]);
                    continue;
                }
                Err(e)
                    if matches!(
                        e.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
                    ) => {}
                Err(_) => return Err("could not read system secret lookup".to_owned()),
            }
            if status.is_none() {
                status = child
                    .try_wait()
                    .map_err(|_| "could not wait for system secret lookup".to_owned())?;
            }
            if let Some(status) = status.filter(|_| eof) {
                if !status.success() {
                    return Err(if let Some(code) = status.code() {
                        format!("system secret lookup exited with status {code}")
                    } else {
                        format!(
                            "system secret lookup terminated by signal {}",
                            status.signal().unwrap_or(0)
                        )
                    });
                }
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }
        let value = std::str::from_utf8(&bytes)
            .map_err(|_| "system secret lookup returned invalid UTF-8".to_owned())?;
        let value = value.replace("\r\n", "\n").replace('\r', "\n");
        let value = value.trim_end_matches('\n');
        if value.is_empty() {
            return Err("system connection has empty psk".into());
        }
        // Neither D-Bus nor the libnm property API accepts embedded NUL.
        if value.contains('\0') {
            return Err("system secret lookup returned an invalid string".into());
        }
        Ok(value.to_owned())
    })();
    if result.is_err() {
        let _ = child.kill();
        let _ = child.wait();
    }
    // Do not retain or print raw nmcli output on any failure path.
    bytes.fill(0);
    result
}
fn secrets(program: &Path, id: &str, uuid: &str, setting: &str) -> Result<String, String> {
    let key = if uuid.is_empty() { id } else { uuid };
    if key.is_empty() {
        return Err("connection has no uuid/id".into());
    }
    if setting != "802-11-wireless-security" {
        return Err(format!("unsupported setting for auto agent: {setting}"));
    }
    lookup(program, key)
}
struct State {
    program: PathBuf,
    ok: bool,
    quit: bool,
}
unsafe extern "C" fn get(
    state: *mut c_void,
    id: *const c_char,
    uuid: *const c_char,
    path: *const c_char,
    setting: *const c_char,
    flags: u32,
    secret: *mut *mut c_char,
    error: *mut *mut c_char,
) {
    let outcome = std::panic::catch_unwind(|| {
        let state = unsafe { &*(state.cast::<State>()) };
        let id = text(id);
        let uuid = text(uuid);
        let setting = text(setting);
        let display = if id.is_empty() { "?" } else { &id };
        log(format!(
            "GetSecrets id={} setting={} flags={flags} path={}",
            quoted(display),
            quoted(&setting),
            text(path)
        ));
        let result = secrets(&state.program, &id, &uuid, &setting);
        match &result {
            Ok(_) => log(format!(
                "GetSecrets ok id={} uuid={} (auto-submit)",
                quoted(display),
                quoted(&uuid)
            )),
            Err(e) => log(format!("GetSecrets failed for {}: {e}", quoted(display))),
        }
        result
    })
    .unwrap_or_else(|_| Err("system secret lookup failed".into()));
    let (target, value) = match outcome {
        Ok(s) => (secret, s),
        Err(e) => (error, e),
    };
    unsafe {
        *target = CString::new(value)
            .expect("validated strings contain no NUL")
            .into_raw();
    }
}
unsafe extern "C" fn cancel(_: *mut c_void, path: *const c_char, setting: *const c_char) {
    log(format!(
        "CancelGetSecrets path={} setting={}",
        text(path),
        quoted(&text(setting))
    ));
}
unsafe extern "C" fn registered(state: *mut c_void, ok: c_int, error: *const c_char) {
    let state = unsafe { &mut *state.cast::<State>() };
    if error.is_null() {
        state.ok = ok != 0;
        log(format!(
            "registered={}",
            if state.ok { "True" } else { "False" }
        ));
    } else {
        log(format!("register failed: {}", text(error)));
        state.quit = true;
    }
}
unsafe extern "C" fn release(pointer: *mut c_char) {
    if !pointer.is_null() {
        let mut bytes = unsafe { CString::from_raw(pointer) }.into_bytes_with_nul();
        bytes.fill(0);
    }
}
static CALLBACKS: Callbacks = Callbacks {
    get,
    cancel,
    registered,
    release,
};
fn run(program: PathBuf) -> i32 {
    // libnm dispatches these callbacks on this GLib context's owning thread.
    // Keep the boxed address stable and hold no Rust references across C calls;
    // bridge_destroy drains callbacks before we reclaim the state below.
    let state = Box::into_raw(Box::new(State {
        program,
        ok: false,
        quit: false,
    }));
    let mut message = std::ptr::null_mut();
    let bridge = unsafe { bridge_new(state.cast(), &CALLBACKS, &mut message) };
    if bridge.is_null() {
        log(format!("initialization failed: {}", text(message)));
        unsafe {
            bridge_free_error(message);
            drop(Box::from_raw(state));
        }
        return 1;
    }
    unsafe {
        bridge_start(bridge);
    }
    loop {
        // libnm invokes the registration callback from bridge_step below.
        if unsafe { (*state).quit } {
            break;
        }
        let signal = unsafe { bridge_signal() };
        if signal != 0 {
            log(format!("signal {signal}; shutting down"));
            break;
        }
        unsafe {
            bridge_step();
        }
    }
    unsafe {
        bridge_destroy(bridge);
    }
    let state = unsafe { Box::from_raw(state) };
    i32::from(!state.ok)
}
pub fn main_entry() -> i32 {
    run(PathBuf::from(env!("NM_AGENT_NMCLI")))
}

#[cfg(feature = "fixtures")]
pub fn fixture_run(program: PathBuf) -> i32 {
    run(program)
}
#[cfg(feature = "fixtures")]
pub fn fixture_secret(
    program: &Path,
    id: &str,
    uuid: &str,
    setting: &str,
) -> Result<String, String> {
    secrets(program, id, uuid, setting)
}
#[cfg(test)]
mod tests;
