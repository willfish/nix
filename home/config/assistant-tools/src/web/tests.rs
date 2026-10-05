use super::*;
use std::{
    collections::VecDeque,
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWriteExt},
    net::TcpListener,
};

#[test]
fn rejects_credentials_non_http_urls_and_every_local_address_family() {
    for url in [
        "file:///etc/passwd",
        "ftp://example.test",
        "http://user:password@example.test",
        "http://:password@example.test",
    ] {
        assert!(parse_url(url).is_err());
    }
    for ip in [
        "0.0.0.0",
        "10.0.0.1",
        "100.94.75.75",
        "127.0.0.1",
        "169.254.1.1",
        "172.16.1.1",
        "192.168.178.1",
        "192.0.2.1",
        "198.18.0.1",
        "203.0.113.1",
        "224.0.0.1",
        "::",
        "::1",
        "::ffff:127.0.0.1",
        "fc00::1",
        "fe80::1",
        "2001:db8::1",
        "2002::1",
    ] {
        assert!(!public_address(ip.parse().unwrap()), "{ip}");
    }
    for ip in [
        "93.184.216.34",
        "1.1.1.1",
        "8.8.8.8",
        "192.0.0.9",
        "2606:4700:4700::1111",
        "2001:4860:4860::8888",
    ] {
        assert!(public_address(ip.parse().unwrap()), "{ip}");
    }
}

#[tokio::test]
async fn local_urls_and_mixed_dns_answers_never_reach_transport() {
    for url in ["http://127.0.0.1", "http://192.168.178.1", "http://[::1]"] {
        assert!(fetch_page(url).await.is_err());
    }
    let calls = AtomicUsize::new(0);
    let result = fetch_with(
        "http://example.test",
        |_| async {
            Ok(vec![
                "93.184.216.34:80".parse().unwrap(),
                "10.0.0.1:80".parse().unwrap(),
            ])
        },
        |_| async {
            calls.fetch_add(1, Ordering::SeqCst);
            Err("must not connect")
        },
    )
    .await;
    assert_eq!(
        result.unwrap_err(),
        "Local and private network addresses are not web sources"
    );
    assert_eq!(calls.load(Ordering::SeqCst), 0);
}

#[tokio::test]
async fn pins_each_redirect_and_rejects_dns_rebinding_before_a_second_connection() {
    let addresses = Mutex::new(VecDeque::from(["93.184.216.34:80", "127.0.0.1:80"]));
    let calls = Mutex::new(Vec::new());
    let result = fetch_with(
        "http://example.test/page",
        |_| {
            let address = addresses
                .lock()
                .unwrap()
                .pop_front()
                .unwrap()
                .parse()
                .unwrap();
            async move { Ok(vec![address]) }
        },
        |target| {
            calls
                .lock()
                .unwrap()
                .push((target.url.to_string(), target.address.to_string()));
            async {
                Ok(Reply {
                    status: 302,
                    location: Some("/private".into()),
                    body: vec![],
                })
            }
        },
    )
    .await;
    assert_eq!(
        result.unwrap_err(),
        "Local and private network addresses are not web sources"
    );
    assert_eq!(
        *calls.lock().unwrap(),
        vec![("http://example.test/page".into(), "93.184.216.34:80".into())]
    );
}

#[tokio::test]
async fn redirects_validate_and_pin_their_own_public_destination() {
    let addresses = Mutex::new(VecDeque::from(["93.184.216.34:80", "1.1.1.1:80"]));
    let calls = Mutex::new(Vec::new());
    let result = fetch_with(
        "http://first.test/page",
        |_| {
            let address = addresses
                .lock()
                .unwrap()
                .pop_front()
                .unwrap()
                .parse()
                .unwrap();
            async move { Ok(vec![address]) }
        },
        |target| {
            calls
                .lock()
                .unwrap()
                .push((target.url.to_string(), target.address.to_string()));
            let status = if target.url.host_str() == Some("first.test") {
                302
            } else {
                200
            };
            async move {
                Ok(Reply {
                    status,
                    location: Some("http://second.test/final".into()),
                    body: b"<p>Safe</p>".to_vec(),
                })
            }
        },
    )
    .await
    .unwrap();
    assert_eq!(
        result,
        json!({"url": "http://second.test/final", "text": "Safe"})
    );
    assert_eq!(
        calls.lock().unwrap()[1],
        ("http://second.test/final".into(), "1.1.1.1:80".into())
    );
}

#[tokio::test]
async fn redirect_limits_and_non_http_redirects_fail_explicitly() {
    let calls = AtomicUsize::new(0);
    let result = fetch_with(
        "http://example.test",
        |_| async { Ok(vec!["1.1.1.1:80".parse().unwrap()]) },
        |_| {
            calls.fetch_add(1, Ordering::SeqCst);
            async {
                Ok(Reply {
                    status: 302,
                    location: Some("/again".into()),
                    body: vec![],
                })
            }
        },
    )
    .await;
    assert_eq!(result.unwrap_err(), "Too many redirects");
    assert_eq!(calls.load(Ordering::SeqCst), 5);
    assert!(
        fetch_with(
            "http://example.test",
            |_| async { Ok(vec!["1.1.1.1:80".parse().unwrap()]) },
            |_| async {
                Ok(Reply {
                    status: 302,
                    location: Some("file:///etc/passwd".into()),
                    body: vec![],
                })
            }
        )
        .await
        .is_err()
    );
}

#[test]
fn certificate_bundle_configuration_fails_closed_on_missing_or_empty_files() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("bundle");
    assert!(ca_bundle(Client::builder(), Some(&path)).is_err());
    std::fs::write(&path, "not a certificate").unwrap();
    assert!(ca_bundle(Client::builder(), Some(&path)).is_err());
    let certified = rcgen::generate_simple_self_signed(vec!["fixture.test".into()]).unwrap();
    std::fs::write(&path, certified.cert.pem()).unwrap();
    assert!(ca_bundle(Client::builder(), Some(&path)).is_ok());
}

