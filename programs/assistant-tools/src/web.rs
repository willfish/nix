use crate::{MAX_FILE, MAX_OUTPUT, Result, truncate};
use reqwest::{Certificate, Client};
use scraper::{Html, Node, Selector};
use serde_json::{Value, json};
use std::{
    future::Future,
    net::{IpAddr, SocketAddr},
    path::Path,
    process::Stdio,
    time::Duration,
};
use tokio::{net::lookup_host, process::Command, time::timeout};
use url::{Host, Url};

fn subnet(ip: u128, prefix: u128, bits: u32, width: u32) -> bool {
    ip >> (width - bits) == prefix >> (width - bits)
}

pub fn public_address(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => {
            let n = u32::from(ip) as u128;
            if n == 0xc0000009 || n == 0xc000000a {
                return true;
            }
            ![
                (0, 8),
                (0x0a000000, 8),
                (0x64400000, 10),
                (0x7f000000, 8),
                (0xa9fe0000, 16),
                (0xac100000, 12),
                (0xc0000000, 24),
                (0xc0000200, 24),
                (0xc0a80000, 16),
                (0xc6120000, 15),
                (0xc6336400, 24),
                (0xcb007100, 24),
                (0xe0000000, 4),
                (0xf0000000, 4),
            ]
            .iter()
            .any(|&(prefix, bits)| subnet(n, prefix, bits, 32))
        }
        IpAddr::V6(ip) => {
            if let Some(ip) = ip.to_ipv4_mapped() {
                return public_address(ip.into());
            }
            let n = u128::from(ip);
            let inside = |prefix: &str, bits| {
                subnet(
                    n,
                    u128::from(prefix.parse::<std::net::Ipv6Addr>().unwrap()),
                    bits,
                    128,
                )
            };
            if inside("2001::", 23) {
                return ["2001:1::1", "2001:1::2"]
                    .iter()
                    .any(|s| ip.to_string() == *s)
                    || [
                        ("2001:3::", 32),
                        ("2001:4:112::", 48),
                        ("2001:20::", 28),
                        ("2001:30::", 28),
                    ]
                    .iter()
                    .any(|&(prefix, bits)| inside(prefix, bits));
            }
            ![
                ("::", 128),
                ("::1", 128),
                ("64:ff9b:1::", 48),
                ("100::", 64),
                ("2001:db8::", 32),
                ("2002::", 16),
                ("3fff::", 20),
                ("fc00::", 7),
                ("fe80::", 10),
                ("ff00::", 8),
            ]
            .iter()
            .any(|&(prefix, bits)| inside(prefix, bits))
        }
    }
}

pub fn parse_url(url: &str) -> Result<Url> {
    let parsed = Url::parse(url).map_err(|_| "Only public HTTP(S) URLs are supported")?;
    if !matches!(parsed.scheme(), "http" | "https")
        || parsed.host().is_none()
        || !parsed.username().is_empty()
        || parsed.password().is_some()
    {
        return Err("Only public HTTP(S) URLs are supported");
    }
    Ok(parsed)
}

struct Target {
    url: Url,
    address: SocketAddr,
}
struct Reply {
    status: u16,
    location: Option<String>,
    body: Vec<u8>,
}

async fn resolve(url: Url) -> Result<Vec<SocketAddr>> {
    let port = url.port_or_known_default().ok_or("Unsupported web port")?;
    match url.host().ok_or("Missing web host")? {
        Host::Ipv4(ip) => Ok(vec![SocketAddr::new(ip.into(), port)]),
        Host::Ipv6(ip) => Ok(vec![SocketAddr::new(ip.into(), port)]),
        Host::Domain(host) => Ok(timeout(Duration::from_secs(20), lookup_host((host, port)))
            .await
            .map_err(|_| "Web hostname lookup timed out")?
            .map_err(|_| "Could not resolve web hostname")?
            .collect()),
    }
}

fn ca_bundle(
    builder: reqwest::ClientBuilder,
    path: Option<&Path>,
) -> Result<reqwest::ClientBuilder> {
    let Some(path) = path else {
        return Ok(builder);
    };
    let bytes = std::fs::read(path).map_err(|_| "Could not load web certificate bundle")?;
    let certificates =
        Certificate::from_pem_bundle(&bytes).map_err(|_| "Invalid web certificate bundle")?;
    if certificates.is_empty() {
        return Err("Invalid web certificate bundle");
    }
    Ok(builder.tls_certs_only(certificates))
}

async fn request(target: Target, certificate: Option<Certificate>) -> Result<Reply> {
    static CRYPTO: std::sync::Once = std::sync::Once::new();
    CRYPTO.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
    // A fresh client per redirect prevents sharing TLS connections between
    // hostnames. Keep the URL hostname for Host and certificate/SNI validation;
    // the DNS override pins the connection to the already validated address.
    let mut builder = Client::builder()
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .pool_max_idle_per_host(0)
        .timeout(Duration::from_secs(20));
    // Use the declared Nix CA bundle on Darwin as well as Linux, rather than
    // silently switching certificate trust to the host's keychain.
    let bundle = std::env::var_os("SSL_CERT_FILE").map(std::path::PathBuf::from);
    builder = ca_bundle(builder, bundle.as_deref())?;
    if let Some(host) = target.url.domain() {
        builder = builder.resolve(host, target.address);
    }
    if let Some(certificate) = certificate {
        builder = builder.add_root_certificate(certificate);
    }
    let client = builder
        .build()
        .map_err(|_| "Could not prepare web request")?;
    let mut response = client
        .get(target.url)
        .header("User-Agent", "LocalAssistant/1.0")
        .header("Connection", "close")
        .send()
        .await
        .map_err(|_| "Could not fetch web page")?;
    let status = response.status().as_u16();
    let location = response
        .headers()
        .get("location")
        .and_then(|v| v.to_str().ok())
        .map(str::to_owned);
    if [301, 302, 303, 307, 308].contains(&status) {
        return Ok(Reply {
            status,
            location,
            body: Vec::new(),
        });
    }
    if !(200..300).contains(&status) {
        return Err("Web source returned an unsuccessful response");
    }
    let mut body = Vec::new();
    while let Some(chunk) = response
        .chunk()
        .await
        .map_err(|_| "Could not read web page")?
    {
        if body.len() + chunk.len() > 2 * MAX_FILE {
            return Err("Page exceeds the 2 MiB download limit");
        }
        body.extend_from_slice(&chunk);
    }
    Ok(Reply {
        status,
        location,
        body,
    })
}

