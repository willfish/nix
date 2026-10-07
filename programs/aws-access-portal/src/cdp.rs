use crate::{Error, Result};
use serde_json::{Value, json};
use std::{net::TcpStream, time::Duration};
use tungstenite::{Message, WebSocket, protocol::WebSocketConfig};
use url::Url;

pub(crate) fn focus_allowed(method: &str) -> Result<()> {
    if matches!(method, "Target.activateTarget" | "Page.bringToFront") {
        Err(Error::new(format!(
            "refusing to focus the browser with {method}"
        )))
    } else {
        Ok(())
    }
}
pub(crate) struct CdpSocket {
    socket: WebSocket<TcpStream>,
    id: u64,
}
impl CdpSocket {
    #[allow(dead_code)] // Slack uses the larger, explicit message limit.
    pub(crate) fn new(endpoint: &str, url: &str) -> Result<Self> {
        Self::with_limit(endpoint, url, 8_000_000)
    }
    pub(crate) fn with_limit(endpoint: &str, url: &str, limit: usize) -> Result<Self> {
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
            .max_message_size(Some(limit))
            .max_frame_size(Some(limit));
        let (socket, _) = tungstenite::client::client_with_config(url, stream, Some(config))
            .map_err(|_| Error::new("Brave debugger handshake failed"))?;
        Ok(Self { socket, id: 0 })
    }
    #[allow(dead_code)] // Used by Slack's bounded temporary-tab cleanup.
    pub(crate) fn timeout(&mut self, duration: Duration) -> Result<()> {
        self.socket
            .get_mut()
            .set_read_timeout(Some(duration))
            .and_then(|_| self.socket.get_mut().set_write_timeout(Some(duration)))
            .map_err(|_| Error::new("could not set CDP timeout"))
    }
    pub(crate) fn call(&mut self, method: &str, params: Value) -> Result<Value> {
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
