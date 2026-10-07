use std::{
    collections::VecDeque,
    env, fs,
    io::{Read, Write},
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{Arc, Mutex, MutexGuard, OnceLock},
    thread,
    time::{Duration, Instant},
};

unsafe extern "C" {
    #[link_name = "kill"]
    fn libc_kill(pid: i32, sig: i32) -> i32;
}

const SIGINT: i32 = 2;
const SIGKILL: i32 = 9;
const SIGTERM: i32 = 15;
const LINE_TIMEOUT: Duration = Duration::from_secs(15);
const CALL_TIMEOUT: Duration = Duration::from_secs(10);

static BUS: Mutex<()> = Mutex::new(());

fn signal(pid: u32, sig: i32) -> bool {
    unsafe { libc_kill(pid as i32, sig) == 0 }
}

fn nonempty(key: &str) -> Option<String> {
    env::var(key).ok().filter(|value| !value.is_empty())
}

fn upsert(env: &mut Vec<(String, String)>, key: &str, value: &str) {
    if let Some(slot) = env.iter_mut().find(|(name, _)| name == key) {
        slot.1 = value.to_owned();
    } else {
        env.push((key.to_owned(), value.to_owned()));
    }
}

fn remove_key(env: &mut Vec<(String, String)>, key: &str) {
    env.retain(|(name, _)| name != key);
}

