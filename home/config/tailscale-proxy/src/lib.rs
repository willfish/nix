use std::{io, net::IpAddr, time::Duration};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::TcpStream,
    time::timeout,
};

pub const HEADER_LIMIT: usize = 65_536;
const SETUP_TIMEOUT: Duration = Duration::from_secs(10);
const BAD_GATEWAY: &[u8] =
    b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";

pub fn tailscale_client(peer: IpAddr) -> bool {
    match peer {
        IpAddr::V4(ip) => (u32::from(ip) & 0xffc0_0000) == 0x6440_0000,
        IpAddr::V6(ip) => match ip.to_ipv4_mapped() {
            Some(ip) => tailscale_client(ip.into()),
            None => ip.segments()[..3] == [0xfd7a, 0x115c, 0xa1e0],
        },
    }
}

pub fn validate_key(key: &str) -> io::Result<&str> {
    let key = key.trim_matches(|c: char| c.is_ascii_whitespace());
    if !key.is_ascii() || key.bytes().any(|c| c < 32 || c == 127) {
        return Err(invalid());
    }
    Ok(key)
}

fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid proxy input")
}

pub fn rewrite_request(head: &[u8], key: &str, peer: IpAddr) -> io::Result<Vec<u8>> {
    if head.len() > HEADER_LIMIT || !head.ends_with(b"\r\n\r\n") {
        return Err(invalid());
    }
    let mut headers = [httparse::EMPTY_HEADER; 128];
    let mut request = httparse::Request::new(&mut headers);
    if request.parse(head).map_err(|_| invalid())? != httparse::Status::Complete(head.len()) {
        return Err(invalid());
    }
    let key = validate_key(key)?;
    let mut websocket = false;
    let mut has_token = false;
    // Preserve the request line and field bytes, including caller credentials.
    // Only Connection and empty Authorization fields are replaced.
    let lines = head[..head.len() - 4].split(|&c| c == b'\n');
    let mut rewritten = Vec::with_capacity(head.len() + key.len() + 64);
    for (index, line) in lines.enumerate() {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        if index != 0 {
            let colon = line.iter().position(|&c| c == b':').ok_or_else(invalid)?;
            let name = &line[..colon];
            let value = line[colon + 1..].trim_ascii();
            if name.eq_ignore_ascii_case(b"connection") {
                continue;
            }
            if name.eq_ignore_ascii_case(b"upgrade") {
                websocket |= value
                    .split(|&c| c == b',')
                    .any(|value| value.trim_ascii().eq_ignore_ascii_case(b"websocket"));
            }
            if name.eq_ignore_ascii_case(b"authorization") {
                let token = if value.len() >= 6 && value[..6].eq_ignore_ascii_case(b"bearer") {
                    value[6..].trim_ascii()
                } else {
                    value
                };
                if token.is_empty() {
                    continue;
                }
                has_token = true;
            }
        }
        rewritten.extend_from_slice(line);
        rewritten.extend_from_slice(b"\r\n");
    }
    if tailscale_client(peer) && !key.is_empty() && !has_token {
        rewritten.extend_from_slice(b"Authorization: Bearer ");
        rewritten.extend_from_slice(key.as_bytes());
        rewritten.extend_from_slice(b"\r\n");
    }
    rewritten.extend_from_slice(if websocket {
        b"Connection: Upgrade\r\n\r\n"
    } else {
        b"Connection: close\r\n\r\n"
    });
    Ok(rewritten)
}

async fn read_headers(client: &mut TcpStream) -> io::Result<(Vec<u8>, Vec<u8>)> {
    let mut data = Vec::new();
    let mut buffer = [0; 4096];
    loop {
        let count = client.read(&mut buffer).await?;
        if count == 0 {
            return Err(invalid());
        }
        data.extend_from_slice(&buffer[..count]);
        if let Some(marker) = data.windows(4).position(|bytes| bytes == b"\r\n\r\n") {
            let end = marker + 4;
            if end > HEADER_LIMIT {
                return Err(invalid());
            }
            let extra = data.split_off(end);
            return Ok((data, extra));
        }
        if data.len() >= HEADER_LIMIT {
            return Err(invalid());
        }
    }
}

