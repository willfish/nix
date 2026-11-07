use crate::{Manifest, RESERVE, Source, TIB, atomic_json, db, file_hash, load_manifest, now, root};
use anyhow::{Context, Result, bail, ensure};
use fs2::FileExt;
use reqwest::{blocking::Client, header};
use rusqlite::params;
use serde_json::{Value, json};
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::Path,
    thread,
    time::{Duration, Instant},
};

pub fn client() -> Result<Client> {
    Ok(Client::builder().user_agent("personal-offline-library/0.1 (licensed private reference archive; sequential requests)")
        .connect_timeout(Duration::from_secs(20)).timeout(Duration::from_secs(120))
        .redirect(reqwest::redirect::Policy::limited(8)).build()?)
}

pub fn used_bytes(root: &Path) -> Result<u64> {
    let mut total = 0_u64;
    for entry in walkdir::WalkDir::new(root).follow_links(false) {
        let entry = entry?;
        if entry.file_type().is_file() {
            match entry.metadata() {
                Ok(metadata) => {
                    total = total
                        .checked_add(metadata.len())
                        .context("Footprint overflow")?
                }
                Err(e)
                    if e.io_error()
                        .is_some_and(|e| e.kind() == std::io::ErrorKind::NotFound) =>
                {
                    ()
                }
                Err(e) => return Err(e.into()),
            }
        }
    }
    Ok(total)
}

fn space(root: &Path, footprint: u64, additional: u64, cap: u64, fixture: bool) -> Result<()> {
    ensure!(
        cap <= TIB && footprint.saturating_add(additional) <= cap,
        "1 TiB library footprint cap would be exceeded"
    );
    let available = fs2::available_space(root)?;
    let total = fs2::total_space(root)?;
    let reserve = if fixture { 0 } else { RESERVE.max(total / 5) };
    ensure!(
        available >= additional.saturating_add(reserve),
        "Insufficient space while retaining the 512 GiB / 20% reserve"
    );
    Ok(())
}

pub fn check_growth(root: &Path, additional: u64, cap: u64, fixture: bool) -> Result<()> {
    space(root, used_bytes(root)?, additional, cap, fixture)
}

pub fn seed(root: &Path, manifest: &Manifest) -> Result<()> {
    let mut connection = db::initialise(root)?;
    let transaction = connection.transaction()?;
    for source in &manifest.sources {
        transaction.execute("INSERT INTO sources(id,metadata,status) VALUES(?1,?2,'pending') ON CONFLICT(id) DO NOTHING",params![source.id,serde_json::to_string(source)?])?;
    }
    transaction.commit()?;
    Ok(())
}

fn acquisition_order(manifest: &Manifest) -> Vec<&Source> {
    let mut selected: Vec<_> = manifest.sources.iter().collect();
    selected.sort_by_key(|source| {
        let priority = if source.provider == "openstax" || source.provider == "kiwix" {
            0
        } else if source.provider == "oapen" {
            1
        } else if source.category == "recreation" {
            2
        } else {
            3
        };
        (priority, source.size.unwrap_or(32 << 20))
    });
    selected
}

fn verify_type(path: &Path, format: &str) -> Result<()> {
    let mut file = File::open(path)?;
    let mut header = [0_u8; 512];
    let n = file.read(&mut header)?;
    let valid = match format {
        "pdf" => header[..n].starts_with(b"%PDF-"),
        "epub" => header[..n].starts_with(b"PK\x03\x04"),
        "zim" => header[..n].starts_with(b"ZIM\x04"),
        "txt" => {
            !String::from_utf8_lossy(&header[..n])
                .to_ascii_lowercase()
                .contains("<!doctype html")
                && !String::from_utf8_lossy(&header[..n])
                    .to_ascii_lowercase()
                    .contains("<html")
        }
        _ => false,
    };
    ensure!(
        n > 0 && valid,
        "Download is empty or not the declared {format} format"
    );
    Ok(())
}

