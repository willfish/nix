use super::*;
use crate::portal::Backend;
use crate::runtime::Runtime;
use std::{
    cell::RefCell, collections::VecDeque, os::unix::fs::PermissionsExt, path::PathBuf, rc::Rc,
    time::Duration,
};
const SECRET: &str = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
const TOKEN: &str = "IQoJb3JpZ2luX2VjEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
type Events = Rc<RefCell<Vec<String>>>;
struct Fake {
    events: Events,
    token: Option<String>,
    stage: usize,
    reject_mfa: bool,
    fail_fill: bool,
    pages: VecDeque<Value>,
}
impl Fake {
    fn new(events: Events) -> Self {
        Self {
            events,
            token: Some("portal-session".into()),
            stage: 0,
            reject_mfa: false,
            fail_fill: false,
            pages: VecDeque::new(),
        }
    }
}
impl Backend for Fake {
    fn token(&mut self) -> Result<Option<String>> {
        Ok(self.token.clone())
    }
    fn api(&mut self, _token: &str, path: &str, _query: &[(&str, String)]) -> Result<Value> {
        self.events.borrow_mut().push(path.into());
        Ok(match path{
 "/token/whoAmI"=>json!({"directoryId":portal::DIRECTORY,"expireDate":9_000_000_000_000i64}),
 "/assignment/accounts"=>self.pages.pop_front().unwrap_or(json!({"accountList":[{"accountId":"844815912454","accountName":"Development"},{"accountId":portal::PRODUCTION,"accountName":"Production"}]})),
 "/assignment/roles"=>json!({"roleList":[{"roleName":"TariffReadOnlyAccess"},{"roleName":"TariffAdministratorAccess"}]}),
 "/federation/credentials"=>json!({"roleCredentials":{"accessKeyId":"ASIAIOSFODNN7EXAMPLE","secretAccessKey":SECRET,"sessionToken":TOKEN,"expiration":9_000_000_000_000i64}}),
 _=>return Err(Error::new("unexpected fake endpoint"))})
    }
    fn open(&mut self) -> Result<()> {
        self.events.borrow_mut().push("open-background".into());
        Ok(())
    }
    fn navigate(&mut self, url: &str) -> Result<()> {
        assert_eq!(url, portal::START);
        Ok(())
    }
    fn state(&mut self) -> Result<Value> {
        Ok(
            json!({"href":if self.stage==3{portal::START}else{"https://eu-west-2.signin.aws/platform/d-9c677042e2"},"username":self.stage==0,"password":self.stage==1,"mfa":self.stage==2,"accounts":self.stage==3,"alert":if self.reject_mfa&&self.stage==2{"code rejected"}else{""},"focused":false}),
        )
    }
    fn fill(&mut self, field: &str, button: &str, value: &str) -> Result<bool> {
        self.events.borrow_mut().push(field.into());
        if self.fail_fill {
            return Ok(false);
        }
        match self.stage {
            0 => {
                assert_eq!(
                    (field, button, value),
                    (portal::USERNAME, portal::USERNAME_BUTTON, "fixture-user")
                );
                self.stage = 1;
            }
            1 => {
                assert_eq!(
                    (field, button, value),
                    (portal::PASSWORD, portal::PASSWORD_BUTTON, "short-password")
                );
                self.stage = 2;
            }
            2 => {
                assert_eq!(
                    (field, button, value),
                    (portal::MFA, portal::MFA_BUTTON, "123456")
                );
                if !self.reject_mfa {
                    self.stage = 3;
                    self.token = Some("new-session".into());
                }
            }
            _ => panic!("unexpected fill"),
        };
        Ok(true)
    }
    fn close(&mut self) -> Result<()> {
        self.events.borrow_mut().push("close-background".into());
        Ok(())
    }
}
struct Clock {
    directory: tempfile::TempDir,
    elapsed: u128,
    approved: bool,
    events: Events,
    totps: usize,
}
impl Clock {
    fn new(events: Events) -> Self {
        Self {
            directory: tempfile::tempdir().unwrap(),
            elapsed: 0,
            approved: false,
            events,
            totps: 0,
        }
    }
}
impl Runtime for Clock {
    fn now(&self) -> i64 {
        1_700_000_000_000
    }
    fn monotonic(&self) -> u128 {
        self.elapsed
    }
    fn sleep(&mut self, d: Duration) {
        self.elapsed += d.as_millis();
    }
    fn secret(&mut self, name: &str) -> Result<String> {
        Ok(if name == "AWS_USERNAME" {
            "fixture-user"
        } else {
            "short-password"
        }
        .into())
    }
    fn totp(&mut self) -> Result<String> {
        self.totps += 1;
        Ok("123456".into())
    }
    fn approve(&mut self, name: &str, id: &str, role: &str) -> Result<bool> {
        assert_eq!(name, "Production");
        assert_eq!(id, portal::PRODUCTION);
        assert!(role.contains("Administrator"));
        self.events.borrow_mut().push("approval".into());
        Ok(self.approved)
    }
    fn state(&self) -> PathBuf {
        self.directory.path().join("state")
    }
}
fn fixtures() -> (Fake, Clock, Events) {
    let events = Rc::new(RefCell::new(vec![]));
    (
        Fake::new(events.clone()),
        Clock::new(events.clone()),
        events,
    )
}
#[test]
fn initialize_tools_and_plain_text() {
    let initialize = handle(json!({"id":1,"method":"initialize"})).unwrap();
    assert_eq!(
        initialize["result"]["serverInfo"]["name"],
        "aws-access-portal"
    );
    assert!(
        initialize["result"]["instructions"]
            .as_str()
            .unwrap()
            .contains("local approval")
    );
    let tools = handle(json!({"id":2,"method":"tools/list"})).unwrap();
    let names: Vec<_> = tools["result"]["tools"]
        .as_array()
        .unwrap()
        .iter()
        .map(|t| t["name"].as_str().unwrap())
        .collect();
    assert_eq!(
        names,
        [
            "status",
            "login",
            "list_accounts",
            "list_roles",
            "export_credentials"
        ]
    );
    assert!(handle(json!({"method":"notifications/initialized"})).is_none());
    let (mut client, mut rt, _) = fixtures();
    let result = call("status", &json!({}), &mut client, &mut rt).unwrap();
    assert!(result.starts_with("Signed in until "));
    assert!(!result.contains('{'));
}
#[test]
fn private_export_returns_no_secrets_and_reuses_cache() {
    let (mut client, mut rt, events) = fixtures();
    let first = portal::export(
        &mut client,
        &mut rt,
        "TariffReadOnlyAccess",
        None,
        Some("Development"),
        portal::REGION,
    )
    .unwrap();
    let text = std::fs::read_to_string(&first.path).unwrap();
    assert!(text.contains(SECRET) && text.contains(TOKEN) && text.contains("unset AWS_PROFILE"));
    assert_eq!(
        std::fs::metadata(&first.path).unwrap().permissions().mode() & 0o777,
        0o600
    );
    assert_eq!(
        std::fs::metadata(first.path.parent().unwrap())
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o700
    );
    let public = first.display().unwrap();
    assert!(!public.contains(SECRET) && !public.contains(TOKEN) && public.contains("source "));
    events.borrow_mut().clear();
    let second = portal::export(
        &mut client,
        &mut rt,
        "TariffReadOnlyAccess",
        Some("844815912454"),
        None,
        portal::REGION,
    )
    .unwrap();
    assert!(second.cached);
    assert!(
        !events
            .borrow()
            .iter()
            .any(|e| e == "/federation/credentials" || e == "approval" || e == "open-background")
    );
}
#[test]
fn production_approval_precedes_fetch_and_cached_exports() {
    let (mut client, mut rt, events) = fixtures();
    let error=call("export_credentials",&json!({"account_name":"Production","role_name":"TariffAdministratorAccess","approved":true,"AWS_PORTAL_ALLOW_APPROVAL_HOOK":"1"}),&mut client,&mut rt).err().unwrap();
    assert!(error.0.contains("not approved"));
    assert!(
        !events
            .borrow()
            .iter()
            .any(|e| e == "/federation/credentials")
    );
    assert!(!rt.state().exists());
    rt.approved = true;
    events.borrow_mut().clear();
    let result = portal::export(
        &mut client,
        &mut rt,
        "TariffAdministratorAccess",
        Some(portal::PRODUCTION),
        None,
        portal::REGION,
    )
    .unwrap();
    assert!(!result.cached);
    let events_ = events.borrow();
    assert!(
        events_.iter().position(|s| s == "approval").unwrap()
            < events_
                .iter()
                .position(|s| s == "/federation/credentials")
                .unwrap()
    );
    drop(events_);
    rt.approved = false;
    events.borrow_mut().clear();
    assert!(
        portal::export(
            &mut client,
            &mut rt,
            "TariffAdministratorAccess",
            Some(portal::PRODUCTION),
            None,
            portal::REGION
        )
        .is_err()
    );
    assert!(events.borrow().iter().any(|s| s == "approval"));
}
#[test]
fn development_admin_and_role_flags_keep_account_scope() {
    let (mut client, mut rt, events) = fixtures();
    portal::export(
        &mut client,
        &mut rt,
        "TariffAdministratorAccess",
        None,
        Some("development"),
        portal::REGION,
    )
    .unwrap();
    assert!(!events.borrow().iter().any(|s| s == "approval"));
    let roles = portal::roles(&mut client, &mut rt, None, Some("Production")).unwrap();
    assert!(roles.contains("TariffAdministratorAccess (approval required)"));
    assert!(!roles.contains("TariffReadOnlyAccess (approval required)"));
}
#[test]
fn login_uses_owned_background_page_and_closes_on_success_or_failure() {
    let (mut client, mut rt, events) = fixtures();
    client.token = None;
    assert!(portal::login(&mut client, &mut rt).unwrap());
    assert_eq!(rt.totps, 1);
    assert_eq!(
        events
            .borrow()
            .iter()
            .filter(|s| s.as_str() == "open-background")
            .count(),
        1
    );
    assert!(events.borrow().iter().any(|s| s == "close-background"));
    let (mut client, mut rt, events) = fixtures();
    client.token = None;
    client.fail_fill = true;
    assert!(portal::login(&mut client, &mut rt).is_err());
    assert_eq!(events.borrow().last().unwrap(), "close-background");
}
#[test]
fn mfa_is_limited_to_two_attempts_and_cleanup_is_not_skipped() {
    let (mut client, mut rt, events) = fixtures();
    client.token = None;
    client.stage = 2;
    client.reject_mfa = true;
    assert!(
        portal::login(&mut client, &mut rt)
            .err()
            .unwrap()
            .0
            .contains("MFA was rejected")
    );
    assert_eq!(rt.totps, 2);
    assert_eq!(events.borrow().last().unwrap(), "close-background");
}
#[test]
fn pagination_selection_region_and_stale_cache_are_preserved() {
    let (mut client, mut rt, events) = fixtures();
    client.pages.push_back(json!({"accountList":[{"accountId":"111111111111","accountName":"First"}],"nextToken":"second"}));
    client
        .pages
        .push_back(json!({"accountList":[{"accountId":"222222222222","accountName":"Second"}]}));
    assert_eq!(portal::accounts(&mut client, &mut rt).unwrap().len(), 2);
    for args in [
        json!({"account_id":"../escape","role_name":"ReadOnly"}),
        json!({"account_name":"Development","role_name":"../escape"}),
        json!({"account_name":"Development","role_name":"ReadOnly","region":"invalid"}),
    ] {
        assert!(call("export_credentials", &args, &mut client, &mut rt).is_err());
    }
    let export = portal::export(
        &mut client,
        &mut rt,
        "ReadOnly",
        None,
        Some("Development"),
        portal::REGION,
    )
    .unwrap();
    std::fs::write(&export.path, "# expiration=1\n").unwrap();
    events.borrow_mut().clear();
    assert!(
        !portal::export(
            &mut client,
            &mut rt,
            "ReadOnly",
            None,
            Some("Development"),
            portal::REGION
        )
        .unwrap()
        .cached
    );
    assert!(
        events
            .borrow()
            .iter()
            .any(|s| s == "/federation/credentials")
    );
}
#[test]
fn focus_is_refused_and_errors_redact_credentials() {
    assert!(client::focus_allowed("Target.activateTarget").is_err());
    assert!(client::focus_allowed("Page.bringToFront").is_err());
    assert!(!runtime::redact(SECRET).contains(SECRET));
    assert!(!runtime::redact("ASIAIOSFODNN7EXAMPLE").contains("ASIAIOSFODNN7EXAMPLE"));
    let mut local = Local::default();
    local.secrets.push("short-password".into());
    assert_eq!(
        local.safe_error("failed short-password"),
        "failed [redacted]"
    );
    assert!(!include_str!("runtime.rs").contains("AWS_PORTAL_ALLOW_APPROVAL_HOOK"));
}
#[test]
fn http_and_cdp_use_the_expected_read_only_protocol() {
    use std::io::{Read, Write};
    use std::net::TcpListener;
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let address = listener.local_addr().unwrap();
    let worker = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut header = vec![];
        while !header.ends_with(b"\r\n\r\n") {
            let mut byte = [0];
            stream.read_exact(&mut byte).unwrap();
            header.push(byte[0]);
        }
        assert!(
            String::from_utf8(header)
                .unwrap()
                .starts_with("GET /json/version ")
        );
        let body =
            json!({"webSocketDebuggerUrl":format!("ws://{address}/devtools/browser/fixture")})
                .to_string();
        write!(
            stream,
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        )
        .unwrap();
        drop(stream);
        let (stream, _) = listener.accept().unwrap();
        let mut ws = tungstenite::accept(stream).unwrap();
        let request: Value = serde_json::from_str(ws.read().unwrap().to_text().unwrap()).unwrap();
        assert_eq!(request["method"], "Storage.getCookies");
        ws.send(tungstenite::Message::Text(json!({"id":request["id"],"result":{"cookies":[{"name":"x-amz-sso_authn","domain":".d-9c677042e2.awsapps.com","value":"synthetic-session"}]}}).to_string().into())).unwrap();
        let _ = ws.read();
        drop(ws);
        let (mut stream, _) = listener.accept().unwrap();
        let mut header = vec![];
        while !header.ends_with(b"\r\n\r\n") {
            let mut byte = [0];
            stream.read_exact(&mut byte).unwrap();
            header.push(byte[0]);
        }
        let header = String::from_utf8(header).unwrap().to_ascii_lowercase();
        assert!(header.starts_with("get /assignment/roles?account_id=844815912454 "));
        for name in [
            "authorization: bearer synthetic-session",
            "x-amz-sso-bearer-token: synthetic-session",
            "x-amz-sso_bearer_token: synthetic-session",
        ] {
            assert!(header.contains(name));
        }
        let body = "{\"roleList\":[]}";
        write!(
            stream,
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        )
        .unwrap();
    });
    let mut client =
        client::Client::for_test(&format!("http://{address}"), &format!("http://{address}"))
            .unwrap();
    let token = client.token().unwrap().unwrap();
    assert_eq!(token, "synthetic-session");
    assert_eq!(
        client
            .api(
                &token,
                "/assignment/roles",
                &[("account_id", "844815912454".into())]
            )
            .unwrap(),
        json!({"roleList":[]})
    );
    worker.join().unwrap();
}
