use super::*;
use std::{
    fs,
    os::unix::fs::PermissionsExt,
    sync::atomic::{AtomicUsize, Ordering},
};
static NEXT: AtomicUsize = AtomicUsize::new(0);
// Serialise fixture creation with spawning: forked test children can briefly
// inherit another thread's writable script descriptor before exec closes it.
static PROCESS: std::sync::Mutex<()> = std::sync::Mutex::new(());
struct Fixture {
    root: PathBuf,
    _guard: std::sync::MutexGuard<'static, ()>,
}
impl Fixture {
    fn new(body: &str) -> Self {
        let guard = PROCESS.lock().unwrap_or_else(|e| e.into_inner());
        let p = std::env::temp_dir().join(format!(
            "nm-agent-unit-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&p).unwrap();
        let file = p.join("nmcli");
        fs::write(&file, format!("#!/usr/bin/env bash\n{body}\n")).unwrap();
        fs::set_permissions(&file, fs::Permissions::from_mode(0o700)).unwrap();
        Self {
            root: p,
            _guard: guard,
        }
    }
    fn program(&self) -> PathBuf {
        self.root.join("nmcli")
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.root).unwrap();
    }
}
#[test]
fn uuid_precedes_id_and_fixed_arguments_are_literal() {
    let f = Fixture::new(
        "[[ $# == 6 && $1 == -s && $2 == -g && $3 == 802-11-wireless-security.psk && $4 == connection && $5 == show && $6 == 'uuid;not-shell' ]] || exit 11\nprintf 'disposable-psk\\n'",
    );
    assert_eq!(
        secrets(
            &f.program(),
            "id",
            "uuid;not-shell",
            "802-11-wireless-security"
        )
        .unwrap(),
        "disposable-psk"
    );
}
#[test]
fn id_fallback_preserves_whitespace() {
    let f = Fixture::new("[[ $6 == ' name with spaces ' ]] || exit 12\nprintf ' value \\n\\n'");
    assert_eq!(
        secrets(
            &f.program(),
            " name with spaces ",
            "",
            "802-11-wireless-security"
        )
        .unwrap(),
        " value "
    );
}
#[test]
fn missing_key_and_unsupported_setting_never_spawn() {
    let absent = Path::new("/nonexistent/nmcli");
    assert_eq!(
        secrets(absent, "", "", "vpn").unwrap_err(),
        "connection has no uuid/id"
    );
    assert_eq!(
        secrets(absent, "id", "", "vpn").unwrap_err(),
        "unsupported setting for auto agent: vpn"
    );
}
#[test]
fn universal_newlines_but_not_general_whitespace_are_stripped() {
    let f = Fixture::new("printf 'a\\r\\nb\\rc\\n\\r\\n'");
    assert_eq!(lookup(&f.program(), "id").unwrap(), "a\nb\nc");
    drop(f);
    let f = Fixture::new("printf ' \\t\\n'");
    assert_eq!(lookup(&f.program(), "id").unwrap(), " \t");
}
#[test]
fn empty_invalid_utf8_nul_nonzero_and_signaled_output_stay_private() {
    for (body, expected) in [
        ("printf '\\r\\n\\r'", "system connection has empty psk"),
        (
            "printf 'DO_NOT_PRINT\\377'",
            "system secret lookup returned invalid UTF-8",
        ),
        (
            "printf 'DO_NOT_PRINT\\0tail'",
            "system secret lookup returned an invalid string",
        ),
        (
            "printf DO_NOT_PRINT; echo DO_NOT_PRINT >&2; exit 7",
            "system secret lookup exited with status 7",
        ),
        (
            "kill -TERM $$",
            "system secret lookup terminated by signal 15",
        ),
    ] {
        let f = Fixture::new(body);
        assert_eq!(lookup(&f.program(), "id").unwrap_err(), expected);
    }
}
#[test]
fn missing_executable_does_not_echo_path() {
    let _guard = PROCESS.lock().unwrap_or_else(|e| e.into_inner());
    assert_eq!(
        lookup(Path::new("/does-not-exist/SENSITIVE"), "id").unwrap_err(),
        "could not start system secret lookup (NotFound)"
    );
}
#[test]
fn stdout_larger_than_pipe_buffer_is_drained() {
    let f = Fixture::new("printf '%200000s' x");
    let value = lookup(&f.program(), "id").unwrap();
    assert_eq!(value.len(), 200000);
    assert!(value.ends_with('x'));
}
#[test]
fn diagnostic_repr_matches_python_style() {
    assert_eq!(quoted("a'b"), "\"a'b\"");
    assert_eq!(quoted("a'b\""), "'a\\'b\"'");
    assert_eq!(quoted("\n\t\r\0\u{7f}"), "'\\n\\t\\r\\x00\\x7f'");
    assert_eq!(quoted("尾🙂"), "'尾🙂'");
    assert_eq!(quoted("\u{a0}\u{202e}\u{e000}"), "'\\xa0\\u202e\\ue000'");
}
