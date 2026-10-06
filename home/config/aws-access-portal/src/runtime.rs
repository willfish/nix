use crate::{Error, Result};
use regex::Regex;
use std::{
    env, fs,
    io::{Read, Write},
    os::unix::fs::{DirBuilderExt, PermissionsExt},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::OnceLock,
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

pub trait Runtime {
    fn now(&self) -> i64;
    fn monotonic(&self) -> u128;
    fn sleep(&mut self, duration: Duration);
    fn secret(&mut self, name: &str) -> Result<String>;
    fn totp(&mut self) -> Result<String>;
    fn approve(&mut self, name: &str, account: &str, role: &str) -> Result<bool>;
    fn state(&self) -> PathBuf;
}
pub struct Local {
    pub secrets: Vec<String>,
    started: Instant,
}
impl Default for Local {
    fn default() -> Self {
        Self {
            secrets: vec![],
            started: Instant::now(),
        }
    }
}
#[allow(deprecated)] // Unix-only package: retain HOME, then passwd lookup.
pub fn home() -> PathBuf {
    env::home_dir().expect("home directory is unavailable")
}
pub fn redact(text: &str) -> String {
    static RE: OnceLock<Regex> = OnceLock::new();
    RE.get_or_init(|| {
        Regex::new(r"(ASIA[A-Z0-9]{16}|AKIA[A-Z0-9]{16}|[A-Za-z0-9+/]{40,}={0,2})").unwrap()
    })
    .replace_all(text, "[redacted]")
    .into_owned()
}
impl Local {
    pub fn safe_error(&self, text: &str) -> String {
        let mut text = text.to_owned();
        for secret in &self.secrets {
            if !secret.is_empty() {
                text = text.replace(secret, "[redacted]");
            }
        }
        redact(&text)
    }
}
pub fn command(
    program: &str,
    args: &[&str],
    input: Option<&str>,
    timeout: Duration,
) -> Result<(bool, String)> {
    let mut child = Command::new(program)
        .args(args)
        .stdin(if input.is_some() {
            Stdio::piped()
        } else {
            Stdio::inherit()
        })
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|_| Error::new("local helper could not start"))?;
    if let Some(text) = input {
        if child
            .stdin
            .take()
            .is_none_or(|mut stdin| stdin.write_all(text.as_bytes()).is_err())
        {
            let _ = child.kill();
            let _ = child.wait();
            return Err(Error::new("local helper input failed"));
        }
    }
    let mut stdout = child
        .stdout
        .take()
        .ok_or_else(|| Error::new("local helper output unavailable"))?;
    let reader = thread::spawn(move || {
        let mut out = Vec::new();
        stdout.read_to_end(&mut out).map(|_| out)
    });
    let deadline = Instant::now() + timeout;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if Instant::now() < deadline => thread::sleep(Duration::from_millis(20)),
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(Error::new("local helper timed out or failed"));
            }
        }
    };
    let bytes = reader
        .join()
        .map_err(|_| Error::new("local helper output failed"))?
        .map_err(|_| Error::new("local helper output failed"))?;
    let text =
        String::from_utf8(bytes).map_err(|_| Error::new("local helper returned invalid text"))?;
    Ok((
        status.success(),
        text.replace("\r\n", "\n").replace('\r', "\n"),
    ))
}
pub fn private_directory(path: &Path) -> Result<()> {
    fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)
        .map_err(|_| Error::new("could not create private state directory"))?;
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
        .map_err(|_| Error::new("could not protect state directory"))
}
pub fn write_private(path: &Path, text: &str) -> Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| Error::new("invalid credential path"))?;
    private_directory(parent)?;
    let mut file = tempfile::Builder::new()
        .prefix(".tmp-")
        .tempfile_in(parent)
        .map_err(|_| Error::new("could not create private credential file"))?;
    file.as_file()
        .set_permissions(fs::Permissions::from_mode(0o600))
        .and_then(|_| file.write_all(text.as_bytes()))
        .and_then(|_| file.flush())
        .and_then(|_| file.as_file().sync_all())
        .map_err(|_| Error::new("could not write credential file"))?;
    file.persist(path)
        .map_err(|_| Error::new("could not publish credential file"))?;
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        .map_err(|_| Error::new("could not protect credential file"))
}
impl Runtime for Local {
    fn now(&self) -> i64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis() as i64
    }
    fn monotonic(&self) -> u128 {
        self.started.elapsed().as_millis()
    }
    fn sleep(&mut self, duration: Duration) {
        thread::sleep(duration)
    }
    fn secret(&mut self, name: &str) -> Result<String> {
        let base = env::var_os("SOPS_NIX_SECRETS_DIR")
            .filter(|s| !s.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                env::var_os("XDG_CONFIG_HOME")
                    .map(PathBuf::from)
                    .unwrap_or_else(|| home().join(".config"))
                    .join("sops-nix/secrets")
            });
        let raw = fs::read_to_string(base.join(name))
            .map_err(|_| Error::new(format!("{name} is missing")))?;
        let trimmed = raw.trim();
        let value = trimmed
            .strip_prefix('"')
            .and_then(|s| s.strip_suffix('"'))
            .unwrap_or(trimmed)
            .to_owned();
        if value.is_empty() {
            return Err(Error::new(format!("{name} is empty")));
        }
        self.secrets.push(value.clone());
        Ok(value)
    }
    fn totp(&mut self) -> Result<String> {
        let second = (self.now() / 1000) % 30;
        if second >= 25 {
            self.sleep(Duration::from_secs((31 - second) as u64));
        }
        let (ok, text) = command("totp-from-sops", &["AWS"], None, Duration::from_secs(15))
            .map_err(|_| Error::new("could not generate an AWS TOTP code"))?;
        if !ok {
            return Err(Error::new("could not generate an AWS TOTP code"));
        }
        let code = text.trim();
        if code.len() != 6 || !code.bytes().all(|b| b.is_ascii_digit()) {
            return Err(Error::new("TOTP helper did not return a 6 digit code"));
        }
        self.secrets.push(code.to_owned());
        Ok(code.to_owned())
    }
    fn approve(&mut self, name: &str, account: &str, role: &str) -> Result<bool> {
        // No environment hook or MCP argument can stand in for a local decision.
        if env::var_os("WAYLAND_DISPLAY").is_none_or(|s| s.is_empty())
            && env::var_os("DISPLAY").is_none_or(|s| s.is_empty())
        {
            return Err(Error::new(
                "production administrator credentials need a local approval prompt, and no display is available",
            ));
        }
        let message = format!(
            "Export production administrator credentials?\nAccount: {name} ({account})\nRole: {role}\nThis grants production admin keys for a few hours.\nSelect Approve only if you intend this. Agents must not answer."
        );
        let (ok, out) = command(
            "fuzzel",
            &[
                "--dmenu",
                "--prompt=Approve production admin? ",
                "--mesg",
                &message,
                "--lines=2",
            ],
            Some("Approve\nDeny\n"),
            Duration::from_secs(90),
        )
        .map_err(|_| {
            Error::new("production administrator approval prompt was unavailable or failed")
        })?;
        Ok(ok && out.trim() == "Approve")
    }
    fn state(&self) -> PathBuf {
        env::var_os("XDG_RUNTIME_DIR")
            .filter(|s| !s.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| home().join(".local/state"))
            .join("aws-access-portal")
    }
}