pub fn run(path: &Path, limit: Option<usize>, fixture: bool) -> Result<Value> {
    let root = root(path, true, fixture)?;
    let lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(root.join("acquisition.lock"))?;
    lock.try_lock_exclusive()
        .context("Another acquisition job owns this library")?;
    let manifest = load_manifest(&root)?;
    seed(&root, &manifest)?;
    let client = client()?;
    let connection = db::connect(&root, true)?;
    let mut completed = 0;
    let mut failures = Vec::new();
    let mut cooldowns: Value = fs::read(root.join("cooldowns.json"))
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or(json!({}));
    for source in acquisition_order(&manifest) {
        if limit.is_some_and(|limit| completed >= limit) {
            break;
        }
        let state: String =
            connection.query_row("SELECT status FROM sources WHERE id=?", [&source.id], |r| {
                r.get(0)
            })?;
        if matches!(state.as_str(), "verified" | "indexed" | "native") {
            continue;
        }
        let host = url::Url::parse(&source.url)?
            .host_str()
            .context("URL has no host")?
            .to_owned();
        if cooldowns[&host].as_u64().unwrap_or(0) > now() {
            bail!(
                "Provider {host} is paused until epoch {}; no request sent",
                cooldowns[&host]
            );
        }
        let result = download(
            &client,
            &root,
            source,
            manifest.byte_cap,
            fixture,
            &mut cooldowns,
        );
        match result {
            Ok((object, hash)) => {
                connection.execute("UPDATE sources SET status='verified',object=?1,sha256=?2,error=NULL WHERE id=?3",params![object,hash,source.id])?;
                atomic_json(
                    &root.join("receipts").join(format!("{}.json", source.id)),
                    &json!({"source_id":source.id,"sha256":hash,"object":object,"verified_at":now(),"upstream_sha256":source.upstream_sha256}),
                )?;
                completed += 1;
                eprintln!("verified {} ({})", source.title, source.id);
            }
            Err(error) => {
                let message = error.to_string();
                connection.execute(
                    "UPDATE sources SET status='failed',error=?1 WHERE id=?2",
                    params![message, source.id],
                )?;
                failures.push(json!({"source_id":source.id,"title":source.title,"error":message}));
                eprintln!("download failed: {}: {error:#}", source.title);
                if message.contains("HTTP 403")
                    || message.contains("HTTP 429")
                    || message.contains("capacity")
                    || message.contains("space")
                    || message.contains("cap would")
                {
                    break;
                }
            }
        }
        thread::sleep(Duration::from_secs(2));
    }
    atomic_json(&root.join("cooldowns.json"), &cooldowns)?;
    let report = json!({"downloaded_this_run":completed,"failures":failures,"footprint_bytes":used_bytes(&root)?,"byte_cap":manifest.byte_cap});
    atomic_json(&root.join("acquisition-report.json"), &report)?;
    ensure!(
        failures.is_empty(),
        "Acquisition has {} failed objects; see acquisition-report.json",
        failures.len()
    );
    Ok(report)
}

