use book_library::{Manifest, Source, acquire, atomic_json, db, digest, extract, file_hash, root};
use rusqlite::params;
use serde_json::{Value, json};
use std::{
    fs,
    io::{BufRead, BufReader, Write},
    net::TcpListener,
    process::{Command, Stdio},
};

fn metadata(url: &str) -> Source {
    Source {
        id: String::new(),
        provider: "fixture".into(),
        title: "Practical electricity".into(),
        authors: "Synthetic fixture".into(),
        category: "practical".into(),
        language: "eng".into(),
        edition: "1".into(),
        licence: "CC0".into(),
        licence_url: "https://creativecommons.org/publicdomain/zero/1.0/".into(),
        origin: "https://example.org/electricity".into(),
        url: url.into(),
        format: "txt".into(),
        size: None,
        upstream_sha256: None,
        metadata: json!({}),
    }
    .identify()
}

fn fixture() -> (tempfile::TempDir, Source) {
    let temp = tempfile::tempdir().unwrap();
    let path = root(temp.path(), true, true).unwrap();
    let text = "Electrical power is measured in watts. Energy is power multiplied by time.\nA 10 watt lamp for 3 hours uses 30 watt-hours. Résistance and current are different quantities.\n";
    let mut source = metadata("https://example.org/electricity.txt");
    source.size = Some(text.len() as u64);
    let hash = digest(text.as_bytes());
    let object = format!("objects/{hash}.txt");
    fs::write(path.join(&object), text).unwrap();
    let manifest = Manifest {
        version: 1,
        created: 1,
        policy_sha256: digest(b"fixture"),
        byte_cap: 64 << 20,
        sources: vec![source.clone()],
        exclusions: vec![],
    };
    atomic_json(&path.join("manifest.json"), &manifest).unwrap();
    acquire::seed(&path, &manifest).unwrap();
    let connection = db::connect(&path, true).unwrap();
    connection
        .execute(
            "UPDATE sources SET status='verified',object=?1,sha256=?2 WHERE id=?3",
            params![object, hash, source.id],
        )
        .unwrap();
    drop(connection);
    extract::ingest(&path, None).unwrap();
    (temp, source)
}

#[test]
fn search_read_grep_and_read_only_boundary() {
    let (temp, source) = fixture();
    let library = db::Library::open(temp.path()).unwrap();
    let result = library
        .search("power AND time", Some("practical"), None, 5)
        .unwrap();
    let hit = &result["books"]["results"][0];
    assert_eq!(hit["source_id"], source.id);
    let locator = hit["locator"].as_str().unwrap();
    let page = library.read(&source.id, locator, 0, 40).unwrap();
    assert_eq!(page["text"].as_str().unwrap().chars().count(), 40);
    assert_eq!(page["next_offset"], 40);
    assert!(page["citation"].as_str().unwrap().starts_with("book://"));
    assert!(library.read(&source.id, locator, 0, 16001).is_err());
    assert!(library.read("../../etc/passwd", locator, 0, 100).is_err());
    let matches = library
        .grep("Résistance", &[source.id.clone()], false, 5)
        .unwrap();
    assert_eq!(matches["matches"].as_array().unwrap().len(), 1);
    assert!(library.grep(".*", &[], true, 5).is_err());
    assert!(library.db.execute("DELETE FROM sources", []).is_err());
    assert!(
        db::audit(temp.path(), true).unwrap()["complete"]
            .as_bool()
            .unwrap()
    );
}

#[test]
fn independent_audit_detects_original_corruption() {
    let (temp, source) = fixture();
    let library = db::Library::open(temp.path()).unwrap();
    fs::write(library.object(&source.id).unwrap(), b"corrupt").unwrap();
    let report = db::audit(temp.path(), true).unwrap();
    assert_eq!(report["complete"], false);
    assert_eq!(report["errors"].as_array().unwrap().len(), 1);
}