fn gquote(value: &str) -> String {
    let mut out = String::from("'");
    for ch in value.chars() {
        match ch {
            '\\' => out.push_str("\\\\"),
            '\'' => out.push_str("\\'"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            other => out.push(other),
        }
    }
    out.push('\'');
    out
}

fn variant_args(setting: &str, flags: u32, id: &str, uuid: &str, path: &str) -> String {
    let mut props = Vec::new();
    if !id.is_empty() {
        props.push(format!("'id': <{}>", gquote(id)));
    }
    if !uuid.is_empty() {
        props.push(format!("'uuid': <{}>", gquote(uuid)));
    }
    props.push("'type': <'802-11-wireless'>".to_owned());
    format!(
        "(@a{{sa{{sv}}}} {{'connection': {{{}}}}}, objectpath {}, {}, @as [], uint32 {})",
        props.join(", "),
        gquote(path),
        gquote(setting),
        flags
    )
}

fn default_args(flags: u32) -> String {
    variant_args(
        "802-11-wireless-security",
        flags,
        "Fixture Wi-Fi",
        "13572468-1234-4321-abcd-123456789012",
        "/org/freedesktop/NetworkManager/Settings/1",
    )
}

fn bus_names(text: &str) -> Vec<String> {
    let bytes = text.as_bytes();
    let mut index = 0;
    let mut names = Vec::new();
    while index < bytes.len() {
        if bytes[index] == b':' {
            let start = index;
            index += 1;
            let digits = index;
            while index < bytes.len() && bytes[index].is_ascii_digit() {
                index += 1;
            }
            if index > digits && index < bytes.len() && bytes[index] == b'.' {
                index += 1;
                let fraction = index;
                while index < bytes.len() && bytes[index].is_ascii_digit() {
                    index += 1;
                }
                if index > fraction {
                    names.push(text[start..index].to_owned());
                    continue;
                }
            }
            index = start + 1;
        } else {
            index += 1;
        }
    }
    names
}

fn fixture_path() -> PathBuf {
    if let Some(path) = nonempty("NM_AGENT_FIXTURE") {
        return PathBuf::from(path);
    }
    if let Some(path) = nonempty("CARGO_BIN_EXE_nm_agent_fixture") {
        return PathBuf::from(path);
    }
    // Some Cargo versions do not export CARGO_BIN_EXE for a feature-gated bin.
    let mut dir = env::current_exe().expect("current test executable");
    while let Some(parent) = dir.parent() {
        let candidate = parent.join("nm-agent-fixture");
        if candidate.is_file() {
            return candidate;
        }
        dir = parent.to_path_buf();
    }
    panic!("fixture binary not found; set NM_AGENT_FIXTURE")
}

fn compile_peer(out: &Path) {
    let src = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/peer.c");
    let pkg = Command::new("pkg-config")
        .args(["--cflags", "--libs", "gio-2.0"])
        .output()
        .expect("pkg-config is required to build the D-Bus peer");
    assert!(
        pkg.status.success(),
        "pkg-config gio-2.0 failed: {}",
        String::from_utf8_lossy(&pkg.stderr)
    );
    let flags = String::from_utf8(pkg.stdout).expect("pkg-config flags are UTF-8");
    let mut cc = Command::new(env::var("CC").unwrap_or_else(|_| "cc".into()));
    cc.args(["-std=c17", "-Wall", "-Wextra", "-Werror"])
        .arg(&src)
        .args(flags.split_whitespace())
        .arg("-o")
        .arg(out);
    let compiled = cc.output().expect("cc");
    assert!(
        compiled.status.success(),
        "peer compile failed\n{}{}",
        String::from_utf8_lossy(&compiled.stdout),
        String::from_utf8_lossy(&compiled.stderr)
    );
}

fn peer_path() -> PathBuf {
    if let Some(path) = nonempty("NM_AGENT_PEER") {
        return PathBuf::from(path);
    }
    static COMPILED: OnceLock<PathBuf> = OnceLock::new();
    COMPILED
        .get_or_init(|| {
            let dir = env::var_os("CARGO_TARGET_TMPDIR")
                .map(PathBuf::from)
                .unwrap_or_else(env::temp_dir);
            let out = dir.join("nm-agent-peer");
            compile_peer(&out);
            out
        })
        .clone()
}

struct Shared {
    lines: VecDeque<String>,
    stderr: String,
    code: Option<Option<i32>>,
}

struct Proc {
    stdin: Option<std::process::ChildStdin>,
    child: std::process::Child,
    shared: Arc<Mutex<Shared>>,
    readers: Vec<thread::JoinHandle<()>>,
    pid: u32,
}

impl Proc {
    fn spawn(file: &str, args: &[String], env: &[(String, String)]) -> Self {
        let mut command = Command::new(file);
        command
            .args(args)
            .env_clear()
            .envs(env.iter().cloned())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = command
            .spawn()
            .unwrap_or_else(|error| panic!("spawn {file} failed: {error}"));
        let pid = child.id();
        let stdout = child.stdout.take().expect("stdout");
        let stderr = child.stderr.take().expect("stderr");
        let stdin = child.stdin.take();
        let shared = Arc::new(Mutex::new(Shared {
            lines: VecDeque::new(),
            stderr: String::new(),
            code: None,
        }));
        let stdout_shared = Arc::clone(&shared);
        let stdout_thread = thread::spawn(move || {
            let mut reader = stdout;
            let mut pending = Vec::new();
            let mut chunk = [0_u8; 4096];
            loop {
                match reader.read(&mut chunk) {
                    Ok(0) => break,
                    Ok(count) => pending.extend_from_slice(&chunk[..count]),
                    Err(_) => break,
                }
                while let Some(split) = pending.iter().position(|byte| *byte == b'\n') {
                    let mut line = pending.drain(..=split).collect::<Vec<_>>();
                    line.pop();
                    let text = String::from_utf8_lossy(&line).into_owned();
                    stdout_shared.lock().unwrap().lines.push_back(text);
                }
            }
            if !pending.is_empty() {
                let text = String::from_utf8_lossy(&pending).into_owned();
                stdout_shared.lock().unwrap().lines.push_back(text);
            }
        });
        let stderr_shared = Arc::clone(&shared);
        let stderr_thread = thread::spawn(move || {
            let mut reader = stderr;
            let mut chunk = [0_u8; 4096];
            loop {
                match reader.read(&mut chunk) {
                    Ok(0) => break,
                    Ok(count) => stderr_shared
                        .lock()
                        .unwrap()
                        .stderr
                        .push_str(&String::from_utf8_lossy(&chunk[..count])),
                    Err(_) => break,
                }
            }
        });
        Self {
            stdin,
            child,
            shared,
            readers: vec![stdout_thread, stderr_thread],
            pid,
        }
    }

    fn pump(&mut self) -> bool {
        if self.shared.lock().unwrap().code.is_some() {
            return true;
        }
        match self.child.try_wait() {
            Ok(Some(status)) => {
                self.shared.lock().unwrap().code = Some(status.code());
                true
            }
            Ok(None) => false,
            Err(error) => panic!("wait failed: {error}"),
        }
    }

    fn finish_readers(&mut self) {
        for handle in self.readers.drain(..) {
            let _ = handle.join();
        }
    }

    fn take_line(&self, prefix: &str) -> Option<String> {
        self.take_matching(|line| line.starts_with(prefix))
    }

    fn take_matching(&self, pred: impl Fn(&str) -> bool) -> Option<String> {
        let mut shared = self.shared.lock().unwrap();
        let index = shared.lines.iter().position(|line| pred(line))?;
        shared.lines.remove(index)
    }

    fn line(&mut self, prefix: &str, timeout: Duration) -> String {
        let start = Instant::now();
        loop {
            if let Some(line) = self.take_line(prefix) {
                return line;
            }
            if self.pump() {
                self.finish_readers();
                if let Some(line) = self.take_line(prefix) {
                    return line;
                }
                let shared = self.shared.lock().unwrap();
                panic!("child exited {:?} {}", shared.code.flatten(), shared.stderr);
            }
            if start.elapsed() > timeout {
                let shared = self.shared.lock().unwrap();
                panic!(
                    "waiting for {prefix}; stderr={}; lines={}",
                    shared.stderr,
                    shared.lines.iter().cloned().collect::<Vec<_>>().join(",")
                );
            }
            thread::sleep(Duration::from_millis(10));
        }
    }

    fn send(&mut self, command: &str) {
        let stdin = self.stdin.as_mut().expect("stdin");
        writeln!(stdin, "{command}").unwrap_or_else(|error| panic!("stdin write: {error}"));
        stdin.flush().unwrap();
    }

    fn stderr(&self) -> String {
        self.shared.lock().unwrap().stderr.clone()
    }

    fn has_prefix(&self, prefix: &str) -> bool {
        self.shared
            .lock()
            .unwrap()
            .lines
            .iter()
            .any(|line| line.starts_with(prefix))
    }

    fn exit_code(&mut self, timeout: Duration) -> Option<i32> {
        let start = Instant::now();
        loop {
            if self.pump() {
                self.finish_readers();
                return self.shared.lock().unwrap().code.flatten();
            }
            if start.elapsed() > timeout {
                panic!("exit timeout {}", self.stderr());
            }
            thread::sleep(Duration::from_millis(10));
        }
    }

    fn kill(&mut self) {
        if self.shared.lock().unwrap().code.is_none() {
            let _ = signal(self.pid, SIGKILL);
            let start = Instant::now();
            while start.elapsed() < Duration::from_secs(7) && !self.pump() {
                thread::sleep(Duration::from_millis(10));
            }
        }
        self.finish_readers();
    }
}

impl Drop for Proc {
    fn drop(&mut self) {
        self.kill();
    }
}

struct Session {
    _bus: MutexGuard<'static, ()>,
    root: PathBuf,
    children: Vec<Proc>,
    env: Vec<(String, String)>,
    manager: usize,
    service: Option<usize>,
    legacy: bool,
}

impl Session {
    fn open(own: bool, legacy: bool) -> Self {
        let guard = BUS.lock().unwrap_or_else(|error| error.into_inner());
        let mut rand = [0_u8; 4];
        let _ = fs::File::open("/dev/urandom").and_then(|mut file| file.read_exact(&mut rand));
        let root = env::temp_dir().join(format!(
            "nm-agent-bus-{}-{:08x}",
            std::process::id(),
            u32::from_ne_bytes(rand)
        ));
        fs::create_dir(&root).expect("temp session");
        let mut session = Self {
            _bus: guard,
            root,
            children: Vec::new(),
            env: Vec::new(),
            manager: 0,
            service: None,
            legacy,
        };
        session.boot(own);
        session
    }

    fn add(&mut self, file: &str, args: &[String], env: &[(String, String)]) -> usize {
        self.children.push(Proc::spawn(file, args, env));
        self.children.len() - 1
    }

    fn boot(&mut self, own: bool) {
        let inherited: Vec<_> = env::vars().collect();
        let bus = self.add(
            "dbus-daemon",
            &[
                "--session".to_owned(),
                "--nofork".to_owned(),
                "--print-address=1".to_owned(),
            ],
            &inherited,
        );
        let address = self.children[bus].line("unix:", LINE_TIMEOUT);
        let mut child_env = inherited;
        remove_key(&mut child_env, "LD_PRELOAD");
        // Both addresses are the disposable daemon. No fixture uses the real system bus.
        for (key, value) in [
            ("DBUS_SESSION_BUS_ADDRESS", address.as_str()),
            ("DBUS_SYSTEM_BUS_ADDRESS", address.as_str()),
            ("LIBNM_USE_SESSION_BUS", "1"),
            ("G_DEBUG", "fatal-criticals"),
            ("NM_TEST_ROOT", self.root.to_str().expect("utf-8 temp path")),
            ("NM_AGENT_NMCLI", "/never-use-runtime-command-overrides"),
        ] {
            upsert(&mut child_env, key, value);
        }
        self.env = child_env;
        let peer = peer_path();
        let peer = peer.to_str().expect("utf-8 peer path").to_owned();
        self.manager = self.add(&peer, &[], &self.env.clone());
        self.children[self.manager].line("READY", LINE_TIMEOUT);
        if own {
            self.children[self.manager].send("own");
            self.children[self.manager].line("OWNED", LINE_TIMEOUT);
        }
        let script = "#!/usr/bin/env bash\nprintf \"%s\\n\" \"$@\" > \"$NM_TEST_ROOT/args\"\nprintf \"%s\\n\" \"$$\" > \"$NM_TEST_ROOT/pid\"\nmode=$(cat \"$NM_TEST_ROOT/mode\")\ncase \"$mode\" in\nfail) cat \"$NM_TEST_ROOT/value\"; cat \"$NM_TEST_ROOT/value\" >&2; exit 9;;\nwait) exec sleep 30;;\n*) cat \"$NM_TEST_ROOT/value\";;\nesac\n";
        let program = self.root.join("nmcli");
        fs::write(&program, script).unwrap();
        fs::set_permissions(&program, fs::Permissions::from_mode(0o700)).unwrap();
        self.value(b"fixture-only-passphrase\n", "ok");
    }

    fn value(&self, data: &[u8], mode: &str) {
        fs::write(self.root.join("value"), data).unwrap();
        fs::write(self.root.join("mode"), mode).unwrap();
    }

    fn start(&mut self, system: bool) {
        let mut env = self.env.clone();
        if system {
            remove_key(&mut env, "LIBNM_USE_SESSION_BUS");
        }
        let nmcli = self.root.join("nmcli");
        let nmcli = nmcli.to_str().expect("utf-8 nmcli path");
        let index = if self.legacy {
            assert!(
                nonempty("NM_AGENT_LEGACY").is_some()
                    && nonempty("NM_AGENT_PYTHON").is_some()
                    && nonempty("NM_AGENT_ROUTE").is_some()
            );
            upsert(&mut env, "LD_PRELOAD", &nonempty("NM_AGENT_ROUTE").unwrap());
            upsert(&mut env, "NM_FIXTURE_EXEC", nmcli);
            let python = nonempty("NM_AGENT_PYTHON").unwrap();
            let legacy = nonempty("NM_AGENT_LEGACY").unwrap();
            self.add(&python, &[legacy], &env)
        } else if let Some(production) = nonempty("NM_AGENT_BIN") {
            assert!(
                nonempty("NM_AGENT_ROUTE").is_some(),
                "packaged command needs routing interposer"
            );
            upsert(&mut env, "LD_PRELOAD", &nonempty("NM_AGENT_ROUTE").unwrap());
            upsert(&mut env, "NM_FIXTURE_EXEC", nmcli);
            self.add(&production, &[], &env)
        } else {
            let fixture = fixture_path();
            let fixture = fixture.to_str().expect("utf-8 fixture").to_owned();
            self.add(&fixture, &["run".to_owned(), nmcli.to_owned()], &env)
        };
        self.service = Some(index);
    }

    fn service_index(&self) -> usize {
        self.service.expect("service")
    }

    fn ready(&mut self) {
        let line = self.children[self.manager].line("REGISTER\t", LINE_TIMEOUT);
        let fields: Vec<_> = line.split('\t').collect();
        let _name = fields.get(1).expect("register sender");
        assert_eq!(
            fields.get(2).copied(),
            Some("org.dotfiles.nm-auto-secret-agent")
        );
        assert_eq!(fields.get(3).copied(), Some("0"));
        let service = self.service_index();
        let start = Instant::now();
        while start.elapsed() < Duration::from_millis(5000)
            && !self.children[service].stderr().contains("registered=True")
        {
            thread::sleep(Duration::from_millis(10));
        }
        let stderr = self.children[service].stderr();
        assert!(stderr.contains("registered=True"), "{stderr}");
    }

    fn call(&mut self, method: &str, args: &str) -> String {
        let manager = self.manager;
        self.children[manager].send(&format!("call\t{method}\t{args}"));
        let start = Instant::now();
        loop {
            if let Some(line) = self.children[self.manager]
                .take_matching(|line| line.starts_with("REPLY\t") || line.starts_with("ERROR\t"))
            {
                return line;
            }
            if let Some(service) = self.service
                && self.children[service].pump()
            {
                self.children[service].finish_readers();
                panic!(
                    "service exited {:?} {}",
                    self.children[service].shared.lock().unwrap().code.flatten(),
                    self.children[service].stderr()
                );
            }
            if start.elapsed() > CALL_TIMEOUT {
                let stderr = self
                    .service
                    .map(|index| self.children[index].stderr())
                    .unwrap_or_default();
                panic!("call timed out {stderr}");
            }
            thread::sleep(Duration::from_millis(10));
        }
    }

    fn stop(&mut self, sig: i32, code: i32) {
        let service = self.service_index();
        if self.children[service].shared.lock().unwrap().code.is_none() {
            let _ = signal(self.children[service].pid, sig);
        }
        let got = self.children[service].exit_code(LINE_TIMEOUT);
        let stderr = self.children[service].stderr();
        assert_eq!(got, Some(code), "{stderr}");
        assert!(
            stderr.contains("signal 15; shutting down")
                || stderr.contains("signal 2; shutting down"),
            "{stderr}"
        );
        assert!(!stderr.contains("fixture-only-passphrase"), "{stderr}");
        assert!(!stderr.contains("GLib-CRITICAL"), "{stderr}");
        assert!(!stderr.contains("panic"), "{stderr}");
    }

    fn args_text(&self) -> String {
        fs::read_to_string(self.root.join("args")).unwrap()
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        for child in self.children.iter_mut().rev() {
            child.kill();
        }
        let _ = fs::remove_dir_all(&self.root);
    }
}

fn session(own: bool, body: impl FnOnce(&mut Session)) {
    let mut current = Session::open(own, false);
    body(&mut current);
}

fn assert_no_args(session: &Session) {
    assert!(!session.root.join("args").exists());
}

#[test]
fn registers_and_returns_wifi_serialization_for_normal_and_request_new() {
    session(true, |current| {
        current.start(false);
        current.ready();
        for flags in [0, 1, 2, 3, 4, 0x8000_0000] {
            let reply = current.call("GetSecrets", &default_args(flags));
            assert!(reply.starts_with("REPLY\t"), "{reply}");
            assert!(reply.contains("'802-11-wireless-security'"), "{reply}");
            assert!(
                reply.contains("'psk': <'fixture-only-passphrase'>"),
                "{reply}"
            );
            let args_text = current.args_text();
            let args: Vec<_> = args_text.trim_end().split('\n').collect();
            assert_eq!(
                args,
                [
                    "-s",
                    "-g",
                    "802-11-wireless-security.psk",
                    "connection",
                    "show",
                    "13572468-1234-4321-abcd-123456789012",
                ]
            );
        }
        current.stop(SIGTERM, 0);
        current.children[current.manager].line("UNREGISTER", LINE_TIMEOUT);
    });
}

#[test]
fn id_fallback_keeps_unicode_and_metacharacters_literal() {
    session(true, |current| {
        current.start(false);
        current.ready();
        let id = "Fixture 尾;$(touch NEVER) 'quote'";
        let reply = current.call(
            "GetSecrets",
            &variant_args(
                "802-11-wireless-security",
                0,
                id,
                "",
                "/org/freedesktop/NetworkManager/Settings/1",
            ),
        );
        assert!(reply.starts_with("REPLY"), "{reply}");
        let args_text = current.args_text();
        assert_eq!(args_text.split('\n').nth(5), Some(id));
        assert!(!current.root.join("NEVER").exists());
        current.stop(SIGINT, 0);
    });
}

#[test]
fn missing_keys_and_unsupported_settings_return_no_secrets_without_nmcli() {
    session(true, |current| {
        current.start(false);
        current.ready();
        let result = current.call(
            "GetSecrets",
            &variant_args(
                "vpn",
                0,
                "",
                "",
                "/org/freedesktop/NetworkManager/Settings/1",
            ),
        );
        assert!(result.contains("NoSecrets"), "{result}");
        assert!(result.contains("connection has no uuid/id"), "{result}");
        let result = current.call(
            "GetSecrets",
            &variant_args(
                "vpn",
                0,
                "Fixture Wi-Fi",
                "13572468-1234-4321-abcd-123456789012",
                "/org/freedesktop/NetworkManager/Settings/1",
            ),
        );
        assert!(result.contains("NoSecrets"), "{result}");
        assert!(
            result.contains("unsupported setting for auto agent: vpn"),
            "{result}"
        );
        assert_no_args(current);
        current.stop(SIGTERM, 0);
    });
}

#[test]
fn empty_invalid_nul_and_failed_output_do_not_leak_credentials() {
    session(true, |current| {
        current.start(false);
        current.ready();
        let cases: [(&[u8], &str, &str); 4] = [
            (b"\r\n\r", "ok", "empty psk"),
            (&[83, 69, 67, 82, 69, 84, 255], "ok", "invalid UTF-8"),
            (b"SECRET\0tail", "ok", "invalid string"),
            (b"SECRET", "fail", "status 9"),
        ];
        for (data, mode, message) in cases {
            current.value(data, mode);
            let result = current.call("GetSecrets", &default_args(0));
            assert!(result.contains("NoSecrets"), "{result}");
            assert!(result.contains(message), "{result}");
            assert!(!result.contains("SECRET"), "{result}");
            assert!(
                !current.children[current.service_index()]
                    .stderr()
                    .contains("SECRET")
            );
        }
        current.stop(SIGTERM, 0);
    });
}

#[test]
fn universal_newlines_strip_only_trailing_lf() {
    session(true, |current| {
        current.start(false);
        current.ready();
        current.value(b" a\r\nb\rc \r\n\n", "ok");
        let result = current.call("GetSecrets", &default_args(0));
        assert!(result.contains("'psk': <' a\\nb\\nc '>"), "{result}");
        current.stop(SIGTERM, 0);
    });
}

#[test]
fn save_and_delete_are_noops_and_absent_cancel_is_a_libnm_error() {
    session(true, |current| {
        current.start(false);
        current.ready();
        let args = "(@a{sa{sv}} {'connection': {'id': <'Fixture'>, 'type': <'802-11-wireless'>}}, objectpath '/org/freedesktop/NetworkManager/Settings/1')";
        for method in ["SaveSecrets", "DeleteSecrets"] {
            let reply = current.call(method, args);
            assert!(reply.starts_with("REPLY\t()"), "{reply}");
        }
        let cancel = current.call(
            "CancelGetSecrets",
            "(objectpath '/org/freedesktop/NetworkManager/Settings/1', '802-11-wireless-security')",
        );
        assert!(
            cancel.contains("No secrets request in progress"),
            "{cancel}"
        );
        assert_no_args(current);
        current.stop(SIGTERM, 0);
    });
}

#[test]
fn invalid_connection_paths_fail_before_application_callbacks() {
    session(true, |current| {
        current.start(false);
        current.ready();
        let result = current.call(
            "GetSecrets",
            &variant_args("802-11-wireless-security", 0, "Fixture", "", "/"),
        );
        assert!(result.contains("InvalidConnection"), "{result}");
        assert_no_args(current);
        current.stop(SIGTERM, 0);
    });
}

#[test]
fn manager_disappearance_reregisters_without_losing_successful_exit() {
    session(true, |current| {
        current.start(false);
        current.ready();
        current.children[current.manager].send("release");
        current.children[current.manager].line("RELEASED", LINE_TIMEOUT);
        current.children[current.manager].line("UNREGISTER", LINE_TIMEOUT);
        thread::sleep(Duration::from_millis(100));
        current.children[current.manager].send("own");
        current.children[current.manager].line("OWNED", LINE_TIMEOUT);
        current.ready();
        let reply = current.call("GetSecrets", &default_args(0));
        assert!(reply.starts_with("REPLY"), "{reply}");
        current.stop(SIGTERM, 0);
    });
}

#[test]
fn registration_refusal_exits_unsuccessfully_without_lookup() {
    session(true, |current| {
        current.children[current.manager].send("deny");
        current.children[current.manager].line("DENY", LINE_TIMEOUT);
        current.start(false);
        current.children[current.manager].line("REGISTER\t", LINE_TIMEOUT);
        let service = current.service_index();
        let code = current.children[service].exit_code(LINE_TIMEOUT);
        let stderr = current.children[service].stderr();
        assert_eq!(code, Some(1), "{stderr}");
        assert!(
            stderr.contains("register failed: registration failed"),
            "{stderr}"
        );
        assert_no_args(current);
    });
}

#[test]
fn unavailable_disposable_bus_fails_initialization_without_lookup() {
    session(true, |current| {
        current.children[0].kill();
        current.start(false);
        let service = current.service_index();
        let code = current.children[service].exit_code(LINE_TIMEOUT);
        let stderr = current.children[service].stderr();
        assert_eq!(code, Some(1), "{stderr}");
        assert!(stderr.contains("initialization failed:"), "{stderr}");
        assert_no_args(current);
    });
}

#[test]
fn missing_manager_fails_initial_registration() {
    session(false, |current| {
        current.start(false);
        let service = current.service_index();
        let code = current.children[service].exit_code(LINE_TIMEOUT);
        let stderr = current.children[service].stderr();
        assert_eq!(code, Some(1), "{stderr}");
        assert!(stderr.contains("register failed:"), "{stderr}");
        assert_no_args(current);
    });
}

#[test]
fn system_bus_manager_owned_by_nonroot_is_not_trusted() {
    session(true, |current| {
        current.children[current.manager].send("names");
        let before = current.children[current.manager].line("REPLY\t", LINE_TIMEOUT);
        current.start(true);
        let mut names = String::new();
        for _ in 0..100 {
            current.children[current.manager].send("names");
            names = current.children[current.manager].line("REPLY\t", LINE_TIMEOUT);
            if names != before {
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }
        let old: std::collections::HashSet<_> = bus_names(&before).into_iter().collect();
        let added: Vec<_> = bus_names(&names)
            .into_iter()
            .filter(|name| !old.contains(name))
            .collect();
        assert_eq!(added.len(), 1, "before={before} names={names}");
        current.children[current.manager].send(&format!("target\t{}", added[0]));
        current.children[current.manager].line("TARGET", LINE_TIMEOUT);
        thread::sleep(Duration::from_millis(100));
        assert!(!current.children[current.manager].has_prefix("REGISTER"));
        let result = current.call("GetSecrets", &default_args(0));
        assert!(result.contains("PermissionDenied"), "{result}");
        assert!(
            result.contains("non authenticated peer rejected"),
            "{result}"
        );
        assert_no_args(current);
        current.stop(SIGTERM, 1);
    });
}

#[test]
fn sigterm_during_blocked_lookup_reaps_child_without_publishing_secret() {
    session(true, |current| {
        current.start(false);
        current.ready();
        current.value(b"fixture-only-passphrase", "wait");
        current.children[current.manager].send(&format!("call\tGetSecrets\t{}", default_args(0)));
        let pid_path = current.root.join("pid");
        for _ in 0..400 {
            if pid_path.exists() {
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }
        let pid: i32 = fs::read_to_string(&pid_path)
            .unwrap_or_else(|error| panic!("pid file: {error}"))
            .trim()
            .parse()
            .expect("pid");
        current.stop(SIGTERM, 0);
        assert!(!signal(pid as u32, 0), "lookup child {pid} was not reaped");
        let reply = current.children[current.manager].line("ERROR\t", LINE_TIMEOUT);
        assert!(reply.contains("AgentCanceled"), "{reply}");
        assert!(
            current.children[current.service_index()]
                .stderr()
                .contains("CancelGetSecrets path="),
            "{}",
            current.children[current.service_index()].stderr()
        );
    });
}

#[test]
fn legacy_reference_matches_payloads_and_policy_errors() {
    if nonempty("NM_AGENT_LEGACY").is_none() {
        eprintln!("skipped: NM_AGENT_LEGACY unset");
        return;
    }
    let mut results = Vec::new();
    for old in [false, true] {
        let mut current = Session::open(true, old);
        current.start(false);
        current.ready();
        let mut replies = Vec::new();
        for value in ["fixture-only-passphrase\n", " x\r\ny \n", "尾🙂\n"] {
            current.value(value.as_bytes(), "ok");
            let reply = current.call("GetSecrets", &default_args(2));
            let stderr = current.children[current.service_index()].stderr();
            assert!(!reply.contains("Timeout was reached"), "{stderr}");
            replies.push(reply);
        }
        replies.push(current.call(
            "GetSecrets",
            &variant_args(
                "vpn",
                0,
                "Fixture Wi-Fi",
                "13572468-1234-4321-abcd-123456789012",
                "/org/freedesktop/NetworkManager/Settings/1",
            ),
        ));
        replies.push(current.call(
            "GetSecrets",
            &variant_args(
                "vpn",
                0,
                "",
                "",
                "/org/freedesktop/NetworkManager/Settings/1",
            ),
        ));
        current.value(b"\n", "ok");
        replies.push(current.call("GetSecrets", &default_args(0)));
        results.push(replies);
        current.stop(SIGTERM, 0);
    }
    assert_eq!(results[0], results[1]);
}

#[test]
fn repeated_requests_do_not_grow_the_open_descriptor_set() {
    session(true, |current| {
        current.start(false);
        current.ready();
        let pid = current.children[current.service_index()].pid;
        let count = || fs::read_dir(format!("/proc/{pid}/fd")).unwrap().count();
        let before = count();
        for _ in 0..40 {
            let reply = current.call("GetSecrets", &default_args(0));
            assert!(reply.starts_with("REPLY"), "{reply}");
        }
        assert!(count() <= before + 1, "fds grew from {before}");
        current.stop(SIGTERM, 0);
    });
}

#[derive(Debug)]
enum Json {
    Null,
    Bool(bool),
    Number(f64),
    String(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl PartialEq for Json {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Null, Self::Null) => true,
            (Self::Bool(left), Self::Bool(right)) => left == right,
            (Self::Number(left), Self::Number(right)) => left == right,
            (Self::String(left), Self::String(right)) => left == right,
            (Self::Array(left), Self::Array(right)) => left == right,
            (Self::Object(left), Self::Object(right)) => {
                left.len() == right.len()
                    && left.iter().all(|(key, value)| {
                        right.iter().any(|(other_key, other_value)| {
                            key == other_key && value == other_value
                        })
                    })
            }
            _ => false,
        }
    }
}

impl Json {
    fn get(&self, key: &str) -> &Self {
        match self {
            Self::Object(entries) => entries
                .iter()
                .find(|(name, _)| name == key)
                .map(|(_, value)| value)
                .unwrap_or_else(|| panic!("missing {key} in {self:?}")),
            other => panic!("expected object, got {other:?}"),
        }
    }
}

fn json_strings(value: &Json) -> Vec<String> {
    match value {
        Json::Array(items) => items
            .iter()
            .map(|item| match item {
                Json::String(text) => text.clone(),
                other => panic!("expected string, got {other:?}"),
            })
            .collect(),
        other => panic!("expected array, got {other:?}"),
    }
}

struct JsonParser<'a> {
    bytes: &'a [u8],
    index: usize,
}

impl<'a> JsonParser<'a> {
    fn parse(text: &'a str) -> Json {
        let mut parser = Self {
            bytes: text.as_bytes(),
            index: 0,
        };
        let value = parser.value();
        parser.skip();
        assert!(parser.index == parser.bytes.len(), "trailing JSON");
        value
    }

    fn skip(&mut self) {
        while self.index < self.bytes.len() && self.bytes[self.index].is_ascii_whitespace() {
            self.index += 1;
        }
    }

    fn peek(&mut self) -> u8 {
        self.skip();
        self.bytes[self.index]
    }

    fn value(&mut self) -> Json {
        match self.peek() {
            b'n' => self.literal(b"null", Json::Null),
            b't' => self.literal(b"true", Json::Bool(true)),
            b'f' => self.literal(b"false", Json::Bool(false)),
            b'"' => Json::String(self.string()),
            b'[' => self.array(),
            b'{' => self.object(),
            b'-' | b'0'..=b'9' => self.number(),
            other => panic!("invalid JSON byte {other}"),
        }
    }

    fn literal(&mut self, expected: &[u8], value: Json) -> Json {
        self.skip();
        assert!(
            self.bytes[self.index..].starts_with(expected),
            "invalid JSON literal"
        );
        self.index += expected.len();
        value
    }

    fn string(&mut self) -> String {
        self.skip();
        assert_eq!(self.bytes[self.index], b'"');
        self.index += 1;
        let mut out = String::new();
        while self.index < self.bytes.len() {
            match self.bytes[self.index] {
                b'"' => {
                    self.index += 1;
                    return out;
                }
                b'\\' => {
                    self.index += 1;
                    let escape = self.bytes[self.index];
                    self.index += 1;
                    match escape {
                        b'"' | b'\\' | b'/' => out.push(escape as char),
                        b'b' => out.push('\u{0008}'),
                        b'f' => out.push('\u{000c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let hex = &self.bytes[self.index..self.index + 4];
                            self.index += 4;
                            let code =
                                u32::from_str_radix(std::str::from_utf8(hex).unwrap(), 16).unwrap();
                            out.push(char::from_u32(code).expect("unicode escape"));
                        }
                        other => panic!("invalid JSON escape {other}"),
                    }
                }
                byte => {
                    let width = if byte < 0x80 {
                        1
                    } else if byte & 0xe0 == 0xc0 {
                        2
                    } else if byte & 0xf0 == 0xe0 {
                        3
                    } else {
                        4
                    };
                    let text =
                        std::str::from_utf8(&self.bytes[self.index..self.index + width]).unwrap();
                    out.push_str(text);
                    self.index += width;
                }
            }
        }
        panic!("unterminated JSON string");
    }

    fn number(&mut self) -> Json {
        self.skip();
        let start = self.index;
        if self.bytes[self.index] == b'-' {
            self.index += 1;
        }
        while self.index < self.bytes.len()
            && matches!(
                self.bytes[self.index],
                b'0'..=b'9' | b'.' | b'e' | b'E' | b'+' | b'-'
            )
        {
            self.index += 1;
        }
        let text = std::str::from_utf8(&self.bytes[start..self.index]).unwrap();
        Json::Number(
            text.parse()
                .unwrap_or_else(|_| panic!("invalid JSON number {text}")),
        )
    }

    fn array(&mut self) -> Json {
        self.skip();
        self.index += 1;
        let mut items = Vec::new();
        if self.peek() == b']' {
            self.index += 1;
            return Json::Array(items);
        }
        loop {
            items.push(self.value());
            match self.peek() {
                b']' => {
                    self.index += 1;
                    return Json::Array(items);
                }
                b',' => self.index += 1,
                other => panic!("invalid JSON array byte {other}"),
            }
        }
    }

    fn object(&mut self) -> Json {
        self.skip();
        self.index += 1;
        let mut items = Vec::new();
        if self.peek() == b'}' {
            self.index += 1;
            return Json::Object(items);
        }
        loop {
            let key = self.string();
            assert_eq!(self.peek(), b':');
            self.index += 1;
            let value = self.value();
            if let Some(slot) = items.iter_mut().find(|(name, _)| *name == key) {
                slot.1 = value;
            } else {
                items.push((key, value));
            }
            match self.peek() {
                b'}' => {
                    self.index += 1;
                    return Json::Object(items);
                }
                b',' => self.index += 1,
                other => panic!("invalid JSON object byte {other}"),
            }
        }
    }
}

#[test]
fn host_enablement_retains_private_graphical_boundary() {
    let Some(path) = nonempty("NM_AGENT_WIRING") else {
        eprintln!("skipped: NM_AGENT_WIRING unset");
        return;
    };
    let wiring = JsonParser::parse(&fs::read_to_string(path).unwrap());
    for name in [
        "william@foundation",
        "william@andromeda",
        "william@starfish",
    ] {
        let config = wiring.get(name);
        assert_eq!(config.get("private"), &Json::Bool(true));
        let agent = config.get("agent");
        assert_eq!(
            json_strings(agent.get("Unit").get("After")),
            vec!["graphical-session.target".to_owned()]
        );
        assert_eq!(
            json_strings(agent.get("Unit").get("PartOf")),
            vec!["graphical-session.target".to_owned()]
        );
        assert_eq!(
            json_strings(agent.get("Install").get("WantedBy")),
            vec!["graphical-session.target".to_owned()]
        );
        assert_eq!(
            agent.get("Service").get("Type"),
            &Json::String("simple".into())
        );
        assert_eq!(
            agent.get("Service").get("Restart"),
            &Json::String("on-failure".into())
        );
        assert_eq!(agent.get("Service").get("RestartSec"), &Json::Number(2.0));
        let exec = json_strings(agent.get("Service").get("ExecStart"));
        assert!(
            exec[0].ends_with("nm-auto-secret-agent-0.1.0/bin/nm-auto-secret-agent"),
            "{}",
            exec[0]
        );
        assert!(!exec[0].contains("python"), "{}", exec[0]);
        let watcher = config.get("watcher");
        assert_eq!(
            json_strings(watcher.get("Unit").get("After")),
            vec![
                "graphical-session.target".to_owned(),
                "nm-auto-secret-agent.service".to_owned(),
            ]
        );
        assert_eq!(watcher.get("Service").get("RestartSec"), &Json::Number(3.0));
    }
    for name in ["william@terminus", "william-darwin"] {
        assert_eq!(wiring.get(name).get("agent"), &Json::Null);
        assert_eq!(wiring.get(name).get("watcher"), &Json::Null);
    }
    if let Some(path) = nonempty("NM_AGENT_PUBLIC_WIRING") {
        let parsed = JsonParser::parse(&fs::read_to_string(path).unwrap());
        assert_eq!(
            parsed,
            Json::Object(vec![
                ("agent".into(), Json::Null),
                ("watcher".into(), Json::Null),
            ])
        );
    }
}
