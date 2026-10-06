use crate::runtime::{Runtime, private_directory, write_private};
use crate::{Error, Result};
use chrono::{DateTime, Utc};
use regex::Regex;
use serde_json::{Value, json};
use std::{
    collections::HashMap,
    fs,
    path::{Path, PathBuf},
    sync::OnceLock,
    time::Duration,
};

pub const DIRECTORY: &str = "d-9c677042e2";
pub const ORIGIN: &str = "https://d-9c677042e2.awsapps.com";
pub const START: &str = "https://d-9c677042e2.awsapps.com/start/#/?tab=accounts";
pub const SIGNIN: &str = "eu-west-2.signin.aws";
pub const REGION: &str = "eu-west-2";
pub const PRODUCTION: &str = "382373577178";
pub const USERNAME: &str = "#username-input";
pub const USERNAME_BUTTON: &str = "#username-submit-button";
pub const PASSWORD: &str = "[data-testid=\"test-password-input\"]";
pub const PASSWORD_BUTTON: &str = "#password-submit-button";
pub const MFA: &str = "[data-testid=\"vmfa-authentication\"] [data-testid=\"test-input\"]";
pub const MFA_BUTTON: &str =
    "[data-testid=\"vmfa-authentication\"] [data-testid=\"test-primary-button\"]";
