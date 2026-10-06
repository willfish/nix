use crate::portal::Backend;
use crate::{Error, Result};
use reqwest::blocking::Client as Http;
use serde_json::{Value, json};
use std::{
    thread,
    time::{Duration, Instant},
};
use url::Url;

const CDP: &str = "http://127.0.0.1:9222";
const API: &str = "https://portal.sso.eu-west-2.amazonaws.com";
use crate::cdp::CdpSocket;
#[cfg(test)]
pub(crate) use crate::cdp::focus_allowed;
struct Tab {
    endpoint: String,
    browser: Option<CdpSocket>,
    page: Option<CdpSocket>,
    target: Option<String>,
    http: Http,
}
impl Tab {
    fn new(endpoint: &str, browser_ws: &str, http: Http) -> Result<Self> {
        let browser = CdpSocket::new(endpoint, browser_ws)?;
        let mut tab = Self {
            endpoint: endpoint.into(),
            browser: Some(browser),
            page: None,
            target: None,
            http,
        };
        let created = tab.browser.as_mut().unwrap().call(
            "Target.createTarget",
            json!({"url":"about:blank","background":true}),
        )?;
        let target = created["targetId"]
            .as_str()
            .filter(|s| !s.is_empty())
            .ok_or_else(|| Error::new("could not create a background portal tab"))?
            .to_owned();
        tab.target = Some(target.clone());
        let base = browser_ws
            .rsplit_once("/devtools/browser/")
            .map(|p| p.0)
            .ok_or_else(|| Error::new("Brave returned an invalid browser debugger URL"))?;
        let mut page = CdpSocket::new(endpoint, &format!("{base}/devtools/page/{target}"))?;
        page.call("Runtime.enable", json!({}))?;
        tab.page = Some(page);
        Ok(tab)
    }
    fn call(&mut self, method: &str, params: Value) -> Result<Value> {
        self.page
            .as_mut()
            .ok_or_else(|| Error::new("portal tab is unavailable"))?
            .call(method, params)
    }
    fn evaluate(&mut self, expression: &str) -> Result<Value> {
        let response = self.call(
            "Runtime.evaluate",
            json!({"expression":expression,"returnByValue":true}),
        )?;
        let remote = &response["result"];
        if remote["subtype"] == "error" || response.get("exceptionDetails").is_some() {
            return Err(Error::new("portal page script failed"));
        }
        Ok(remote.get("value").cloned().unwrap_or(Value::Null))
    }
    fn close(&mut self) -> Result<()> {
        self.page.take();
        let target = self.target.take();
        let result = if let (Some(browser), Some(target)) = (&mut self.browser, &target) {
            browser
                .call("Target.closeTarget", json!({"targetId":target}))
                .map(|_| ())
        } else {
            Ok(())
        };
        self.browser.take();
        result?;
        if let Some(target) = target {
            let end = Instant::now() + Duration::from_secs(5);
            while Instant::now() < end {
                let tabs = self
                    .http
                    .get(format!("{}/json/list", self.endpoint))
                    .timeout(Duration::from_secs(2))
                    .send()
                    .ok()
                    .and_then(|response| response.json::<Value>().ok());
                if tabs
                    .as_ref()
                    .and_then(Value::as_array)
                    .is_some_and(|tabs| !tabs.iter().any(|tab| tab["id"] == target))
                {
                    return Ok(());
                }
                thread::sleep(Duration::from_millis(200));
            }
            return Err(Error::new("background portal tab was not closed"));
        }
        Ok(())
    }
}
impl Drop for Tab {
    fn drop(&mut self) {
        let _ = self.close();
    }
}
pub struct Client {
    cdp: String,
    api: String,
    http: Http,
    local: Http,
    tab: Option<Tab>,
}
impl Client {
    pub fn new() -> Result<Self> {
        Self::endpoints(CDP, API)
    }
    fn endpoints(cdp: &str, api: &str) -> Result<Self> {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let http = Http::builder()
            .redirect(reqwest::redirect::Policy::none())
            .timeout(Duration::from_secs(30))
            .build()
            .map_err(|_| Error::new("could not initialize portal HTTPS"))?;
        let local = Http::builder()
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .timeout(Duration::from_secs(5))
            .build()
            .map_err(|_| Error::new("could not initialize local CDP"))?;
        Ok(Self {
            cdp: cdp.into(),
            api: api.into(),
            http,
            local,
            tab: None,
        })
    }
    #[cfg(test)]
    pub fn for_test(cdp: &str, api: &str) -> Result<Self> {
        Self::endpoints(cdp, api)
    }
    fn browser_ws(&self) -> Result<String> {
        let version: Value = self
            .local
            .get(format!("{}/json/version", self.cdp))
            .send()
            .and_then(|r| r.error_for_status())
            .and_then(|r| r.json())
            .map_err(|_| Error::new(format!("Brave CDP is unavailable at {}", self.cdp)))?;
        version["webSocketDebuggerUrl"]
            .as_str()
            .map(str::to_owned)
            .ok_or_else(|| Error::new("Brave CDP did not provide a browser debugger URL"))
    }
    fn tab(&mut self) -> Result<&mut Tab> {
        self.tab
            .as_mut()
            .ok_or_else(|| Error::new("portal tab is unavailable"))
    }
}
impl Backend for Client {
    fn token(&mut self) -> Result<Option<String>> {
        let url = self.browser_ws()?;
        let mut socket = CdpSocket::new(&self.cdp, &url)?;
        let body = socket.call("Storage.getCookies", json!({}))?;
        let Some(cookies) = body["cookies"].as_array() else {
            return Ok(None);
        };
        for cookie in cookies {
            let name = cookie["name"].as_str().unwrap_or("");
            if !matches!(name, "x-amz-sso_authn" | "__Host-idc-access-portal-authn") {
                continue;
            }
            let domain = cookie["domain"]
                .as_str()
                .unwrap_or("")
                .trim_start_matches('.');
            let allowed = ["awsapps.com", "amazonaws.com"]
                .iter()
                .any(|suffix| domain == *suffix || domain.ends_with(&format!(".{suffix}")));
            if allowed {
                if let Some(value) = cookie["value"].as_str().filter(|s| !s.is_empty()) {
                    return Ok(Some(value.into()));
                }
            }
        }
        Ok(None)
    }
    fn api(&mut self, token: &str, path: &str, query: &[(&str, String)]) -> Result<Value> {
        let mut url = Url::parse(&format!("{}{path}", self.api))
            .map_err(|_| Error::new("invalid portal endpoint"))?;
        if !query.is_empty() {
            url.query_pairs_mut()
                .extend_pairs(query.iter().map(|(k, v)| (*k, v.as_str())));
        }
        let response = self
            .http
            .get(url)
            .header("Authorization", format!("Bearer {token}"))
            .header("x-amz-sso-bearer-token", token)
            .header("x-amz-sso_bearer_token", token)
            .header("Accept", "application/json")
            .send()
            .map_err(|_| Error::new(format!("portal {path} request failed")))?;
        if !response.status().is_success() {
            return Err(Error::new(format!(
                "portal {path} returned HTTP {}",
                response.status().as_u16()
            )));
        }
        response
            .json()
            .map_err(|_| Error::new(format!("portal {path} request failed")))
    }
    fn open(&mut self) -> Result<()> {
        let url = self.browser_ws()?;
        self.tab = Some(Tab::new(&self.cdp, &url, self.local.clone())?);
        Ok(())
    }
    fn navigate(&mut self, url: &str) -> Result<()> {
        self.tab()?
            .call("Page.navigate", json!({"url":url}))
            .map(|_| ())
    }
    fn state(&mut self) -> Result<Value> {
        let value = self.tab()?.evaluate(include_str!("page-state.js"))?;
        if !value.is_object() {
            return Err(Error::new("could not read the portal page"));
        }
        Ok(value)
    }
    fn fill(&mut self, field: &str, button: &str, value: &str) -> Result<bool> {
        let expression = format!(
            "{}({}, {}, {})",
            include_str!("fill.js").trim(),
            json!(field),
            json!(button),
            json!(value)
        );
        Ok(self.tab()?.evaluate(&expression)? == Value::Bool(true))
    }
    fn close(&mut self) -> Result<()> {
        match self.tab.take() {
            Some(mut tab) => tab.close(),
            None => Ok(()),
        }
    }
}
