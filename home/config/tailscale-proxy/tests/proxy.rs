use std::{
    fs,
    path::PathBuf,
    process::{Child, Command, Stdio},
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tailscale_open_proxy::proxy_connection;
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::{TcpListener, TcpStream},
    task::JoinHandle,
    time::{sleep, timeout},
};

const BUDGET: Duration = Duration::from_secs(3);

struct Driver(JoinHandle<()>);
impl Drop for Driver {
    fn drop(&mut self) {
        self.0.abort();
    }
}

async fn fixture(host: &str) -> (TcpStream, TcpListener, Driver) {
    let upstream = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = (
        "127.0.0.1".to_string(),
        upstream.local_addr().unwrap().port(),
    );
    let proxy = TcpListener::bind((host, 0)).await.unwrap();
    let client = TcpStream::connect(proxy.local_addr().unwrap())
        .await
        .unwrap();
    let (accepted, peer) = proxy.accept().await.unwrap();
    let driver = Driver(tokio::spawn(async move {
        proxy_connection(accepted, &address, "fixture-server-key", peer.ip()).await;
    }));
    (client, upstream, driver)
}

async fn headers(socket: &mut TcpStream) -> Vec<u8> {
    let mut result = Vec::new();
    timeout(BUDGET, async {
        while !result.ends_with(b"\r\n\r\n") {
            result.push(socket.read_u8().await.unwrap());
        }
    })
    .await
    .expect("headers timed out");
    result
}

async fn collect(socket: &mut TcpStream) -> Vec<u8> {
    let mut result = Vec::new();
    timeout(BUDGET, socket.read_to_end(&mut result))
        .await
        .expect("response timed out")
        .unwrap();
    result
}

#[tokio::test]
async fn actual_local_peers_cannot_spoof_tailnet_authentication() {
    for host in ["127.0.0.1", "::1"] {
        let (mut client, upstream, _driver) = fixture(host).await;
        client.write_all(b"GET /props?x=1 HTTP/1.1\r\nHost: relay\r\nConnection: keep-alive\r\nX-Forwarded-For: 100.94.75.75\r\nForwarded: for=\"[fd7a:115c:a1e0::1]\"\r\n\r\n").await.unwrap();
        let (mut server, _) = timeout(BUDGET, upstream.accept()).await.unwrap().unwrap();
        let head = String::from_utf8(headers(&mut server).await).unwrap();
        assert!(head.starts_with("GET /props?x=1 HTTP/1.1\r\nHost: relay\r\n"));
        assert!(!head.contains("Authorization:"));
        assert!(!head.contains("fixture-server-key"));
        assert!(!head.contains("keep-alive"));
        assert!(head.ends_with("Connection: close\r\n\r\n"));
        server
            .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
            .await
            .unwrap();
        drop(server);
        assert!(collect(&mut client).await.ends_with(b"ok"));
    }
}

#[tokio::test]
async fn fragmented_headers_and_large_coalesced_uploads_are_forwarded_intact() {
    let (mut client, upstream, _driver) = fixture("127.0.0.1").await;
    let body = vec![b'x'; 256 * 1024];
    client
        .write_all(b"POST /completion HTTP/1.1\r\nHo")
        .await
        .unwrap();
    sleep(Duration::from_millis(10)).await;
    let body_copy = body.clone();
    let sender = tokio::spawn(async move {
        let mut request =
            format!("st: relay\r\nContent-Length: {}\r\n\r\n", body_copy.len()).into_bytes();
        request.extend_from_slice(&body_copy);
        client.write_all(&request).await.unwrap();
        client
    });
    let (mut server, _) = timeout(BUDGET, upstream.accept()).await.unwrap().unwrap();
    let head = String::from_utf8(headers(&mut server).await).unwrap();
    assert!(head.contains("Host: relay\r\n"));
    let mut received = vec![0; body.len()];
    timeout(BUDGET, server.read_exact(&mut received))
        .await
        .unwrap()
        .unwrap();
    assert_eq!(received, body);
    server
        .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
        .await
        .unwrap();
    drop(server);
    assert!(collect(&mut sender.await.unwrap()).await.ends_with(b"ok"));
}