pub trait Backend {
    fn token(&mut self) -> Result<Option<String>>;
    fn api(&mut self, token: &str, path: &str, query: &[(&str, String)]) -> Result<Value>;
    fn open(&mut self) -> Result<()>;
    fn navigate(&mut self, url: &str) -> Result<()>;
    fn state(&mut self) -> Result<Value>;
    fn fill(&mut self, field: &str, button: &str, value: &str) -> Result<bool>;
    fn close(&mut self) -> Result<()>;
}
pub fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}
fn number(value: &Value, key: &str) -> Option<i64> {
    let n = value.get(key)?.as_f64()?;
    if !n.is_finite() || n < i64::MIN as f64 || n >= i64::MAX as f64 {
        None
    } else {
        Some(n as i64)
    }
}
pub fn iso(ms: i64) -> Result<String> {
    DateTime::<Utc>::from_timestamp_millis(ms)
        .map(|t| t.format("%Y-%m-%dT%H:%M:%SZ").to_string())
        .ok_or_else(|| Error::new("portal returned an invalid expiry"))
}
pub fn production_admin(id: &str, name: &str, role: &str) -> bool {
    (id == PRODUCTION || name.trim().eq_ignore_ascii_case("production"))
        && role.to_ascii_lowercase().contains("administrator")
}
fn regex(pattern: &'static str, value: &str) -> bool {
    Regex::new(pattern).unwrap().is_match(value)
}
pub fn account_id(value: &str) -> bool {
    value.len() == 12 && value.bytes().all(|b| b.is_ascii_digit())
}
fn valid_session(client: &mut dyn Backend, rt: &dyn Runtime, token: &str) -> bool {
    client
        .api(token, "/token/whoAmI", &[])
        .ok()
        .and_then(|v| number(&v, "expireDate"))
        .is_some_and(|expiry| expiry > rt.now() + 60_000)
}
pub fn status(client: &mut dyn Backend, rt: &dyn Runtime) -> Result<Value> {
    let Some(token) = client.token()? else {
        return Ok(json!({"logged_in":false}));
    };
    let Ok(body) = client.api(&token, "/token/whoAmI", &[]) else {
        return Ok(json!({"logged_in":false}));
    };
    let Some(expiry) = number(&body, "expireDate").filter(|n| *n > rt.now()) else {
        return Ok(json!({"logged_in":false}));
    };
    Ok(
        json!({"logged_in":true,"directory_id":if text(&body,"directoryId").is_empty(){DIRECTORY}else{text(&body,"directoryId")},"expires_at":iso(expiry)?,"expires_in_seconds":((expiry-rt.now())/1000).max(0)}),
    )
}
#[derive(Clone, Copy, PartialEq, Eq)]
enum Step {
    Portal,
    Username,
    Password,
    Mfa,
    Signin,
    Unknown,
}
fn classify(state: &Value) -> Step {
    let href = text(state, "href");
    if state["accounts"].as_bool() == Some(true) && href.contains(ORIGIN) && !href.contains(SIGNIN)
    {
        Step::Portal
    } else if state["mfa"].as_bool() == Some(true) {
        Step::Mfa
    } else if state["password"].as_bool() == Some(true) {
        Step::Password
    } else if state["username"].as_bool() == Some(true) {
        Step::Username
    } else if href.contains("signin.aws") {
        Step::Signin
    } else {
        Step::Unknown
    }
}
fn wait_step(client: &mut dyn Backend, rt: &mut dyn Runtime, old: Step) -> Result<()> {
    let end = rt.monotonic() + 25_000;
    while rt.monotonic() < end {
        rt.sleep(Duration::from_millis(400));
        let state = client.state()?;
        if classify(&state) != old {
            let alert = text(&state, "alert");
            return if alert.is_empty() {
                Ok(())
            } else {
                Err(Error::new(alert))
            };
        }
    }
    Err(Error::new("timed out waiting for the next sign-in step"))
}
fn submit_mfa(client: &mut dyn Backend, rt: &mut dyn Runtime) -> Result<()> {
    let mut last_alert = String::new();
    for _ in 0..2 {
        let code = rt.totp()?;
        if !client.fill(MFA, MFA_BUTTON, &code)? {
            return Err(Error::new("MFA field was not submitted"));
        }
        let end = rt.monotonic() + 20_000;
        let mut retry = false;
        while rt.monotonic() < end {
            rt.sleep(Duration::from_millis(400));
            let state = client.state()?;
            if classify(&state) == Step::Portal {
                return Ok(());
            }
            let alert = text(&state, "alert");
            if !alert.is_empty() && alert != last_alert && classify(&state) == Step::Mfa {
                last_alert = alert.to_owned();
                retry = true;
                break;
            }
        }
        if !retry {
            break;
        }
    }
    Err(Error::new("MFA was rejected"))
}
fn login_page(client: &mut dyn Backend, rt: &mut dyn Runtime) -> Result<()> {
    client.navigate(START)?;
    let end = rt.monotonic() + 45_000;
    let mut arrived = false;
    while rt.monotonic() < end {
        rt.sleep(Duration::from_millis(400));
        let state = client.state()?;
        match classify(&state) {
            Step::Portal => {
                arrived = true;
                break;
            }
            Step::Username => {
                let value = rt.secret("AWS_USERNAME")?;
                if !client.fill(USERNAME, USERNAME_BUTTON, &value)? {
                    return Err(Error::new("username field was not submitted"));
                }
                wait_step(client, rt, Step::Username)?;
            }
            Step::Password => {
                let value = rt.secret("AWS_PASSWORD")?;
                if !client.fill(PASSWORD, PASSWORD_BUTTON, &value)? {
                    return Err(Error::new("password field was not submitted"));
                }
                wait_step(client, rt, Step::Password)?;
            }
            Step::Mfa => {
                submit_mfa(client, rt)?;
                arrived = true;
                break;
            }
            _ => {}
        }
    }
    if !arrived {
        return Err(Error::new("timed out waiting for the access portal"));
    }
    let state = client.state()?;
    if classify(&state) != Step::Portal {
        return Err(Error::new(format!(
            "login did not reach the accounts tab: {}",
            if text(&state, "alert").is_empty() {
                "still not on the accounts tab"
            } else {
                text(&state, "alert")
            }
        )));
    }
    Ok(())
}
pub fn login(client: &mut dyn Backend, rt: &mut dyn Runtime) -> Result<bool> {
    if let Some(token) = client.token()? {
        if valid_session(client, rt, &token) {
            return Ok(false);
        }
    }
    client.open()?;
    let result = login_page(client, rt);
    client.close()?;
    result?;
    let token = client
        .token()?
        .ok_or_else(|| Error::new("login finished but the portal session is not usable"))?;
    if !valid_session(client, rt, &token) {
        return Err(Error::new(
            "login finished but the portal session is not usable",
        ));
    }
    Ok(true)
}
fn require_token(client: &mut dyn Backend, rt: &mut dyn Runtime) -> Result<String> {
    let token = client.token()?;
    if token
        .as_ref()
        .is_some_and(|token| valid_session(client, rt, token))
    {
        return Ok(token.unwrap());
    }
    login(client, rt)?;
    client
        .token()?
        .ok_or_else(|| Error::new("portal session is missing after login"))
}
fn collect(
    client: &mut dyn Backend,
    token: &str,
    path: &str,
    key: &str,
    query: &[(&str, String)],
) -> Result<Vec<Value>> {
    let mut out = vec![];
    let mut next = String::new();
    for _ in 0..20 {
        let mut query = query.to_vec();
        if !next.is_empty() {
            query.push(("next_token", next));
        }
        let body = client.api(token, path, &query)?;
        if let Some(items) = body.get(key).and_then(Value::as_array) {
            out.extend(items.iter().cloned());
        }
        next = text(&body, "nextToken").to_owned();
        if next.is_empty() {
            return Ok(out);
        }
    }
    Err(Error::new(format!("{path} did not finish paging")))
}
#[derive(Clone)]
pub struct Account {
    pub id: String,
    pub name: String,
}
pub fn accounts(client: &mut dyn Backend, rt: &mut dyn Runtime) -> Result<Vec<Account>> {
    let token = require_token(client, rt)?;
    collect(client, &token, "/assignment/accounts", "accountList", &[])?
        .into_iter()
        .map(|v| {
            let id = text(&v, "accountId");
            let name = text(&v, "accountName");
            if !account_id(id) || name.chars().any(char::is_control) {
                return Err(Error::new("portal returned an invalid account"));
            }
            Ok(Account {
                id: id.to_owned(),
                name: name.to_owned(),
            })
        })
        .collect()
}
fn resolve(
    client: &mut dyn Backend,
    rt: &mut dyn Runtime,
    id: Option<&str>,
    name: Option<&str>,
) -> Result<Account> {
    if id.is_some_and(|id| !account_id(id)) {
        return Err(Error::new("account_id must be a 12 digit AWS account id"));
    }
    if name.is_some_and(|name| !regex(r"\A[A-Za-z0-9 ._+\-]{1,64}\z", name)) {
        return Err(Error::new("account_name has unexpected characters"));
    }
    if id.is_none() && name.is_none() {
        return Err(Error::new("account_id or account_name is required"));
    }
    let chosen: Vec<_> = accounts(client, rt)?
        .into_iter()
        .filter(|a| {
            id.is_none_or(|id| id == a.id)
                && name.is_none_or(|name| name.eq_ignore_ascii_case(&a.name))
        })
        .collect();
    match chosen.len() {
        0 => Err(Error::new("no matching AWS account")),
        1 => Ok(chosen.into_iter().next().unwrap()),
        _ => Err(Error::new("account name is ambiguous")),
    }
}
pub fn roles(
    client: &mut dyn Backend,
    rt: &mut dyn Runtime,
    id: Option<&str>,
    name: Option<&str>,
) -> Result<String> {
    let a = resolve(client, rt, id, name)?;
    let token = require_token(client, rt)?;
    let roles = collect(
        client,
        &token,
        "/assignment/roles",
        "roleList",
        &[("account_id", a.id.clone())],
    )?;
    let mut lines = vec![format!("{} {}", a.name, a.id)];
    for role in roles {
        let name = text(&role, "roleName");
        lines.push(format!(
            "{name}{}",
            if production_admin(&a.id, &a.name, name) {
                " (approval required)"
            } else {
                ""
            }
        ));
    }
    Ok(lines.join("\n"))
}
fn credential_quote(value: &str) -> Result<String> {
    if value.contains(['\'', '\n', '\r', '\0']) {
        Err(Error::new("credential value has an unexpected character"))
    } else {
        Ok(format!("'{value}'"))
    }
}
fn shell_quote(path: &Path) -> String {
    let s = path.to_string_lossy();
    if s.bytes()
        .all(|b| b.is_ascii_alphanumeric() || b"_@%+=:,./-".contains(&b))
    {
        s.into_owned()
    } else {
        format!("'{}'", s.replace('\'', "'\"'\"'"))
    }
}
fn read_cache(path: &Path, now: i64) -> Result<Option<HashMap<String, String>>> {
    if !path.is_file() {
        return Ok(None);
    }
    let Ok(text) = fs::read_to_string(path) else {
        return Ok(None);
    };
    let mut meta = HashMap::new();
    for line in text.lines() {
        let Some(line) = line.strip_prefix("# ") else {
            break;
        };
        if let Some((key, value)) = line.split_once('=') {
            meta.insert(key.to_owned(), value.to_owned());
        }
    }
    let Some(expiry) = meta
        .get("expiration")
        .filter(|s| s.bytes().all(|b| b.is_ascii_digit()))
        .and_then(|s| s.parse::<i64>().ok())
    else {
        return Ok(None);
    };
    if expiry <= now + 300_000 {
        match fs::remove_file(path) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err(Error::new("could not expire cached credentials")),
        }
        return Ok(None);
    }
    Ok(Some(meta))
}
pub struct Export {
    pub account: Account,
    pub role: String,
    pub expiry: i64,
    pub path: PathBuf,
    pub cached: bool,
}
impl Export {
    pub fn display(&self) -> Result<String> {
        Ok(format!(
            "{} {}{}\nuntil {}\nsource {}",
            self.account.name,
            self.role,
            if self.cached { " cached" } else { "" },
            iso(self.expiry)?,
            shell_quote(&self.path)
        ))
    }
}
pub fn export(
    client: &mut dyn Backend,
    rt: &mut dyn Runtime,
    role: &str,
    id: Option<&str>,
    name: Option<&str>,
    region: &str,
) -> Result<Export> {
    static ROLE: OnceLock<Regex> = OnceLock::new();
    if !ROLE
        .get_or_init(|| Regex::new(r"\A[A-Za-z0-9+=,.@_\-]{1,64}\z").unwrap())
        .is_match(role)
    {
        return Err(Error::new(
            "role_name is required and must be an IAM role name",
        ));
    }
    if !regex(r"\A[a-z]{2}-[a-z]+-[0-9]\z", region) {
        return Err(Error::new("region must look like eu-west-2"));
    }
    let a = resolve(client, rt, id, name)?;
    if production_admin(&a.id, &a.name, role) && !rt.approve(&a.name, &a.id, role)? {
        return Err(Error::new(
            "production administrator credentials were not approved",
        ));
    }
    let root = rt.state();
    private_directory(&root)?;
    let path = root.join(format!("{}-{role}.env", a.id));
    if let Some(cached) = read_cache(&path, rt.now())? {
        if cached.get("account_id") == Some(&a.id)
            && cached.get("role_name").map(String::as_str) == Some(role)
            && cached.get("region").map(String::as_str) == Some(region)
        {
            return Ok(Export {
                account: a,
                role: role.to_owned(),
                expiry: cached["expiration"].parse().unwrap(),
                path,
                cached: true,
            });
        }
    }
    let token = require_token(client, rt)?;
    let body = client.api(
        &token,
        "/federation/credentials",
        &[("account_id", a.id.clone()), ("role_name", role.to_owned())],
    )?;
    let creds = &body["roleCredentials"];
    let access = creds
        .get("accessKeyId")
        .and_then(Value::as_str)
        .filter(|s| s.starts_with("ASIA"));
    let secret = creds.get("secretAccessKey").and_then(Value::as_str);
    let session = creds.get("sessionToken").and_then(Value::as_str);
    let expiry = number(creds, "expiration");
    let (Some(access), Some(secret), Some(session), Some(expiry)) =
        (access, secret, session, expiry)
    else {
        return Err(Error::new("portal did not return usable role credentials"));
    };
    let env = format!(
        "# aws-access-portal v1\n# account_id={}\n# account_name={}\n# role_name={role}\n# expiration={expiry}\n# region={region}\nunset AWS_PROFILE\nexport AWS_ACCESS_KEY_ID={}\nexport AWS_SECRET_ACCESS_KEY={}\nexport AWS_SESSION_TOKEN={}\nexport AWS_DEFAULT_REGION={}\nexport AWS_REGION={}\n",
        a.id,
        a.name,
        credential_quote(access)?,
        credential_quote(secret)?,
        credential_quote(session)?,
        credential_quote(region)?,
        credential_quote(region)?
    );
    write_private(&path, &env)?;
    Ok(Export {
        account: a,
        role: role.to_owned(),
        expiry,
        path,
        cached: false,
    })
}
