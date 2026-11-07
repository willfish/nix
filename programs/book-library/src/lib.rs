pub mod acquire;
pub mod db;
pub mod extract;
pub mod mcp;
pub mod sources;
pub mod zim;

use anyhow::{Context, Result, bail, ensure};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::{Component, Path, PathBuf},
    process::{Command, Stdio},
    time::{SystemTime, UNIX_EPOCH},
};

pub const TIB: u64 = 1 << 40;
pub const RESERVE: u64 = 512 << 30;

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Source {
    pub id: String,
    pub provider: String,
    pub title: String,
    #[serde(default)]
    pub authors: String,
    pub category: String,
    pub language: String,
    pub edition: String,
    pub licence: String,
    pub licence_url: String,
    pub origin: String,
    pub url: String,
    pub format: String,
    pub size: Option<u64>,
    pub upstream_sha256: Option<String>,
    #[serde(default)]
    pub metadata: serde_json::Value,
}

impl Source {
    pub fn identify(mut self) -> Self {
        self.id = digest(format!("{}\n{}\n{}", self.provider, self.url, self.edition).as_bytes());
        self
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Manifest {
    pub version: u32,
    pub created: u64,
    pub policy_sha256: String,
    pub byte_cap: u64,
    pub sources: Vec<Source>,
    pub exclusions: Vec<serde_json::Value>,
}

pub fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

pub fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

pub fn file_hash(path: &Path) -> Result<String> {
    let mut hash = Sha256::new();
    let mut reader = File::open(path)?;
    let mut buf = [0_u8; 128 * 1024];
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hash.update(&buf[..n]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

pub fn valid_id(id: &str) -> bool {
    id.len() == 64
        && id
            .bytes()
            .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
}

pub fn root(path: &Path, create: bool, allow_non_zfs: bool) -> Result<PathBuf> {
    if create {
        ensure!(path.is_absolute(), "Library root must be absolute");
        if !allow_non_zfs {
            let existing = path
                .ancestors()
                .find(|p| p.exists())
                .context("No existing parent")?;
            let result = Command::new("findmnt")
                .args(["-n", "-o", "FSTYPE", "-T"])
                .arg(existing)
                .output()?;
            ensure!(
                result.status.success() && String::from_utf8_lossy(&result.stdout).trim() == "zfs",
                "Acquisition requires a mounted ZFS root; refusing OS-disk fallback"
            );
        }
        fs::create_dir_all(path)?;
        for subdir in [
            "objects",
            "partial",
            "scratch",
            "manifests",
            "receipts",
            "recovery",
        ] {
            fs::create_dir_all(path.join(subdir))?;
        }
    }
    path.canonicalize().context("Library root does not exist")
}

pub fn writer_lock(root: &Path) -> Result<File> {
    let file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(root.join("writer.lock"))?;
    file.try_lock_exclusive()
        .context("Another library maintenance job owns the writer lock")?;
    Ok(file)
}

pub fn safe_file(root: &Path, relative: &str) -> Result<PathBuf> {
    let rel = Path::new(relative);
    ensure!(
        !rel.is_absolute() && rel.components().all(|c| matches!(c, Component::Normal(_))),
        "Invalid stored object path"
    );
    let path = root.join(rel).canonicalize()?;
    ensure!(
        path.starts_with(root) && path.is_file(),
        "Object escapes the library root or is not a file"
    );
    Ok(path)
}

pub fn atomic_json(path: &Path, value: &impl Serialize) -> Result<()> {
    let mut temp = tempfile::NamedTempFile::new_in(path.parent().context("Missing parent")?)?;
    serde_json::to_writer_pretty(&mut temp, value)?;
    temp.write_all(b"\n")?;
    temp.as_file().sync_all()?;
    temp.persist(path).map_err(|e| e.error)?;
    Ok(())
}

pub fn load_manifest(root: &Path) -> Result<Manifest> {
    let manifest: Manifest = serde_json::from_reader(File::open(root.join("manifest.json"))?)?;
    ensure!(
        manifest.version == 1 && manifest.byte_cap <= TIB,
        "Unsupported manifest or cap above 1 TiB"
    );
    for source in &manifest.sources {
        ensure!(
            matches!(source.format.as_str(), "pdf" | "epub" | "zim" | "txt"),
            "Unsupported source format"
        );
        if let Some(hash) = &source.upstream_sha256 {
            ensure!(
                valid_id(&hash.to_ascii_lowercase()),
                "Invalid upstream checksum"
            );
        }
        ensure!(
            valid_id(&source.id) && source.clone().identify().id == source.id,
            "Invalid source identity"
        );
        ensure!(
            !source.licence.is_empty() && !source.licence_url.is_empty(),
            "Missing source licence"
        );
    }
    Ok(manifest)
}

pub fn bounded_command(
    program: &str,
    args: &[String],
    max_bytes: usize,
    seconds: u64,
) -> Result<String> {
    use std::os::unix::process::CommandExt;
    let mut child = Command::new("timeout")
        .process_group(0)
        .arg("--kill-after=5s")
        .arg(format!("{seconds}s"))
        .arg(program)
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .with_context(|| format!("Start packaged {program}"))?;
    let mut out = Vec::new();
    child
        .stdout
        .take()
        .context("Missing output")?
        .take(max_bytes as u64 + 1)
        .read_to_end(&mut out)?;
    if out.len() > max_bytes {
        // The group contains only this invocation and its extractor children.
        unsafe {
            libc::kill(-(child.id() as i32), libc::SIGKILL);
        }
        let _ = child.wait();
        bail!("{program} exceeded its output bound");
    }
    let status = child.wait()?;
    ensure!(status.success(), "{program} failed or timed out: {status}");
    Ok(String::from_utf8_lossy(&out).into_owned())
}

pub fn plain_html(input: &str) -> String {
    let document = scraper::Html::parse_document(input);
    let selector =
        scraper::Selector::parse("p, h1, h2, h3, h4, li, pre, blockquote, td, th").unwrap();
    let mut blocks = Vec::new();
    for element in document.select(&selector) {
        if element
            .ancestors()
            .filter_map(scraper::ElementRef::wrap)
            .any(|e| matches!(e.value().name(), "script" | "style" | "nav" | "footer"))
        {
            continue;
        }
        if element
            .ancestors()
            .skip(1)
            .filter_map(scraper::ElementRef::wrap)
            .any(|e| matches!(e.value().name(), "li" | "blockquote" | "td" | "th"))
        {
            continue;
        }
        let text = element.text().collect::<Vec<_>>().join(" ");
        let text = text.split_whitespace().collect::<Vec<_>>().join(" ");
        if !text.is_empty() {
            blocks.push(text);
        }
    }
    blocks.join("\n\n")
}
