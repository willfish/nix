use super::*;
use std::os::unix::fs::MetadataExt;
const TOKEN: &str = "xoxc-synthetic-test-token";
const COOKIE: &str = "xoxd-synthetic-test-cookie/+%2F==";
fn tab(id: &str, url: &str) -> Value {
    json!({"id":id,"url":url,"type":"page","webSocketDebuggerUrl":format!("ws://fixture/{id}")})
}
struct Fake {
    tabs: Vec<Value>,
    cookies: Vec<Value>,
    created: Vec<String>,
    closed: Vec<String>,
    evaluated: Vec<String>,
    page: String,
    publish: bool,
    evaluate_error: bool,
    auth_ok: bool,
    auth_error: bool,
}
impl Fake {
    fn new(tabs: Vec<Value>) -> Self {
        Self {
            tabs,
            cookies: vec![json!({"name":"d","domain":".slack.com","value":COOKIE})],
            created: vec![],
            closed: vec![],
            evaluated: vec![],
            page: String::new(),
            publish: true,
            evaluate_error: false,
            auth_ok: true,
            auth_error: false,
        }
    }
}
impl Browser for Fake {
    fn cookies(&mut self) -> Result<Vec<Value>> {
        Ok(self.cookies.clone())
    }
    fn tabs(&mut self) -> Result<Vec<Value>> {
        Ok(self
            .tabs
            .iter()
            .filter(|t| self.publish || t["id"] != "owned")
            .cloned()
            .collect())
    }
    fn create(&mut self, url: &str) -> Result<String> {
        self.created.push(url.into());
        self.tabs.push(tab("owned", url));
        Ok("owned".into())
    }
    fn page(&mut self, url: &str) -> Result<()> {
        self.page = url.rsplit('/').next().unwrap().into();
        Ok(())
    }
    fn evaluate(&mut self) -> Result<Value> {
        self.evaluated.push(self.page.clone());
        if self.evaluate_error {
            return Err(Error::new("fixture evaluation failure"));
        }
        let page = self.tabs.iter().find(|t| t["id"] == self.page).unwrap();
        Ok(json!({"url":page["url"],"tokens":[TOKEN]}))
    }
    fn detach(&mut self) {}
    fn close(&mut self, id: &str) -> Result<()> {
        self.closed.push(id.into());
        self.tabs.retain(|t| t["id"] != id);
        Ok(())
    }
    fn pause(&mut self, _: u64) {}
    fn authenticate(&mut self, token: &str, cookie: &str) -> Result<Value> {
        assert_eq!(token, TOKEN);
        assert_eq!(cookie, COOKIE);
        if self.auth_error {
            return Err(Error::new("auth unavailable"));
        }
        Ok(json!({"ok":self.auth_ok,"user":"fixture","team":"fixture"}))
    }
}
fn check(browser: &mut Fake, workspace: &str) -> bool {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("tokens.env");
    fs::write(&path, "previous session\n").unwrap();
    let inode = path.metadata().unwrap().ino();
    let result = refresh(browser, workspace, &path);
    if result.is_err() {
        assert_eq!(fs::read_to_string(&path).unwrap(), "previous session\n");
        assert_eq!(path.metadata().unwrap().ino(), inode);
    } else {
        assert_eq!(
            fs::read_to_string(&path).unwrap(),
            format!("SLACK_COOKIE_D={COOKIE}\nSLACK_XOXC={TOKEN}\n")
        );
        assert_ne!(path.metadata().unwrap().ino(), inode);
        assert_eq!(path.metadata().unwrap().permissions().mode() & 0o777, 0o600);
        assert_eq!(
            root.path().metadata().unwrap().permissions().mode() & 0o777,
            0o700
        );
    }
    assert!(fs::read_dir(root.path()).unwrap().all(|e| {
        !e.unwrap()
            .file_name()
            .to_string_lossy()
            .starts_with(".tokens-")
    }));
    if let Err(ref error) = result {
        assert!(!error.0.contains(TOKEN) && !error.0.contains(COOKIE));
    }
    result.is_ok()
}
#[test]
fn cookie_domains_are_exact() {
    for domain in [
        "evilslack.com",
        "slack.com.evil.invalid",
        "notslack.invalid",
        "..slack.com",
        "evil@.slack.com",
    ] {
        let mut fake = Fake::new(vec![]);
        fake.cookies[0]["domain"] = json!(domain);
        assert!(!check(&mut fake, ""));
        assert!(fake.created.is_empty());
    }
    for domain in ["slack.com", ".slack.com", "app.slack.com"] {
        let mut fake = Fake::new(vec![]);
        fake.cookies[0]["domain"] = json!(domain);
        assert!(check(&mut fake, ""));
    }
}
#[test]
fn auth_rejection_network_failure_and_invalid_cookie_preserve_previous_session() {
    for network in [false, true] {
        let mut fake = Fake::new(vec![]);
        fake.auth_ok = false;
        fake.auth_error = network;
        assert!(!check(&mut fake, ""));
    }
    let mut fake = Fake::new(vec![]);
    fake.cookies[0]["value"] = json!("xoxd-test\nSLACK_XOXC=bad");
    assert!(!check(&mut fake, ""));
}
#[test]
fn reuses_matching_tab_and_never_navigates_or_closes_unrelated_pages() {
    let mut fake = Fake::new(vec![
        tab("unrelated", "https://example.invalid/editor"),
        tab("existing", "https://app.slack.com/client/TEXISTING/channel"),
    ]);
    assert!(check(&mut fake, ""));
    assert_eq!(fake.evaluated, ["existing"]);
    assert!(fake.created.is_empty() && fake.closed.is_empty());
    assert_eq!(fake.tabs[0]["url"], "https://example.invalid/editor");
}
#[test]
fn creates_and_closes_only_owned_background_tab() {
    let mut fake = Fake::new(vec![tab("unrelated", "https://example.invalid/editor")]);
    assert!(check(&mut fake, ""));
    assert_eq!(fake.created, ["https://app.slack.com/client"]);
    assert_eq!(fake.closed, ["owned"]);
    assert_eq!(fake.tabs.len(), 1);
    assert_eq!(fake.tabs[0]["url"], "https://example.invalid/editor");
}
#[test]
fn owned_tab_closes_after_extraction_or_discovery_failure() {
    for discovery in [false, true] {
        let mut fake = Fake::new(vec![tab("unrelated", "https://example.invalid/editor")]);
        fake.publish = !discovery;
        fake.evaluate_error = !discovery;
        assert!(!check(&mut fake, ""));
        assert_eq!(fake.closed, ["owned"]);
        assert_eq!(fake.tabs.len(), 1);
    }
}
#[test]
fn existing_tab_is_not_closed_on_failure_and_workspace_selects_its_page() {
    let mut fake = Fake::new(vec![tab(
        "existing",
        "https://app.slack.com/client/TEXISTING",
    )]);
    fake.evaluate_error = true;
    assert!(!check(&mut fake, ""));
    assert!(fake.closed.is_empty());
    let mut fake = Fake::new(vec![
        tab("other", "https://app.slack.com/client/TOTHER"),
        tab("expected", "https://app.slack.com/client/TEXPECTED/channel"),
    ]);
    assert!(check(&mut fake, "TEXPECTED"));
    assert_eq!(fake.evaluated, ["expected"]);
    assert!(fake.created.is_empty() && fake.closed.is_empty());
}
#[test]
fn failed_atomic_publication_cleans_temporary_file() {
    let root = tempfile::tempdir().unwrap();
    let target = root.path().join("tokens.env");
    fs::create_dir(&target).unwrap();
    assert!(persist(&target, TOKEN, COOKIE).is_err());
    assert!(target.is_dir());
    assert_eq!(fs::read_dir(root.path()).unwrap().count(), 1);
}
#[test]
fn sops_receives_literal_credentials_over_stdin_not_argv() {
    let root = tempfile::tempdir().unwrap();
    fs::create_dir(root.path().join("secrets")).unwrap();
    let path = root.path().join("tokens.env");
    persist(&path, TOKEN, COOKIE).unwrap();
    let script = root.path().join("sops");
    fs::write(
        &script,
        "#!/bin/sh\nprintf '%s\\n' \"$@\" >> args\ncat >> values\nprintf '\\n' >> values\n",
    )
    .unwrap();
    fs::set_permissions(&script, fs::Permissions::from_mode(0o700)).unwrap();
    sops_update(&path, root.path(), &script).unwrap();
    let args = fs::read_to_string(root.path().join("args")).unwrap();
    assert!(args.contains("--value-stdin"));
    assert!(!args.contains(TOKEN) && !args.contains(COOKIE));
    let values = fs::read_to_string(root.path().join("values")).unwrap();
    let values: Vec<String> = values
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect();
    assert_eq!(values, [COOKIE, TOKEN]);
}