#[tokio::test]
async fn sse_chunks_arrive_without_waiting_for_reply_completion() {
    let (mut client, upstream, _driver) = fixture("127.0.0.1").await;
    client
        .write_all(b"GET /stream HTTP/1.1\r\nHost: relay\r\n\r\n")
        .await
        .unwrap();
    let (mut server, _) = timeout(BUDGET, upstream.accept()).await.unwrap().unwrap();
    headers(&mut server).await;
    let first = b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\nb\r\ndata: one\n\n\r\n";
    server.write_all(first).await.unwrap();
    let mut received = vec![0; first.len()];
    timeout(BUDGET, client.read_exact(&mut received))
        .await
        .unwrap()
        .unwrap();
    assert_eq!(received, first);
    let last = b"b\r\ndata: two\n\n\r\n0\r\n\r\n";
    server.write_all(last).await.unwrap();
    drop(server);
    assert_eq!(collect(&mut client).await, last);
}

#[tokio::test]
async fn websocket_upgrades_and_binary_frames_are_bidirectional() {
    let (mut client, upstream, _driver) = fixture("127.0.0.1").await;
    client.write_all(b"GET /ws HTTP/1.1\r\nHost: relay\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n\r\n").await.unwrap();
    let (mut server, _) = timeout(BUDGET, upstream.accept()).await.unwrap().unwrap();
    let head = String::from_utf8(headers(&mut server).await).unwrap();
    assert!(head.contains("Upgrade: websocket\r\n"));
    assert!(head.ends_with("Connection: Upgrade\r\n\r\n"));
    server.write_all(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n").await.unwrap();
    assert!(headers(&mut client).await.starts_with(b"HTTP/1.1 101"));
    let frame = [0x82, 0x04, 0x00, 0xff, 0x01, 0x02];
    client.write_all(&frame).await.unwrap();
    let mut received = [0; 6];
    timeout(BUDGET, server.read_exact(&mut received))
        .await
        .unwrap()
        .unwrap();
    assert_eq!(received, frame);
    server.write_all(&received).await.unwrap();
    drop(server);
    assert_eq!(collect(&mut client).await, frame);
}

#[tokio::test]
async fn malformed_or_oversized_headers_do_not_reach_upstream() {
    for request in [
        "GET / HTTP/1.1\r\nInvalid field\r\n\r\n".to_string(),
        format!(
            "GET / HTTP/1.1\r\nHost: relay\r\nX-Large: {}\r\n\r\n",
            "x".repeat(65_536)
        ),
    ] {
        let (mut client, upstream, mut driver) = fixture("127.0.0.1").await;
        client.write_all(request.as_bytes()).await.unwrap();
        // Closing an oversized request can reset TCP with unread input. Check
        // the complete zero-length error response rather than requiring EOF.
        assert!(
            headers(&mut client)
                .await
                .starts_with(b"HTTP/1.1 502 Bad Gateway\r\n")
        );
        timeout(BUDGET, &mut driver.0).await.unwrap().unwrap();
        assert!(
            timeout(Duration::from_millis(20), upstream.accept())
                .await
                .is_err()
        );
    }
}

#[tokio::test]
async fn unavailable_upstream_returns_only_a_generic_failure() {
    let reserve = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = (
        "127.0.0.1".to_string(),
        reserve.local_addr().unwrap().port(),
    );
    drop(reserve);
    let proxy = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let mut client = TcpStream::connect(proxy.local_addr().unwrap())
        .await
        .unwrap();
    let (accepted, peer) = proxy.accept().await.unwrap();
    let _driver = Driver(tokio::spawn(async move {
        proxy_connection(accepted, &address, "fixture-server-key", peer.ip()).await;
    }));
    client
        .write_all(b"GET / HTTP/1.1\r\nHost: relay\r\n\r\n")
        .await
        .unwrap();
    assert_eq!(
        collect(&mut client).await,
        b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"
    );
}

#[tokio::test]
async fn disconnecting_client_releases_upstream_and_proxy_task() {
    let (mut client, upstream, mut driver) = fixture("127.0.0.1").await;
    client
        .write_all(b"GET /stream HTTP/1.1\r\nHost: relay\r\n\r\n")
        .await
        .unwrap();
    let (mut server, _) = timeout(BUDGET, upstream.accept()).await.unwrap().unwrap();
    headers(&mut server).await;
    drop(client);
    assert!(collect(&mut server).await.is_empty());
    timeout(BUDGET, &mut driver.0).await.unwrap().unwrap();
}

struct Process {
    child: Child,
    dir: PathBuf,
}
impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = fs::remove_dir_all(&self.dir);
    }
}

