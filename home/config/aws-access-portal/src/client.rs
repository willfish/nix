use crate::portal::Backend;
use crate::{Error, Result};
use reqwest::blocking::Client as Http;
use serde_json::{Value, json};
use std::{
    net::TcpStream,
    thread,
    time::{Duration, Instant},
};
use tungstenite::{Message, WebSocket, protocol::WebSocketConfig};
use url::Url;

const CDP: &str = "http://127.0.0.1:9222";
const API: &str = "https://portal.sso.eu-west-2.amazonaws.com";
pub(crate) fn focus_allowed(method: &str) -> Result<()> {
    if matches!(method, "Target.activateTarget" | "Page.bringToFront") {
        Err(Error::new(format!(
            "refusing to focus the browser with {method}"
        )))
    } else {
        Ok(())
    }
}
struct CdpSocket {
    socket: WebSocket<TcpStream>,
    id: u64,
}
impl CdpSocket {
    fn new(endpoint: &str, url: &str) -> Result<Self> {
        let base = Url::parse(endpoint).map_err(|_| Error::new("invalid CDP endpoint"))?;
        let target = Url::parse(url)
            .map_err(|_| Error::new("Brave CDP returned an invalid debugger URL"))?;
        if target.scheme() != "ws"
            || target.host() != base.host()
            || target.port_or_known_default() != base.port_or_known_default()
            || !target.username().is_empty()
            || target.password().is_some()
        {
            return Err(Error::new(
                "Brave debugger URL did not match the configured local endpoint",
            ));
        }
        let addresses = target
            .socket_addrs(|| None)
            .map_err(|_| Error::new("could not resolve the local Brave debugger"))?;
        let mut connected = None;
        for addr in addresses {
            if let Ok(stream) = TcpStream::connect_timeout(&addr, Duration::from_secs(10)) {
                connected = Some(stream);
                break;
            }
        }
        let stream =
            connected.ok_or_else(|| Error::new("could not connect to the Brave debugger"))?;
        stream
            .set_read_timeout(Some(Duration::from_secs(30)))
            .and_then(|_| stream.set_write_timeout(Some(Duration::from_secs(30))))
            .map_err(|_| Error::new("could not configure the Brave debugger connection"))?;
        let config = WebSocketConfig::default()
            .max_message_size(Some(8_000_000))
            .max_frame_size(Some(8_000_000));
        let (socket, _) = tungstenite::client::client_with_config(url, stream, Some(config))
            .map_err(|_| Error::new("Brave debugger handshake failed"))?;
        Ok(Self { socket, id: 0 })
    }
    fn call(&mut self, method: &str, params: Value) -> Result<Value> {
        focus_allowed(method)?;
        self.id += 1;
        let mut message = json!({"id":self.id,"method":method});
        if params.as_object().is_some_and(|p| !p.is_empty()) {
            message["params"] = params;
        }
        self.socket
            .send(Message::Text(message.to_string().into()))
            .map_err(|_| Error::new(format!("CDP {method} failed")))?;
        loop {
            let message = self
                .socket
                .read()
                .map_err(|_| Error::new(format!("CDP {method} failed")))?;
            let bytes = match message {
                Message::Text(s) => s.as_bytes().to_vec(),
                Message::Binary(b) => b.to_vec(),
                Message::Close(_) => return Err(Error::new(format!("CDP {method} closed"))),
                _ => continue,
            };
            let value: Value = serde_json::from_slice(&bytes)
                .map_err(|_| Error::new("Brave debugger returned invalid JSON"))?;
            if value["id"].as_u64() != Some(self.id) {
                continue;
            }
            if value.get("error").is_some() {
                return Err(Error::new(format!("CDP {method} failed")));
            }
            return Ok(value.get("result").cloned().unwrap_or_else(|| json!({})));
        }
    }
}
impl Drop for CdpSocket {
    fn drop(&mut self) {
        let _ = self.socket.close(None);
    }
}
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