#[test]
fn readable_html_removes_noise_and_bounds_unicode_output() {
    assert_eq!(
        readable(b"<meta charset=windows-1252><p>caf\xe9</p>"),
        "café"
    );
    assert_eq!(readable(b"\xef\xbb\xbf<p>UTF-8 text</p>"), "UTF-8 text");
    assert_eq!(readable(b"<html><head><style>hidden</style></head><body><nav>hidden</nav><p>Useful &amp; clear <b>text</b></p><script>hidden</script><footer>hidden</footer></body></html>"), "Useful & clear text");
    assert_eq!(
        readable("💡".repeat(MAX_OUTPUT + 1).as_bytes())
            .chars()
            .count(),
        MAX_OUTPUT
    );
}

async fn head<R: AsyncRead + Unpin>(stream: &mut R) -> String {
    let mut bytes = Vec::new();
    while !bytes.ends_with(b"\r\n\r\n") {
        bytes.push(stream.read_u8().await.unwrap());
    }
    String::from_utf8(bytes).unwrap()
}

#[tokio::test]
async fn actual_http_transport_pins_ip_preserves_host_and_rejects_oversized_body() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (mut stream, _) = listener.accept().await.unwrap();
        let headers = head(&mut stream).await;
        assert!(headers.starts_with("GET /page?q=1 HTTP/1.1\r\n"));
        assert!(
            headers
                .to_lowercase()
                .contains(&format!("host: example.test:{}\r\n", address.port()))
        );
        assert!(
            headers
                .to_lowercase()
                .contains("user-agent: localassistant/1.0")
        );
        assert!(!headers.to_lowercase().contains("authorization:"));
        stream
            .write_all(
                format!(
                    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n",
                    2 * MAX_FILE + 1
                )
                .as_bytes(),
            )
            .await
            .unwrap();
        let _ = stream.write_all(&vec![b'x'; 2 * MAX_FILE + 1]).await;
    });
    let result = request(
        Target {
            url: Url::parse(&format!("http://example.test:{}/page?q=1", address.port())).unwrap(),
            address,
        },
        None,
    )
    .await;
    assert_eq!(result.err(), Some("Page exceeds the 2 MiB download limit"));
    timeout(Duration::from_secs(3), server)
        .await
        .unwrap()
        .unwrap();
}

#[tokio::test]
async fn real_tls_preserves_sni_and_verifies_a_new_hostname_on_the_same_ip() {
    let certified = rcgen::generate_simple_self_signed(vec!["first.test".into()]).unwrap();
    let root = Certificate::from_der(certified.cert.der()).unwrap();
    let key = rustls::pki_types::PrivateKeyDer::Pkcs8(certified.signing_key.serialize_der().into());
    let config = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(vec![certified.cert.der().clone()], key)
        .unwrap();
    let acceptor = tokio_rustls::TlsAcceptor::from(Arc::new(config));
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let server = tokio::spawn(async move {
        let (stream, _) = listener.accept().await.unwrap();
        let mut stream = acceptor.accept(stream).await.unwrap();
        assert_eq!(stream.get_ref().1.server_name(), Some("first.test"));
        assert!(
            head(&mut stream)
                .await
                .to_lowercase()
                .contains(&format!("host: first.test:{}", address.port()))
        );
        stream
            .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nSafe")
            .await
            .unwrap();
        stream.shutdown().await.unwrap();
        let (stream, _) = listener.accept().await.unwrap();
        // The client must reject the first.test certificate for second.test.
        assert!(acceptor.accept(stream).await.is_err());
    });
    let first = request(
        Target {
            url: Url::parse(&format!("https://first.test:{}/page?q=1", address.port())).unwrap(),
            address,
        },
        Some(root.clone()),
    )
    .await
    .unwrap();
    assert_eq!(first.body, b"Safe");
    assert!(
        request(
            Target {
                url: Url::parse(&format!("https://second.test:{}/final", address.port())).unwrap(),
                address
            },
            Some(root)
        )
        .await
        .is_err()
    );
    timeout(Duration::from_secs(3), server)
        .await
        .unwrap()
        .unwrap();
}

#[tokio::test]
async fn search_uses_the_existing_provider_cli_with_bounded_results_and_no_shell_interpolation() {
    use std::os::unix::fs::PermissionsExt;
    let dir = tempfile::tempdir().unwrap();
    let provider = dir.path().join("provider");
    let log = dir.path().join("args");
    std::fs::write(&provider, format!("#!/bin/sh\nwhile [ $# -gt 0 ]; do\ncase \"$1\" in --query) query=$2; shift;; --max_results) count=$2; shift;; --output) output=$2; shift;; esac\nshift\ndone\nprintf '%s\\n' \"$query\" \"$count\" > '{}'\nprintf '[{{\"title\":\"Title\",\"href\":\"https://example.test\",\"body\":\"Snippet\"}}]' > \"$output\"\n", log.display())).unwrap();
    std::fs::set_permissions(&provider, std::fs::Permissions::from_mode(0o700)).unwrap();
    let query = "$(not-a-command) a query with spaces";
    let result = search(&provider, query, 100).await.unwrap();
    assert_eq!(result[0]["title"], "Title");
    assert_eq!(
        std::fs::read_to_string(log).unwrap(),
        format!("{query}\n8\n")
    );
    std::fs::write(&provider, "#!/bin/sh\nexit 1\n").unwrap();
    assert_eq!(
        search(&provider, "query", 0).await.err(),
        Some("Search provider failed")
    );
}