#[test]
fn recreation_requires_an_explicit_category_or_source_selection() {
    let (temp, mut source) = fixture();
    source.category = "recreation".into();
    let connection = db::connect(temp.path(), true).unwrap();
    connection
        .execute(
            "UPDATE sources SET metadata=?1 WHERE id=?2",
            params![serde_json::to_string(&source).unwrap(), source.id],
        )
        .unwrap();
    drop(connection);
    let library = db::Library::open(temp.path()).unwrap();
    assert!(
        library.search("watts", None, None, 5).unwrap()["books"]["results"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    assert_eq!(
        library
            .search("watts", Some("recreation"), None, 5)
            .unwrap()["books"]["results"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    assert_eq!(
        library
            .search_scope("watts", None, None, Some(&source.id), 5)
            .unwrap()["books"]["results"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
}

#[test]
fn essentials_copy_keeps_compact_reference_and_plain_reading_export() {
    let (temp, mut source) = fixture();
    source.title = "Wikipedia".into();
    source.metadata = json!({"portable":true});
    let mut manifest = book_library::load_manifest(temp.path()).unwrap();
    manifest.sources = vec![source.clone()];
    atomic_json(&temp.path().join("manifest.json"), &manifest).unwrap();
    let connection = db::connect(temp.path(), true).unwrap();
    connection
        .execute(
            "UPDATE sources SET metadata=?1 WHERE id=?2",
            params![serde_json::to_string(&source).unwrap(), source.id],
        )
        .unwrap();
    drop(connection);
    let parent = tempfile::tempdir().unwrap();
    let destination = parent.path().join("essentials");
    assert_eq!(
        db::snapshot(temp.path(), &destination, 1).unwrap()["sources"],
        1
    );
    assert!(
        fs::read_to_string(destination.join("text").join(format!("{}.txt", source.id)))
            .unwrap()
            .contains("watt-hours")
    );
    assert!(
        db::audit(&destination, true).unwrap()["complete"]
            .as_bool()
            .unwrap()
    );
    assert!(db::snapshot(temp.path(), &destination, 1).is_err());
}

#[test]
fn acquisition_resumes_a_verified_range_without_duplicate_bytes() {
    let temp = tempfile::tempdir().unwrap();
    let path = root(temp.path(), true, true).unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let bytes = b"A synthetic book about water and energy.\n";
    let url = format!("http://{}/book.txt", listener.local_addr().unwrap());
    let mut source = metadata(&url);
    source.size = Some(bytes.len() as u64);
    source.upstream_sha256 = Some(digest(bytes));
    fs::write(
        path.join("partial").join(format!("{}.part", source.id)),
        &bytes[..10],
    )
    .unwrap();
    let manifest = Manifest {
        version: 1,
        created: 1,
        policy_sha256: digest(b"fixture"),
        byte_cap: 64 << 20,
        sources: vec![source.clone()],
        exclusions: vec![],
    };
    atomic_json(&path.join("manifest.json"), &manifest).unwrap();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut request = String::new();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        loop {
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            request.push_str(&line);
            if line == "\r\n" {
                break;
            }
        }
        assert!(request.to_ascii_lowercase().contains("range: bytes=10-"));
        write!(stream,"HTTP/1.1 206 Partial Content\r\nContent-Length: {}\r\nContent-Range: bytes 10-{}/{}\r\nETag: \"fixture-v1\"\r\nConnection: close\r\n\r\n",bytes.len()-10,bytes.len()-1,bytes.len()).unwrap();
        stream.write_all(&bytes[10..]).unwrap();
    });
    assert_eq!(
        acquire::run(&path, None, true).unwrap()["downloaded_this_run"],
        1
    );
    server.join().unwrap();
    let library = db::Library::open(&path).unwrap();
    assert_eq!(
        file_hash(&library.object(&source.id).unwrap()).unwrap(),
        source.upstream_sha256.unwrap()
    );
    assert_eq!(
        acquire::run(&path, None, true).unwrap()["downloaded_this_run"],
        0
    );
    // A verified sample receipt can seed a new generation without another HTTP request.
    drop(library);
    db::connect(&path, true)
        .unwrap()
        .execute(
            "UPDATE sources SET status='pending',object=NULL,sha256=NULL WHERE id=?",
            [&source.id],
        )
        .unwrap();
    assert_eq!(
        acquire::run(&path, None, true).unwrap()["downloaded_this_run"],
        1
    );
}

#[test]
fn acquisition_restarts_a_partial_from_a_different_request_url() {
    let temp = tempfile::tempdir().unwrap();
    let path = root(temp.path(), true, true).unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let bytes = b"The complete selected original, not an extracted derivative.\n";
    let url = format!("http://{}/original.txt", listener.local_addr().unwrap());
    let mut source = metadata(&url);
    source.size = Some(bytes.len() as u64);
    let rejected = b"Rejected derivative bytes";
    fs::write(
        path.join("partial").join(format!("{}.part", source.id)),
        rejected,
    )
    .unwrap();
    atomic_json(
        &path.join("partial").join(format!("{}.json", source.id)),
        &json!({"url":format!("{url}?sequence=1"),"etag":"\"same-item-validator\""}),
    )
    .unwrap();
    let manifest = Manifest {
        version: 1,
        created: 1,
        policy_sha256: digest(b"fixture"),
        byte_cap: 64 << 20,
        sources: vec![source.clone()],
        exclusions: vec![],
    };
    atomic_json(&path.join("manifest.json"), &manifest).unwrap();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut request = String::new();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        loop {
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            request.push_str(&line);
            if line == "\r\n" {
                break;
            }
        }
        assert!(request.starts_with("GET /original.txt HTTP/1.1\r\n"));
        assert!(!request.to_ascii_lowercase().contains("range:"));
        write!(stream,"HTTP/1.1 200 OK\r\nContent-Length: {}\r\nETag: \"same-item-validator\"\r\nConnection: close\r\n\r\n",bytes.len()).unwrap();
        stream.write_all(bytes).unwrap();
    });
    assert_eq!(
        acquire::run(&path, None, true).unwrap()["downloaded_this_run"],
        1
    );
    server.join().unwrap();
    let library = db::Library::open(&path).unwrap();
    assert_eq!(
        fs::read(library.object(&source.id).unwrap()).unwrap(),
        bytes
    );
    let receipt: Value = serde_json::from_reader(
        fs::File::open(path.join("partial").join(format!("{}.json", source.id))).unwrap(),
    )
    .unwrap();
    assert_eq!(receipt["url"], url);
    assert_eq!(receipt["response_url"], url);
}

#[test]
fn provider_cooldowns_defer_only_that_host_and_keep_failure_reports_current() {
    provider_cooldown_case(503);
    provider_cooldown_case(500);
}

fn provider_cooldown_case(status: u16) {
    let temp = tempfile::tempdir().unwrap();
    let path = root(temp.path(), true, true).unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    let mut blocked = metadata(&format!("{url}/first.txt"));
    blocked.size = Some(5);
    let mut also_blocked = metadata(&format!("{url}/second.txt"));
    also_blocked.size = Some(6);
    let mut cached = metadata("https://independent.example/book.txt");
    let text = b"An independently verified source about water.\n";
    cached.size = Some(text.len() as u64);
    let hash = digest(text);
    let object = format!("objects/{hash}.txt");
    fs::write(path.join(&object), text).unwrap();
    atomic_json(
        &path.join("receipts").join(format!("{}.json", cached.id)),
        &json!({"source_id":cached.id,"sha256":hash,"object":object}),
    )
    .unwrap();
    let manifest = Manifest {
        version: 1,
        created: 1,
        policy_sha256: digest(b"fixture"),
        byte_cap: 64 << 20,
        sources: vec![blocked.clone(), also_blocked, cached.clone()],
        exclusions: vec![],
    };
    atomic_json(&path.join("manifest.json"), &manifest).unwrap();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        loop {
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            if line == "\r\n" {
                break;
            }
        }
        write!(stream,"HTTP/1.1 {status} Fixture Error\r\nRetry-After: 3600\r\nContent-Type: text/html\r\nServer: fixture\r\nX-Cache: {}\r\nSet-Cookie: do-not-retain=private-fixture-value\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "x".repeat(2048)).unwrap();
    });
    assert!(acquire::run(&path, Some(1), true).is_err());
    server.join().unwrap();
    let refusal_path = path
        .join("partial")
        .join(format!("{}.refusal.json", blocked.id));
    let refusal: Value = serde_json::from_reader(fs::File::open(&refusal_path).unwrap()).unwrap();
    assert_eq!(refusal["source_id"], blocked.id);
    assert_eq!(refusal["request_url"], blocked.url);
    assert_eq!(refusal["response_host"], "127.0.0.1");
    assert_eq!(refusal["http_status"], status);
    assert_eq!(refusal["headers"]["retry-after"], "3600");
    assert_eq!(refusal["headers"]["content-type"], "text/html");
    assert_eq!(refusal["headers"]["server"], "fixture");
    assert_eq!(refusal["headers"].as_object().unwrap().len(), 4);
    assert_eq!(refusal["headers"]["x-cache"].as_str().unwrap().len(), 1024);
    assert!(refusal["cooldown_until"].as_u64().unwrap() >= book_library::now() + 3590);
    assert!(
        !fs::read_to_string(&refusal_path)
            .unwrap()
            .contains("private-fixture-value")
    );
    let report: Value =
        serde_json::from_reader(fs::File::open(path.join("acquisition-report.json")).unwrap())
            .unwrap();
    assert_eq!(report["downloaded_this_run"], 1);
    assert_eq!(report["failures"][0]["source_id"], blocked.id);
    assert_eq!(report["deferred_providers"]["127.0.0.1"]["sources"], 1);
    assert!(acquire::run(&path, Some(1), true).is_err());
    let report: Value =
        serde_json::from_reader(fs::File::open(path.join("acquisition-report.json")).unwrap())
            .unwrap();
    assert_eq!(report["downloaded_this_run"], 0);
    assert!(report["failures"].as_array().unwrap().is_empty());
    assert_eq!(report["deferred_providers"]["127.0.0.1"]["sources"], 2);
    let retained: Value = serde_json::from_reader(fs::File::open(&refusal_path).unwrap()).unwrap();
    assert_eq!(retained, refusal);
    db::connect(&path, true)
        .unwrap()
        .execute(
            "UPDATE sources SET status='pending' WHERE id=?",
            [&cached.id],
        )
        .unwrap();
    let report = acquire::run(&path, Some(1), true).unwrap();
    assert_eq!(report["downloaded_this_run"], 1);
    assert_eq!(report["deferred_providers"]["127.0.0.1"]["sources"], 2);
}

#[test]
fn acquisition_prioritises_untried_sources_and_rotates_persisted_failures() {
    let temp = tempfile::tempdir().unwrap();
    let path = root(temp.path(), true, true).unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    let mut first = metadata(&format!("{url}/first.txt"));
    first.size = Some(1);
    let mut second = metadata(&format!("{url}/second.txt"));
    second.size = Some(2);
    let text = b"A newly acquired practical book about clean water.\n";
    let mut untried = metadata(&format!("{url}/untried.txt"));
    untried.size = Some(text.len() as u64);
    let manifest = Manifest {
        version: 1,
        created: 1,
        policy_sha256: digest(b"fixture"),
        byte_cap: 64 << 20,
        sources: vec![first.clone(), second.clone(), untried.clone()],
        exclusions: vec![],
    };
    atomic_json(&path.join("manifest.json"), &manifest).unwrap();
    acquire::seed(&path, &manifest).unwrap();
    let connection = db::connect(&path, true).unwrap();
    for source in [&first, &second] {
        connection
            .execute(
                "UPDATE sources SET status='failed',error='retained refusal' WHERE id=?",
                [&source.id],
            )
            .unwrap();
    }
    drop(connection);
    let server = std::thread::spawn(move || {
        for endpoint in ["untried", "first", "second"] {
            let (mut stream, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            let mut request = String::new();
            reader.read_line(&mut request).unwrap();
            assert!(request.starts_with(&format!("GET /{endpoint}.txt HTTP/1.1\r\n")));
            loop {
                let mut line = String::new();
                reader.read_line(&mut line).unwrap();
                if line == "\r\n" {
                    break;
                }
            }
            if endpoint == "untried" {
                write!(
                    stream,
                    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    text.len()
                )
                .unwrap();
                stream.write_all(text).unwrap();
            } else {
                stream.write_all(b"HTTP/1.1 503 Service Unavailable\r\nRetry-After: 3600\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").unwrap();
            }
        }
    });
    assert_eq!(
        acquire::run(&path, Some(1), true).unwrap()["downloaded_this_run"],
        1
    );
    let connection = db::connect(&path, false).unwrap();
    for source in [&first, &second] {
        let retained: (String, String) = connection
            .query_row(
                "SELECT status,error FROM sources WHERE id=?",
                [&source.id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .unwrap();
        assert_eq!(retained, ("failed".into(), "retained refusal".into()));
    }
    drop(connection);
    assert!(acquire::run(&path, Some(1), true).is_err());
    // A new invocation still sends no request during the persisted host cooldown.
    assert!(acquire::run(&path, Some(1), true).is_err());
    let report: Value =
        serde_json::from_reader(fs::File::open(path.join("acquisition-report.json")).unwrap())
            .unwrap();
    assert_eq!(report["deferred_providers"]["127.0.0.1"]["sources"], 2);
    assert!(report["failures"].as_array().unwrap().is_empty());
    // Expire only the synthetic fixture's cooldown, never a production deadline.
    atomic_json(&path.join("cooldowns.json"), &json!({"127.0.0.1":0})).unwrap();
    assert!(acquire::run(&path, Some(1), true).is_err());
    server.join().unwrap();
    let report: Value =
        serde_json::from_reader(fs::File::open(path.join("acquisition-report.json")).unwrap())
            .unwrap();
    assert_eq!(report["failures"][0]["source_id"], second.id);
    let connection = db::connect(&path, false).unwrap();
    let failures: u64 = connection
        .query_row(
            "SELECT count(*) FROM sources WHERE status='failed'",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(failures, 2);
    assert_eq!(book_library::load_manifest(&path).unwrap().sources.len(), 3);
}

#[test]
fn mcp_stdio_initialises_lists_and_calls_real_retrieval() {
    let (temp, _) = fixture();
    let mut child = Command::new("timeout")
        .arg("15s")
        .arg(env!("CARGO_BIN_EXE_book-library"))
        .args(["--root", temp.path().to_str().unwrap(), "mcp"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    let mut output = BufReader::new(child.stdout.take().unwrap());
    let mut call = |request: Value| -> Value {
        writeln!(input, "{request}").unwrap();
        input.flush().unwrap();
        if request.get("id").is_none() {
            return Value::Null;
        }
        let mut response = String::new();
        output.read_line(&mut response).unwrap();
        serde_json::from_str(&response).unwrap()
    };
    let hello = call(
        json!({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"offline-fixture","version":"1"}}}),
    );
    assert_eq!(hello["result"]["serverInfo"]["name"], "Offline knowledge");
    call(json!({"jsonrpc":"2.0","method":"notifications/initialized"}));
    let list = call(json!({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}));
    assert_eq!(list["result"]["tools"].as_array().unwrap().len(), 5);
    assert!(
        list["result"]["tools"]
            .as_array()
            .unwrap()
            .iter()
            .all(|t| t["annotations"]["readOnlyHint"] == true)
    );
    let found = call(
        json!({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"search","arguments":{"query":"watts"}}}),
    );
    assert_eq!(
        found["result"]["structuredContent"]["books"]["results"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    drop(call);
    drop(input);
    drop(output);
    assert!(child.wait().unwrap().success());
}
