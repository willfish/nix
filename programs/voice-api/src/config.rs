use hyper::Uri;
use serde::Deserialize;
use serde_json::{Map, Value};
use std::{collections::BTreeMap, time::Duration};

#[derive(Deserialize)]
pub struct Engine {
    pub unit: String,
    pub health_url: String,
}

#[derive(Deserialize)]
#[serde(default)]
pub struct Config {
    pub port: u16,
    pub stt_url: String,
    pub tts_url: String,
    pub systemctl: Vec<String>,
    pub engines: BTreeMap<String, Engine>,
    pub voices: BTreeMap<String, Map<String, Value>>,
    pub tts_model: String,
    pub readiness_timeout: f64,
    pub idle_timeout: f64,
    pub queue_timeout: f64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            port: 8180,
            stt_url: String::new(),
            tts_url: String::new(),
            systemctl: vec!["systemctl".into(), "--user".into()],
            engines: BTreeMap::new(),
            voices: BTreeMap::from([("samantha".into(), Map::new())]),
            tts_model: "pi-voice".into(),
            readiness_timeout: 60.0,
            idle_timeout: 120.0,
            queue_timeout: 5.0,
        }
    }
}

// No DNS, credentials, HTTPS, proxy configuration, or alternative loopback
// spellings. Health probes must have the same boundary as inference requests.
pub fn loopback_uri(url: &str) -> Result<Uri, ()> {
    let uri: Uri = url.parse().map_err(|_| ())?;
    let authority = uri.authority().ok_or(())?;
    if uri.scheme_str() != Some("http") || uri.host() != Some("127.0.0.1") {
        return Err(());
    }
    let authority_ok = authority.as_str() == "127.0.0.1"
        || authority
            .as_str()
            .strip_prefix("127.0.0.1:")
            .is_some_and(|port| {
                !port.is_empty()
                    && port.bytes().all(|c| c.is_ascii_digit())
                    && port.parse::<u16>().is_ok_and(|port| port != 0)
            });
    if !authority_ok || url.contains('#') {
        return Err(());
    }
    Ok(uri)
}

impl Config {
    pub fn validate(&self) -> Result<(), ()> {
        loopback_uri(&self.stt_url)?;
        loopback_uri(&self.tts_url)?;
        for engine in self.engines.values() {
            loopback_uri(&engine.health_url)?;
            if engine.unit.is_empty() || engine.unit.starts_with('-') {
                return Err(());
            }
        }
        if self.systemctl.is_empty() || self.systemctl[0].is_empty() {
            return Err(());
        }
        for value in [
            self.readiness_timeout,
            self.idle_timeout,
            self.queue_timeout,
        ] {
            Duration::try_from_secs_f64(value).map_err(|_| ())?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_only_numeric_loopback_http() {
        for url in [
            "http://127.0.0.1/inference",
            "http://127.0.0.1:8179/v1/models",
        ] {
            assert!(loopback_uri(url).is_ok());
        }
        for url in [
            "https://127.0.0.1/",
            "http://localhost/",
            "http://127.1/",
            "http://127.0.0.1.example.com/",
            "http://[::1]/",
            "file:///etc/passwd",
            "http://user@127.0.0.1/",
            "http://127.0.0.1@other/",
            "http://127.0.0.1/#fragment",
            "http://127.0.0.1:/",
            "http://127.0.0.1:65536/",
            "http://127.0.0.1:0/",
        ] {
            assert!(loopback_uri(url).is_err(), "accepted {url}");
        }
    }

    #[test]
    fn rejects_invalid_durations_before_starting_tasks() {
        for value in [-1.0, f64::NAN, f64::INFINITY, f64::MAX] {
            let config = Config {
                stt_url: "http://127.0.0.1/stt".into(),
                tts_url: "http://127.0.0.1/tts".into(),
                queue_timeout: value,
                ..Config::default()
            };
            assert!(config.validate().is_err());
        }
    }
}
