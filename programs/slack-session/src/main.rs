#[path = "../../aws-access-portal/src/cdp.rs"]
mod cdp;
#[cfg(test)]
mod tests;
use cdp::CdpSocket;
use regex::Regex;
use reqwest::blocking::Client;
use serde_json::{Value, json};
use std::{
    env, fs,
    io::Write,
    os::unix::fs::{DirBuilderExt, PermissionsExt},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    thread,
    time::Duration,
};
#[derive(Debug)]
pub struct Error(String);
impl Error {
    pub fn new(s: impl Into<String>) -> Self {
        Self(s.into())
    }
}
type Result<T> = std::result::Result<T, Error>;
fn text<'a>(v: &'a Value, key: &str) -> &'a str {
    v.get(key).and_then(Value::as_str).unwrap_or("")
}
fn encoded(s: &str) -> String {
    let mut out = String::new();
    for byte in s.bytes() {
        if byte.is_ascii_alphanumeric() || b"-_.~".contains(&byte) {
            out.push(byte as char);
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}
fn slack_client(url: &str, workspace: &str) -> bool {
    let Ok(parsed) = url::Url::parse(url) else {
        return false;
    };
    let path: Vec<_> = parsed.path().split('/').collect();
    parsed.scheme() == "https"
        && parsed.host_str() == Some("app.slack.com")
        && parsed.username().is_empty()
        && parsed.password().is_none()
        && parsed.port().is_none()
        && path.get(1) == Some(&"client")
        && (workspace.is_empty() || path.get(2).is_some_and(|s| *s == encoded(workspace)))
}
fn cookie_domain(s: &str) -> bool {
    Regex::new(r"(?i)\A\.?(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)*slack\.com\z")
        .unwrap()
        .is_match(s)
}
fn valid_tokens(token: &str, cookie: &str) -> bool {
    Regex::new(r"\Axoxc-[A-Za-z0-9%._-]+\z")
        .unwrap()
        .is_match(token)
        && Regex::new(r"\Axoxd-[A-Za-z0-9%._/+=-]+\z")
            .unwrap()
            .is_match(cookie)
}
trait Browser {
    fn cookies(&mut self) -> Result<Vec<Value>>;
    fn tabs(&mut self) -> Result<Vec<Value>>;
    fn create(&mut self, url: &str) -> Result<String>;
    fn page(&mut self, url: &str) -> Result<()>;
    fn evaluate(&mut self) -> Result<Value>;
    fn detach(&mut self);
    fn close(&mut self, id: &str) -> Result<()>;
    fn pause(&mut self, ms: u64);
    fn authenticate(&mut self, token: &str, cookie: &str) -> Result<Value>;
}
struct Brave {
    endpoint: String,
    browser_url: String,
    http: Client,
    local: Client,
    browser: Option<CdpSocket>,
    page: Option<CdpSocket>,
}
impl Brave {
    fn new(endpoint: String) -> Result<Self> {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let local = Client::builder()
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .timeout(Duration::from_secs(5))
            .build()
            .map_err(|_| Error::new("could not initialize CDP"))?;
        let value: Value = local
            .get(format!("{endpoint}/json/version"))
            .send()
            .and_then(|r| r.error_for_status())
            .and_then(|r| r.json())
            .map_err(|_| Error::new("Brave CDP is unavailable"))?;
        let browser_url = text(&value, "webSocketDebuggerUrl").to_owned();
        if browser_url.is_empty() {
            return Err(Error::new("Brave did not provide a debugger URL"));
        }
        let http = Client::builder()
            .redirect(reqwest::redirect::Policy::none())
            .timeout(Duration::from_secs(30))
            .build()
            .map_err(|_| Error::new("could not initialize Slack HTTPS"))?;
        Ok(Self {
            endpoint,
            browser_url,
            http,
            local,
            browser: None,
            page: None,
        })
    }
    fn socket(&self, url: &str) -> Result<CdpSocket> {
        CdpSocket::with_limit(&self.endpoint, url, 50_000_000)
    }
}
impl Browser for Brave {
    fn cookies(&mut self) -> Result<Vec<Value>> {
        let mut socket = self.socket(&self.browser_url)?;
        let data = socket.call("Storage.getCookies", json!({}))?;
        Ok(data["cookies"].as_array().cloned().unwrap_or_default())
    }
    fn tabs(&mut self) -> Result<Vec<Value>> {
        self.local
            .get(format!("{}/json/list", self.endpoint))
            .send()
            .and_then(|r| r.error_for_status())
            .and_then(|r| r.json())
            .map_err(|_| Error::new("could not list Brave tabs"))
    }
    fn create(&mut self, url: &str) -> Result<String> {
        let mut browser = self.socket(&self.browser_url)?;
        let created = browser.call("Target.createTarget", json!({"url":url,"background":true}))?;
        let id = text(&created, "targetId").to_owned();
        self.browser = Some(browser);
        if id.is_empty() {
            Err(Error::new("could not create a temporary Slack refresh tab"))
        } else {
            Ok(id)
        }
    }
    fn page(&mut self, url: &str) -> Result<()> {
        let mut page = self.socket(url)?;
        page.call("Page.enable", json!({}))?;
        page.call("Runtime.enable", json!({}))?;
        self.page = Some(page);
        Ok(())
    }
    fn evaluate(&mut self) -> Result<Value> {
        let page = self
            .page
            .as_mut()
            .ok_or_else(|| Error::new("Slack page is unavailable"))?;
        let result = page.call(
            "Runtime.evaluate",
            json!({"expression":include_str!("tokens.js"),"returnByValue":true}),
        )?;
        let value = result["result"]["value"].as_str().unwrap_or("");
        if value.is_empty() {
            return Ok(Value::Null);
        }
        serde_json::from_str(value).map_err(|_| Error::new("Slack page returned invalid data"))
    }
    fn detach(&mut self) {
        self.page.take();
    }
    fn close(&mut self, id: &str) -> Result<()> {
        let browser = self
            .browser
            .as_mut()
            .ok_or_else(|| Error::new("browser debugger unavailable"))?;
        browser.timeout(Duration::from_secs(5))?;
        let result = browser.call("Target.closeTarget", json!({"targetId":id}));
        self.browser.take();
        result.map(|_| ())
    }
    fn pause(&mut self, ms: u64) {
        thread::sleep(Duration::from_millis(ms));
    }
    fn authenticate(&mut self, token: &str, cookie: &str) -> Result<Value> {
        self.http
            .post("https://slack.com/api/auth.test")
            .header("Authorization", format!("Bearer {token}"))
            .header("Cookie", format!("d={cookie}"))
            .body("")
            .send()
            .and_then(|r| r.error_for_status())
            .and_then(|r| r.json())
            .map_err(|_| Error::new("Slack auth.test request failed; previous session retained"))
    }
}
fn private_directory(path: &Path) -> Result<()> {
    fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)
        .and_then(|_| fs::set_permissions(path, fs::Permissions::from_mode(0o700)))
        .map_err(|_| Error::new("could not protect Slack session directory"))
}
fn persist(path: &Path, token: &str, cookie: &str) -> Result<()> {
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    private_directory(parent)?;
    let mut temp = tempfile::Builder::new()
        .prefix(".tokens-")
        .tempfile_in(parent)
        .map_err(|_| Error::new("could not create private Slack session file"))?;
    temp.as_file()
        .set_permissions(fs::Permissions::from_mode(0o600))
        .and_then(|_| write!(temp, "SLACK_COOKIE_D={cookie}\nSLACK_XOXC={token}\n"))
        .and_then(|_| temp.flush())
        .and_then(|_| temp.as_file().sync_all())
        .map_err(|_| Error::new("could not write Slack session file"))?;
    temp.persist(path)
        .map_err(|_| Error::new("could not publish Slack session; previous session retained"))?;
    Ok(())
}
fn refresh(browser: &mut dyn Browser, workspace: &str, path: &Path) -> Result<Value> {
    let cookie = browser
        .cookies()?
        .iter()
        .find(|c| text(c, "name") == "d" && cookie_domain(text(c, "domain")))
        .map(|c| text(c, "value").to_owned())
        .filter(|s| s.starts_with("xoxd-"))
        .ok_or_else(|| Error::new("no Slack cookie d=xoxd found; log into Slack in Brave first"))?;
    let mut owned = None;
    let token = (|| {
        let existing = browser.tabs()?.into_iter().find(|tab| {
            text(tab, "type") == "page"
                && !text(tab, "webSocketDebuggerUrl").is_empty()
                && slack_client(text(tab, "url"), workspace)
        });
        let page = if let Some(page) = existing {
            page
        } else {
            let url = if workspace.is_empty() {
                "https://app.slack.com/client".to_owned()
            } else {
                format!("https://app.slack.com/client/{}", encoded(workspace))
            };
            let id = browser.create(&url)?;
            owned = Some(id.clone());
            let mut page = None;
            for _ in 0..20 {
                page = browser
                    .tabs()?
                    .into_iter()
                    .find(|t| text(t, "id") == id && !text(t, "webSocketDebuggerUrl").is_empty());
                if page.is_some() {
                    break;
                }
                browser.pause(250);
            }
            page.ok_or_else(|| Error::new("could not connect to the temporary Slack refresh tab"))?
        };
        browser.page(text(&page, "webSocketDebuggerUrl"))?;
        for _ in 0..45 {
            browser.pause(1000);
            let value = browser.evaluate()?;
            if slack_client(text(&value, "url"), workspace) {
                if let Some(token) = value["tokens"]
                    .as_array()
                    .and_then(|a| a.first())
                    .and_then(Value::as_str)
                {
                    return Ok(token.to_owned());
                }
            }
        }
        Err(Error::new(
            "failed to find xoxc token from Slack client page",
        ))
    })();
    browser.detach();
    if let Some(id) = owned {
        if browser.close(&id).is_err() {
            eprintln!("warning: could not close temporary Slack refresh tab");
        }
    }
    let token = token?;
    if !valid_tokens(&token, &cookie) {
        return Err(Error::new("invalid Slack credential format"));
    }
    let auth = browser.authenticate(&token, &cookie)?;
    if auth["ok"] != true {
        return Err(Error::new("auth.test failed; previous session retained"));
    }
    persist(path, &token, &cookie)?;
    Ok(auth)
}
fn find_program(name: &str) -> Option<PathBuf> {
    env::split_paths(&env::var_os("PATH")?)
        .map(|p| p.join(name))
        .find(|p| {
            p.is_file()
                && p.metadata()
                    .is_ok_and(|m| m.permissions().mode() & 0o111 != 0)
        })
}
fn sops_update(path: &Path, repo: &Path, sops: &Path) -> Result<()> {
    let raw = fs::read_to_string(path)
        .map_err(|_| Error::new("could not read persisted Slack credentials"))?;
    let mut token = None;
    let mut cookie = None;
    for line in raw.lines() {
        if let Some((key, value)) = line.split_once('=') {
            match key {
                "SLACK_XOXC" => token = Some(value),
                "SLACK_COOKIE_D" => cookie = Some(value),
                _ => {}
            }
        }
    }
    let (Some(token), Some(cookie)) = (token, cookie) else {
        return Err(Error::new("invalid persisted Slack credentials"));
    };
    if !valid_tokens(token, cookie) {
        return Err(Error::new("invalid persisted Slack credentials"));
    }
    for (key, value) in [("SLACK_COOKIE_D", cookie), ("SLACK_XOXC", token)] {
        let mut child = Command::new(sops)
            .args(["set", "--value-stdin"])
            .arg(repo.join("secrets/env.yaml"))
            .arg(format!("[\"{key}\"]"))
            .current_dir(repo)
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|_| Error::new("could not run SOPS"))?;
        let input = serde_json::to_string(value)
            .map_err(|_| Error::new("could not encode Slack credential"))?;
        if child
            .stdin
            .take()
            .is_none_or(|mut stream| stream.write_all(input.as_bytes()).is_err())
        {
            let _ = child.kill();
            let _ = child.wait();
            return Err(Error::new("SOPS input failed"));
        }
        if !child.wait().is_ok_and(|s| s.success()) {
            return Err(Error::new("SOPS update failed"));
        }
        println!("sops updated {key}");
    }
    Ok(())
}
fn nonempty(name: &str) -> Option<String> {
    env::var(name).ok().filter(|s| !s.is_empty())
}
fn run() -> Result<()> {
    #[allow(deprecated)]
    let home = env::home_dir().ok_or_else(|| Error::new("home directory unavailable"))?;
    let directory = nonempty("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home.join(".config"))
        .join("slack-session");
    private_directory(&directory)?;
    let path = nonempty("OUT_FILE")
        .map(PathBuf::from)
        .unwrap_or_else(|| directory.join("tokens.env"));
    let workspace = nonempty("SLACK_TEAM_ID").unwrap_or_default();
    let endpoint = nonempty("SLACK_CDP_URL").unwrap_or_else(|| "http://127.0.0.1:9222".into());
    let mut browser = Brave::new(endpoint.trim_end_matches('/').into())?;
    let auth = refresh(&mut browser, workspace.trim(), &path)?;
    println!("wrote {} (xoxc+cookie d)", path.display());
    println!(
        "auth ok user={} team={}",
        text(&auth, "user"),
        text(&auth, "team")
    );
    if nonempty("SLACK_UPDATE_SOPS").unwrap_or_else(|| "1".into()) == "1" {
        if let Some(sops) = find_program("sops") {
            let repo = nonempty("NIX_CONFIG_ROOT")
                .or_else(|| nonempty("SLACK_DOTFILES_ROOT"))
                .map(PathBuf::from)
                .unwrap_or_else(|| home.join("Repositories/nix-config"));
            if fs::File::open(repo.join("secrets/env.yaml")).is_ok()
                && fs::File::open(&path).is_ok()
            {
                sops_update(&path, &repo, &sops)?;
                println!(
                    "note: commit/push nix-config, then in ~/.dotfiles update its input, build and hmswitch"
                );
            }
        }
    }
    Ok(())
}
fn main() {
    if env::args().nth(1).as_deref() == Some("--help") {
        println!(
            "slack-refresh-session: refresh the private Slack browser session from visible Brave; SLACK_UPDATE_SOPS=0 disables the optional encrypted-secret update"
        );
        return;
    }
    std::panic::set_hook(Box::new(|_| {
        eprintln!("Slack refresh failed; credential values have not been logged.")
    }));
    if let Err(error) = run() {
        eprintln!("Slack refresh failed: {}", error.0);
        std::process::exit(1);
    }
}