fn download(
    client: &Client,
    root: &Path,
    source: &Source,
    cap: u64,
    fixture: bool,
    cooldowns: &mut Value,
) -> Result<(String, String)> {
    let receipt_path = root.join("receipts").join(format!("{}.json", source.id));
    if receipt_path.is_file() {
        let receipt: Value = serde_json::from_reader(File::open(receipt_path)?)?;
        ensure!(
            receipt["source_id"] == source.id,
            "Cached acquisition receipt has a different source identity"
        );
        let hash = receipt["sha256"]
            .as_str()
            .filter(|hash| crate::valid_id(hash))
            .context("Cached receipt lacks a valid SHA-256")?;
        let relative = format!("objects/{hash}.{}", source.format);
        ensure!(
            receipt["object"] == relative,
            "Cached receipt has an unexpected object path"
        );
        let file = crate::safe_file(root, &relative)?;
        verify_type(&file, &source.format)?;
        if let Some(size) = source.size {
            ensure!(
                fs::metadata(&file)?.len() == size,
                "Cached object size differs from frozen metadata"
            );
        }
        if let Some(expected) = &source.upstream_sha256 {
            ensure!(
                expected.eq_ignore_ascii_case(hash),
                "Cached object differs from the upstream checksum"
            );
        }
        ensure!(
            file_hash(&file)? == hash,
            "Cached acquisition object is corrupt"
        );
        return Ok((relative, hash.to_owned()));
    }
    if let Some(expected) = &source.upstream_sha256 {
        let expected = expected.to_ascii_lowercase();
        ensure!(crate::valid_id(&expected), "Invalid upstream SHA-256");
        let relative = format!("objects/{expected}.{}", source.format);
        let existing = root.join(&relative);
        if existing.exists() {
            verify_type(&existing, &source.format)?;
            if let Some(size) = source.size {
                ensure!(
                    fs::metadata(&existing)?.len() == size,
                    "Existing object size differs from exact source metadata"
                );
            }
            ensure!(
                file_hash(&existing)? == expected,
                "Existing object SHA-256 mismatch"
            );
            return Ok((relative, expected));
        }
    }
    let url = url::Url::parse(&source.url)?;
    ensure!(
        url.scheme() == "https"
            || (fixture && url.scheme() == "http" && url.host_str() == Some("127.0.0.1")),
        "Only HTTPS acquisition URLs are allowed"
    );
    ensure!(
        url.username().is_empty() && url.password().is_none(),
        "Credentials must not be embedded in source URLs"
    );
    let partial = root.join("partial").join(format!("{}.part", source.id));
    let receipt_path = root.join("partial").join(format!("{}.json", source.id));
    let previous: Value = fs::read(&receipt_path)
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or(json!({}));
    let validator = previous["etag"]
        .as_str()
        .or_else(|| previous["last_modified"].as_str());
    let mut offset = fs::metadata(&partial).map(|m| m.len()).unwrap_or(0);
    if offset > 0 && validator.is_none() && source.upstream_sha256.is_none() {
        offset = 0;
    }
    let mut request = client
        .get(source.url.clone())
        .timeout(Duration::from_secs(24 * 3600));
    if offset > 0 {
        request = request.header(header::RANGE, format!("bytes={offset}-"));
        if let Some(value) = validator {
            request = request.header(header::IF_RANGE, value);
        }
    }
    let mut response = request.send()?;
    let status = response.status();
    if status.as_u16() == 403 || status.as_u16() == 429 || status.as_u16() == 503 {
        let delay = response
            .headers()
            .get(header::RETRY_AFTER)
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.parse::<u64>().ok())
            .unwrap_or(if status.as_u16() == 403 { 86400 } else { 3600 });
        cooldowns[url.host_str().unwrap()] = json!(now() + delay.max(60));
        atomic_json(&root.join("cooldowns.json"), cooldowns)?;
        bail!(
            "HTTP {}: provider access/rate refusal; paused for at least {} seconds",
            status.as_u16(),
            delay.max(60)
        );
    }
    ensure!(status.is_success(), "HTTP {status}");
    if offset > 0 && status.as_u16() != 206 {
        offset = 0;
    }
    if status.as_u16() == 206 {
        let range = response
            .headers()
            .get(header::CONTENT_RANGE)
            .and_then(|v| v.to_str().ok())
            .context("Partial response omitted Content-Range")?;
        ensure!(
            range.starts_with(&format!("bytes {offset}-")),
            "Unexpected resumed byte range"
        );
    }
    let etag = response
        .headers()
        .get(header::ETAG)
        .and_then(|v| v.to_str().ok())
        .map(str::to_owned);
    let last_modified = response
        .headers()
        .get(header::LAST_MODIFIED)
        .and_then(|v| v.to_str().ok())
        .map(str::to_owned);
    atomic_json(
        &receipt_path,
        &json!({"url":source.url,"etag":etag,"last_modified":last_modified}),
    )?;
    let expected = source
        .size
        .or_else(|| response.content_length().map(|length| length + offset));
    let mut footprint = used_bytes(root)?;
    space(
        root,
        footprint,
        expected.unwrap_or(0).saturating_sub(offset),
        cap,
        fixture,
    )?;
    let mut file = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(offset == 0)
        .append(offset > 0)
        .open(&partial)?;
    let start = Instant::now();
    let mut received = 0_u64;
    let mut chunk = [0_u8; 256 * 1024];
    loop {
        let n = response.read(&mut chunk)?;
        if n == 0 {
            break;
        }
        space(root, footprint, n as u64, cap, fixture)?;
        file.write_all(&chunk[..n])?;
        footprint += n as u64;
        received += n as u64;
        let minimum = Duration::from_secs_f64(received as f64 / (10.0 * 1024.0 * 1024.0));
        if minimum > start.elapsed() {
            thread::sleep(minimum - start.elapsed());
        }
    }
    file.sync_all()?;
    drop(file);
    if let Some(expected) = expected {
        ensure!(
            fs::metadata(&partial)?.len() == expected,
            "Downloaded size differs from the frozen/HTTP size"
        );
    }
    verify_type(&partial, &source.format)?;
    let hash = file_hash(&partial)?;
    if let Some(expected) = &source.upstream_sha256 {
        ensure!(
            hash.eq_ignore_ascii_case(expected),
            "Upstream SHA-256 mismatch; unverified partial retained for diagnosis"
        );
    }
    let relative = format!("objects/{hash}.{}", source.format);
    let destination = root.join(&relative);
    if destination.exists() {
        ensure!(
            file_hash(&destination)? == hash,
            "Existing content-addressed object is corrupt"
        );
        fs::remove_file(&partial)?;
    } else {
        fs::rename(&partial, &destination)?;
    }
    Ok((relative, hash))
}