fn readable(body: &[u8]) -> String {
    let prefix = Html::parse_document(&String::from_utf8_lossy(&body[..body.len().min(1024)]));
    let selector = Selector::parse("meta[charset], meta[http-equiv]").unwrap();
    let declared = prefix.select(&selector).find_map(|element| {
        let value = element.value();
        let label = value.attr("charset").or_else(|| {
            if !value
                .attr("http-equiv")
                .is_some_and(|v| v.eq_ignore_ascii_case("content-type"))
            {
                return None;
            }
            value.attr("content")?.split(';').find_map(|part| {
                let (name, value) = part.split_once('=')?;
                name.trim()
                    .eq_ignore_ascii_case("charset")
                    .then_some(value.trim().trim_matches(['\'', '"']))
            })
        })?;
        encoding_rs::Encoding::for_label(label.as_bytes())
    });
    let mut detector = chardetng::EncodingDetector::new();
    detector.feed(body, true);
    let encoding = encoding_rs::Encoding::for_bom(body)
        .map(|(encoding, _)| encoding)
        .or(declared)
        .unwrap_or_else(|| detector.guess(None, true));
    let (decoded, _, _) = encoding.decode(body);
    let document = Html::parse_document(&decoded);
    let mut pieces = Vec::new();
    for node in document.tree.root().descendants() {
        if let Node::Text(text) = node.value() {
            let hidden = node.ancestors().any(|ancestor| {
                matches!(ancestor.value(), Node::Element(element)
                if ["script", "style", "nav", "footer"].contains(&element.name()))
            });
            if !hidden && !text.trim().is_empty() {
                pieces.push(text.trim());
            }
        }
    }
    truncate(&pieces.join(" "), MAX_OUTPUT)
}

async fn fetch_with<R, RF, T, TF>(initial: &str, resolver: R, transport: T) -> Result<Value>
where
    R: Fn(Url) -> RF,
    RF: Future<Output = Result<Vec<SocketAddr>>>,
    T: Fn(Target) -> TF,
    TF: Future<Output = Result<Reply>>,
{
    let mut url = parse_url(initial)?;
    for _ in 0..5 {
        let addresses = resolver(url.clone()).await?;
        if addresses.is_empty()
            || addresses
                .iter()
                .any(|address| !public_address(address.ip()))
        {
            return Err("Local and private network addresses are not web sources");
        }
        let reply = transport(Target {
            url: url.clone(),
            address: addresses[0],
        })
        .await?;
        if [301, 302, 303, 307, 308].contains(&reply.status) {
            url = parse_url(
                url.join(reply.location.as_deref().ok_or("Invalid web redirect")?)
                    .map_err(|_| "Invalid web redirect")?
                    .as_str(),
            )?;
            continue;
        }
        return Ok(json!({"url": url.as_str(), "text": readable(&reply.body)}));
    }
    Err("Too many redirects")
}

pub async fn fetch_page(url: &str) -> Result<Value> {
    fetch_with(url, resolve, |target| request(target, None)).await
}

pub async fn search(provider: &Path, query: &str, max_results: i64) -> Result<Value> {
    // DDGS is an unchanged third-party metasearch engine, not repository-owned
    // Python. Use its CLI rather than reimplementing or narrowing its providers.
    let dir = tempfile::tempdir().map_err(|_| "Could not prepare search")?;
    let output = dir.path().join("results.json");
    let mut child = Command::new(provider)
        .args(["text", "--query", query, "--max_results"])
        .arg(max_results.clamp(1, 8).to_string())
        .arg("--output")
        .arg(&output)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .kill_on_drop(true)
        .spawn()
        .map_err(|_| "Could not start search provider")?;
    // DDGS owns the per-provider timeout and fallback sequence. Do not impose
    // a shorter whole-search timeout that would cut off later providers.
    let status = child.wait().await.map_err(|_| "Search provider failed")?;
    if !status.success() {
        return Err("Search provider failed");
    }
    let file = std::fs::File::open(output).map_err(|_| "Search provider returned no results")?;
    let mut bytes = Vec::new();
    std::io::Read::read_to_end(
        &mut std::io::Read::take(file, (2 * MAX_FILE + 1) as u64),
        &mut bytes,
    )
    .map_err(|_| "Could not read search results")?;
    if bytes.len() > 2 * MAX_FILE {
        return Err("Search results exceed the download limit");
    }
    let mut results: Vec<Value> =
        serde_json::from_slice(&bytes).map_err(|_| "Search provider returned invalid results")?;
    results.truncate(max_results.clamp(1, 8) as usize);
    Ok(results.into())
}

#[cfg(test)]
mod tests;
