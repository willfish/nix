use crate::config::{Config, loopback_uri};
use hound::{SampleFormat, WavReader};
use http_body_util::{BodyExt, Full, Limited};
use hyper::{Request, body::Bytes, client::conn::http1};
use hyper_util::rt::TokioIo;
use serde_json::{Value, json};
use std::{collections::BTreeSet, io::Cursor, process::Stdio, time::Duration};
use tokio::{
    net::TcpStream,
    process::Command,
    time::{Instant, sleep, timeout},
};

pub const LIMIT: usize = 16 * 1024 * 1024;
const OUTPUT_LIMIT: usize = 32 * 1024 * 1024;

pub struct ApiError(pub u16, pub &'static str);
pub type ApiResult = Result<(&'static str, Vec<u8>), ApiError>;
pub type Query = Vec<(String, String)>;

pub struct Backend {
    pub config: Config,
    pub last_used: Instant,
    started: BTreeSet<String>,
}

impl Backend {
    pub fn new(config: Config) -> Self {
        Self {
            started: config.engines.values().map(|e| e.unit.clone()).collect(),
            config,
            last_used: Instant::now(),
        }
    }

    async fn systemctl(&self, command: &str, unit: &str) -> Result<(), ()> {
        let mut child = Command::new(&self.config.systemctl[0])
            .args(&self.config.systemctl[1..])
            .args([command, unit])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .kill_on_drop(true)
            .spawn()
            .map_err(|_| ())?;
        match timeout(Duration::from_secs(30), child.wait()).await {
            Ok(Ok(status)) if status.success() => Ok(()),
            _ => {
                let _ = child.kill().await;
                let _ = child.wait().await;
                Err(())
            }
        }
    }

    async fn ready(&mut self, engine: &str) -> Result<(), ApiError> {
        let Some(spec) = self.config.engines.get(engine) else {
            return Ok(());
        };
        self.systemctl("start", &spec.unit)
            .await
            .map_err(|_| ApiError(503, "Could not start the local inference engine"))?;
        self.started.insert(spec.unit.clone());
        let check = async {
            loop {
                if let Ok(data) =
                    request(&spec.health_url, None, "", 16384, Duration::from_secs(1)).await
                    && let Ok(data) = serde_json::from_slice::<Value>(&data)
                {
                    let ready = match engine {
                        "stt" => data.get("status").and_then(Value::as_str) == Some("ok"),
                        "tts" => data
                            .get("data")
                            .and_then(Value::as_array)
                            .is_some_and(|models| {
                                models.iter().any(|model| {
                                    model.get("id").and_then(Value::as_str)
                                        == Some(&self.config.tts_model)
                                        && model.get("loaded").and_then(Value::as_bool)
                                            == Some(true)
                                })
                            }),
                        _ => false,
                    };
                    if ready {
                        return;
                    }
                }
                sleep(Duration::from_millis(100)).await;
            }
        };
        timeout(
            Duration::from_secs_f64(self.config.readiness_timeout),
            check,
        )
        .await
        .map_err(|_| ApiError(503, "Local inference engine is not ready"))
    }

    pub async fn stop_engines(&mut self) {
        for unit in self.started.clone() {
            if self.systemctl("stop", &unit).await.is_ok() {
                self.started.remove(&unit);
            }
        }
    }

    pub async fn listen(&mut self, body: Vec<u8>, query: &Query) -> ApiResult {
        if !valid_wav(&body, false) {
            return Err(ApiError(400, "Expected mono PCM16 WAV audio"));
        }
        let mut random = [0u8; 16];
        getrandom::fill(&mut random)
            .map_err(|_| ApiError(500, "Could not prepare inference request"))?;
        let boundary: String = random.iter().map(|b| format!("{b:02x}")).collect();
        let language = first(query, "language", "en");
        let prompt = query
            .iter()
            .filter(|(k, _)| k == "keyterm")
            .map(|(_, v)| v.as_str())
            .collect::<Vec<_>>()
            .join(", ");
        let mut form = Vec::new();
        for (key, value) in [
            ("response_format", "json"),
            ("temperature", "0"),
            ("language", language),
            ("prompt", &prompt),
        ] {
            form.extend_from_slice(format!("--{boundary}\r\nContent-Disposition: form-data; name=\"{key}\"\r\n\r\n{value}\r\n").as_bytes());
        }
        form.extend_from_slice(format!("--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"dictation.wav\"\r\nContent-Type: audio/wav\r\n\r\n").as_bytes());
        form.extend_from_slice(&body);
        form.extend_from_slice(format!("\r\n--{boundary}--\r\n").as_bytes());
        self.ready("stt").await?;
        let result = request(
            &self.config.stt_url,
            Some(form),
            &format!("multipart/form-data; boundary={boundary}"),
            OUTPUT_LIMIT,
            Duration::from_secs(85),
        )
        .await?;
        let result: Value = serde_json::from_slice(&result)
            .map_err(|_| ApiError(502, "INVALID_UPSTREAM_RESPONSE"))?;
        let text = result
            .get("text")
            .and_then(Value::as_str)
            .ok_or(ApiError(502, "Inference engine returned no transcript"))?;
        Ok((
            "application/json",
            json!({"results": {"channels": [{"alternatives": [{"transcript": text}]}]}})
                .to_string()
                .into_bytes(),
        ))
    }

    pub async fn speak(
        &mut self,
        body: Vec<u8>,
        query: &Query,
        context: Option<&str>,
    ) -> ApiResult {
        for (key, expected, message) in [
            (
                "encoding",
                "linear16",
                "Only encoding=linear16 is supported",
            ),
            ("container", "wav", "Only container=wav is supported"),
            (
                "sample_rate",
                "24000",
                "Only sample_rate=24000 is supported",
            ),
        ] {
            let values: Vec<_> = query
                .iter()
                .filter(|(k, _)| k == key)
                .map(|(_, v)| v.as_str())
                .collect();
            if !values.is_empty() && values != [expected] {
                return Err(ApiError(400, message));
            }
        }
        let payload: Value = serde_json::from_slice(&body)
            .map_err(|_| ApiError(400, "Expected JSON containing text"))?;
        let text = payload
            .get("text")
            .and_then(Value::as_str)
            .filter(|s| !s.trim().is_empty())
            .ok_or(ApiError(400, "Expected nonempty text"))?;
        let model = first(query, "model", "samantha");
        if !self.config.voices.contains_key(model) {
            return Err(ApiError(400, "Unknown local voice model"));
        }
        let mut words = text.split_whitespace().count();
        if let Some(context) = context {
            let count = context
                .trim()
                .parse::<usize>()
                .ok()
                .filter(|&n| n <= 1_000_000)
                .ok_or(ApiError(400, "Invalid reply word count"))?;
            words = words.max(count);
        }
        let long = format!("{model}-long");
        let model = if words > 50 && self.config.voices.contains_key(&long) {
            &long
        } else {
            model
        };
        let mut payload = self.config.voices[model].clone();
        payload.insert("model".into(), self.config.tts_model.clone().into());
        payload.insert("input".into(), text.into());
        payload.insert("language".into(), "English".into());
        self.ready("tts").await?;
        let result = request(
            &self.config.tts_url,
            Some(Value::Object(payload).to_string().into_bytes()),
            "application/json",
            OUTPUT_LIMIT,
            Duration::from_secs(85),
        )
        .await?;
        if !valid_wav(&result, true) {
            return Err(ApiError(502, "Inference engine returned invalid WAV"));
        }
        Ok(("audio/wav", result))
    }
}

fn first<'a>(query: &'a Query, key: &str, default: &'a str) -> &'a str {
    query
        .iter()
        .find(|(k, _)| k == key)
        .map(|(_, v)| v.as_str())
        .unwrap_or(default)
}

fn valid_wav(bytes: &[u8], synthesis: bool) -> bool {
    let Ok(mut reader) = WavReader::new(Cursor::new(bytes)) else {
        return false;
    };
    let spec = reader.spec();
    if spec.sample_format != SampleFormat::Int
        || spec.bits_per_sample != 16
        || spec.channels == 0
        || (!synthesis && spec.channels != 1)
        || (synthesis && spec.sample_rate != 24000)
    {
        return false;
    }
    // Decode to EOF to reject truncated data, including the last partial frame.
    reader.samples::<i16>().all(|sample| sample.is_ok())
}

// Direct TCP to a validated numeric loopback address. This deliberately has no
// TLS, DNS, redirect following, proxy environment, cookies or auth forwarding.
async fn request(
    url: &str,
    body: Option<Vec<u8>>,
    content_type: &str,
    limit: usize,
    budget: Duration,
) -> Result<Vec<u8>, ApiError> {
    let unavailable = || ApiError(502, "Local inference engine unavailable or request failed");
    let uri = loopback_uri(url).map_err(|_| unavailable())?;
    let result = timeout(budget, async {
        let stream =
            TcpStream::connect((std::net::Ipv4Addr::LOCALHOST, uri.port_u16().unwrap_or(80)))
                .await
                .map_err(|_| unavailable())?;
        let (mut sender, connection) = http1::handshake(TokioIo::new(stream))
            .await
            .map_err(|_| unavailable())?;
        let connection = tokio::spawn(async move {
            let _ = connection.await;
        });
        // Abort the driver if a request times out or gets cancelled as well.
        struct Driver(tokio::task::JoinHandle<()>);
        impl Drop for Driver {
            fn drop(&mut self) {
                self.0.abort();
            }
        }
        let _driver = Driver(connection);
        let method = if body.is_some() { "POST" } else { "GET" };
        let bytes = body.unwrap_or_default();
        let request = Request::builder()
            .method(method)
            .uri(uri.path_and_query().map(|p| p.as_str()).unwrap_or("/"))
            .header("Host", uri.authority().ok_or_else(unavailable)?.as_str())
            .header("Content-Type", content_type)
            .header("Content-Length", bytes.len())
            .header("Connection", "close")
            .body(Full::new(Bytes::from(bytes)))
            .map_err(|_| unavailable())?;
        let reply = sender
            .send_request(request)
            .await
            .map_err(|_| unavailable())?;
        if !reply.status().is_success() {
            return Err(unavailable());
        }
        let body = Limited::new(reply.into_body(), limit)
            .collect()
            .await
            .map_err(|_| ApiError(502, "Invalid or oversized inference response"))?;
        Ok(body.to_bytes().to_vec())
    })
    .await;
    result.map_err(|_| unavailable())?
}