fn key_file(contents: &str) -> PathBuf {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let dir = std::env::temp_dir().join(format!("tailscale-proxy-{}-{stamp}", std::process::id()));
    fs::create_dir(&dir).unwrap();
    fs::write(dir.join("key"), contents).unwrap();
    dir
}

#[test]
fn cli_rejects_invalid_inputs_without_disclosing_paths_or_contents() {
    let binary = env!("CARGO_BIN_EXE_tailscale-open-proxy");
    for key in [
        "fixture-secret\r\nInjected: yes",
        "fixture-secret\0",
        "fixture-sécret",
    ] {
        let dir = key_file(key);
        let result = Command::new(binary)
            .args(["--listen-port", "0", "--upstream-port", "1", "--key-file"])
            .arg(dir.join("key"))
            .output()
            .unwrap();
        fs::remove_dir_all(&dir).unwrap();
        assert_eq!(result.status.code(), Some(1));
        let logs = String::from_utf8(result.stderr).unwrap();
        assert!(logs.contains("invalid API key"));
        assert!(!logs.contains("fixture-"));
        assert!(!logs.contains(dir.to_str().unwrap()));
        assert!(result.stdout.is_empty());
    }
    for args in [
        vec![],
        vec!["--listen-port", "invalid"],
        vec!["--unknown", "value"],
    ] {
        assert!(
            !Command::new(binary)
                .args(args)
                .output()
                .unwrap()
                .status
                .success()
        );
    }
    let help = Command::new(binary).arg("--help").output().unwrap();
    assert!(help.status.success());
    assert!(
        String::from_utf8(help.stdout)
            .unwrap()
            .contains("--key-file PATH")
    );
}

#[tokio::test]
async fn built_executable_serves_http_and_stops_on_sigterm() {
    let upstream = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let reserve = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let listen = reserve.local_addr().unwrap();
    drop(reserve);
    let dir = key_file("fixture-server-key\n");
    let child = Command::new(env!("CARGO_BIN_EXE_tailscale-open-proxy"))
        .args(["--listen-host", "127.0.0.1", "--listen-port"])
        .arg(listen.port().to_string())
        .arg("--upstream-port")
        .arg(upstream.local_addr().unwrap().port().to_string())
        .arg("--key-file")
        .arg(dir.join("key"))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let mut process = Process { child, dir };
    let mut client = timeout(BUDGET, async {
        loop {
            assert!(
                process.child.try_wait().unwrap().is_none(),
                "proxy exited during startup"
            );
            if let Ok(client) = TcpStream::connect(listen).await {
                break client;
            }
            sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .unwrap();
    client
        .write_all(b"GET /props HTTP/1.1\r\nHost: relay\r\n\r\n")
        .await
        .unwrap();
    let (mut server, _) = timeout(BUDGET, upstream.accept()).await.unwrap().unwrap();
    assert!(
        !String::from_utf8(headers(&mut server).await)
            .unwrap()
            .contains("Authorization:")
    );
    server
        .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
        .await
        .unwrap();
    drop(server);
    assert!(collect(&mut client).await.ends_with(b"ok"));
    // Unix signal delivery without a platform-specific Rust dependency.
    assert!(
        Command::new("sh")
            .args([
                "-c",
                "kill -TERM \"$1\"",
                "_",
                &process.child.id().to_string()
            ])
            .status()
            .unwrap()
            .success()
    );
    timeout(BUDGET, async {
        while process.child.try_wait().unwrap().is_none() {
            sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .unwrap();
    let mut logs = String::new();
    std::io::Read::read_to_string(&mut process.child.stderr.take().unwrap(), &mut logs).unwrap();
    assert!(logs.is_empty());
}