pub async fn proxy_connection(
    mut client: TcpStream,
    upstream: &(String, u16),
    key: &str,
    peer: IpAddr,
) {
    let setup = timeout(SETUP_TIMEOUT, async {
        let (head, extra) = read_headers(&mut client).await?;
        let head = rewrite_request(&head, key, peer)?;
        let mut server = TcpStream::connect((upstream.0.as_str(), upstream.1)).await?;
        server.write_all(&head).await?;
        server.write_all(&extra).await?;
        Ok::<_, io::Error>(server)
    })
    .await;
    match setup {
        Ok(Ok(mut server)) => {
            // Do not buffer entire replies or impose an inference timeout. This
            // carries chunked/SSE responses and upgraded WebSockets unchanged.
            let (mut client_read, mut client_write) = client.split();
            let (mut server_read, mut server_write) = server.split();
            tokio::select! {
                _ = tokio::io::copy(&mut client_read, &mut server_write) => {},
                _ = tokio::io::copy(&mut server_read, &mut client_write) => {},
            }
        }
        _ => {
            let _ = timeout(SETUP_TIMEOUT, client.write_all(BAD_GATEWAY)).await;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::net::TcpListener;

    fn rewrite(head: &[u8], peer: &str) -> String {
        String::from_utf8(rewrite_request(head, " secret\n", peer.parse().unwrap()).unwrap())
            .unwrap()
    }

    #[test]
    fn trusts_only_tailnet_ranges_including_mapped_ipv4() {
        for ip in [
            "100.64.0.0",
            "100.94.75.75",
            "100.106.132.35",
            "100.127.255.255",
            "fd7a:115c:a1e0::12",
            "::ffff:100.94.75.75",
        ] {
            assert!(tailscale_client(ip.parse().unwrap()), "{ip}");
        }
        for ip in [
            "100.63.255.255",
            "100.128.0.0",
            "100.1.2.3",
            "192.168.178.57",
            "127.0.0.1",
            "::1",
            "fd7a:115c:a1df::1",
            "fd7a:115c:a1e1::1",
            "::ffff:192.168.178.57",
        ] {
            assert!(!tailscale_client(ip.parse().unwrap()), "{ip}");
        }
    }

    #[test]
    fn injects_only_for_actual_tailnet_peers() {
        let head = b"GET /props HTTP/1.1\r\nHost: relay\r\nConnection: keep-alive\r\nX-Forwarded-For: 100.94.75.75\r\n\r\n";
        for peer in ["100.94.75.75", "fd7a:115c:a1e0::1", "::ffff:100.94.75.75"] {
            let result = rewrite(head, peer);
            assert!(result.contains("Authorization: Bearer secret\r\n"));
            assert!(result.ends_with("Connection: close\r\n\r\n"));
            assert!(!result.contains("keep-alive"));
        }
        for peer in ["192.168.178.20", "127.0.0.1", "::1"] {
            assert!(!rewrite(head, peer).contains("secret"));
        }
    }

    #[test]
    fn preserves_existing_tokens_and_replaces_empty_authorization() {
        for credential in ["Bearer client", "bearer client", "Basic client", "client"] {
            let head = format!(
                "GET /props HTTP/1.1\r\nHost: relay\r\nauthorization: {credential}\r\n\r\n"
            );
            for peer in ["100.94.75.75", "192.168.178.20"] {
                let result = rewrite(head.as_bytes(), peer);
                assert!(result.contains(&format!("authorization: {credential}\r\n")));
                assert!(!result.contains("secret"));
            }
        }
        for credential in ["", "Bearer", "bEaReR  "] {
            let head = format!(
                "GET /mcp HTTP/1.1\r\nHost: relay\r\nAuthorization: {credential}\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n\r\n"
            );
            let result = rewrite(head.as_bytes(), "fd7a:115c:a1e0::1");
            assert_eq!(result.matches("Authorization:").count(), 1);
            assert!(result.contains("Authorization: Bearer secret\r\n"));
            assert!(result.contains("Upgrade: websocket\r\n"));
            assert!(result.ends_with("Connection: Upgrade\r\n\r\n"));
        }
    }

    #[test]
    fn rejects_malformed_headers_and_keys_without_echoing_them() {
        for key in [
            "secret\r\nInjected: yes",
            "secret\0",
            "sécret",
            "secret\u{7f}",
        ] {
            assert!(validate_key(key).is_err());
        }
        for head in [
            b"GET / HTTP/1.1\r\nHost: relay".as_slice(),
            b"GET / HTTP/1.1\r\nInvalid field\r\n\r\n".as_slice(),
            b"GET / HTTP/1.1\r\nHost: relay\r\n\r\nextra".as_slice(),
        ] {
            assert!(rewrite_request(head, "secret", "100.94.75.75".parse().unwrap()).is_err());
        }
    }

    #[tokio::test]
    async fn tailnet_request_reaches_upstream_with_injected_key_and_body() {
        let upstream = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let proxy = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let upstream_address = (
            "127.0.0.1".to_string(),
            upstream.local_addr().unwrap().port(),
        );
        let mut client = TcpStream::connect(proxy.local_addr().unwrap())
            .await
            .unwrap();
        let (accepted, _) = proxy.accept().await.unwrap();
        let task = tokio::spawn(async move {
            proxy_connection(
                accepted,
                &upstream_address,
                "secret",
                "100.94.75.75".parse().unwrap(),
            )
            .await;
        });
        client
            .write_all(b"POST /props HTTP/1.1\r\nHost: relay\r\nContent-Length: 4\r\n\r\nbody")
            .await
            .unwrap();
        let (mut server, _) = upstream.accept().await.unwrap();
        let (head, mut body) = read_headers(&mut server).await.unwrap();
        assert!(
            String::from_utf8(head)
                .unwrap()
                .contains("Authorization: Bearer secret\r\n")
        );
        while body.len() < 4 {
            body.push(server.read_u8().await.unwrap());
        }
        assert_eq!(body, b"body");
        server
            .write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
            .await
            .unwrap();
        drop(server);
        let mut response = Vec::new();
        timeout(Duration::from_secs(2), client.read_to_end(&mut response))
            .await
            .unwrap()
            .unwrap();
        assert!(response.ends_with(b"ok"));
        task.await.unwrap();
    }
}
