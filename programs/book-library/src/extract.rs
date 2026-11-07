use crate::{Source, atomic_json, bounded_command, db, plain_html, root, safe_file, writer_lock};
use anyhow::{Context, Result, bail, ensure};
use fs2::FileExt;
use rusqlite::params;
use serde_json::{Value, json};
use std::{
    fs::{File, OpenOptions},
    io::Read,
    path::Path,
};

#[derive(Debug)]
pub struct Passage {
    pub locator: String,
    pub heading: String,
    pub body: String,
}

fn chunks(text: &str, prefix: &str, heading: &str) -> Vec<Passage> {
    let chars: Vec<char> = text.chars().collect();
    chars
        .chunks(6000)
        .enumerate()
        .filter_map(|(i, part)| {
            let body = part.iter().collect::<String>();
            if body.trim().is_empty() {
                None
            } else {
                Some(Passage {
                    locator: if i == 0 {
                        prefix.to_owned()
                    } else {
                        format!("{prefix}:part:{}", i + 1)
                    },
                    heading: heading.to_owned(),
                    body,
                })
            }
        })
        .collect()
}

pub fn extract(path: &Path, source: &Source, scratch: &Path) -> Result<(Vec<Passage>, Value)> {
    match source.format.as_str() {
        "pdf" => {
            let text = bounded_command(
                "pdftotext",
                &[
                    "-enc".into(),
                    "UTF-8".into(),
                    "-layout".into(),
                    path.to_string_lossy().into_owned(),
                    "-".into(),
                ],
                64 * 1024 * 1024,
                120,
            )?;
            let info = bounded_command(
                "pdfinfo",
                &[path.to_string_lossy().into_owned()],
                64 * 1024,
                30,
            )?;
            let count = info
                .lines()
                .find_map(|l| {
                    l.strip_prefix("Pages:")
                        .and_then(|s| s.trim().parse::<usize>().ok())
                })
                .context("Cannot determine PDF page count")?;
            ensure!(
                count <= 5000,
                "PDF exceeds 5000-page bound; requires explicit review"
            );
            let workspace = tempfile::tempdir_in(scratch)?;
            let extracted: Vec<_> = text.split('\u{c}').collect();
            let mut pages = Vec::new();
            let mut empty = Vec::new();
            let mut ocr_pages = Vec::new();
            for number in 1..=count {
                let original = extracted.get(number - 1).copied().unwrap_or("");
                let weak = original.chars().filter(|c| c.is_alphanumeric()).count() < 24;
                let ocr_text = if weak {
                    let prefix = workspace.path().join("page");
                    bounded_command(
                        "pdftoppm",
                        &[
                            "-f".into(),
                            number.to_string(),
                            "-l".into(),
                            number.to_string(),
                            "-r".into(),
                            "150".into(),
                            "-scale-to".into(),
                            "2400".into(),
                            "-singlefile".into(),
                            "-png".into(),
                            path.to_string_lossy().into_owned(),
                            prefix.to_string_lossy().into_owned(),
                        ],
                        1024,
                        90,
                    )?;
                    let text = bounded_command(
                        "tesseract",
                        &[
                            prefix.with_extension("png").to_string_lossy().into_owned(),
                            "stdout".into(),
                            "-l".into(),
                            "eng".into(),
                        ],
                        2 * 1024 * 1024,
                        90,
                    )?;
                    ocr_pages.push(number);
                    Some(text)
                } else {
                    None
                };
                let page = ocr_text
                    .as_deref()
                    .filter(|s| {
                        s.chars().filter(|c| c.is_alphanumeric()).count()
                            > original.chars().filter(|c| c.is_alphanumeric()).count()
                    })
                    .unwrap_or(original);
                if page.trim().is_empty() {
                    empty.push(number);
                }
                pages.extend(chunks(
                    page,
                    &format!("pdf:{number}"),
                    &format!(
                        "PDF page {number}{}",
                        if weak { " (OCR attempted)" } else { "" }
                    ),
                ));
            }
            ensure!(!pages.is_empty(), "No searchable text extracted from PDF");
            Ok((
                pages,
                json!({"format":"pdf-text","page_count":count,"ocr_pages":ocr_pages,"empty_text_pages":empty,"limitations":"PDF page indexes are not printed page labels. Low-text pages receive bounded English OCR; blank pages are recorded. Figures, formulas and tables may still need the original; OCR is not authoritative."}),
            ))
        }
        "epub" => {
            let mut archive = zip::ZipArchive::new(File::open(path)?)?;
            let container = {
                let mut entry = archive.by_name("META-INF/container.xml")?;
                let mut s = String::new();
                entry
                    .by_ref()
                    .take(2 * 1024 * 1024)
                    .read_to_string(&mut s)?;
                s
            };
            let doc = roxmltree::Document::parse(&container)?;
            let opf = doc
                .descendants()
                .find(|n| n.has_tag_name("rootfile"))
                .and_then(|n| n.attribute("full-path"))
                .context("EPUB has no package document")?
                .to_owned();
            let package = {
                let mut entry = archive.by_name(&opf)?;
                let mut s = String::new();
                entry
                    .by_ref()
                    .take(4 * 1024 * 1024)
                    .read_to_string(&mut s)?;
                s
            };
            let doc = roxmltree::Document::parse(&package)?;
            let base = Path::new(&opf).parent().unwrap_or_else(|| Path::new(""));
            let mut passages = Vec::new();
            for reference in doc.descendants().filter(|n| n.has_tag_name("itemref")) {
                let id = reference
                    .attribute("idref")
                    .context("EPUB spine has no ID")?;
                let href = doc
                    .descendants()
                    .find(|n| n.has_tag_name("item") && n.attribute("id") == Some(id))
                    .and_then(|n| n.attribute("href"))
                    .context("EPUB spine references a missing item")?;
                let name = base.join(href).to_string_lossy().into_owned();
                let mut html = String::new();
                archive
                    .by_name(&name)?
                    .take(8 * 1024 * 1024)
                    .read_to_string(&mut html)?;
                let text = plain_html(&html);
                passages.extend(chunks(&text, &format!("epub:{name}"), &name));
            }
            ensure!(!passages.is_empty(), "No EPUB spine text extracted");
            Ok((
                passages,
                json!({"format":"epub-text","limitations":"EPUB locations refer to spine files/parts, not invented page numbers; consult the original for illustrations."}),
            ))
        }
        "txt" => {
            let mut text = String::new();
            File::open(path)?
                .take(64 * 1024 * 1024 + 1)
                .read_to_string(&mut text)?;
            ensure!(
                text.len() <= 64 * 1024 * 1024,
                "Text exceeds extraction bound"
            );
            let passages = chunks(&text, "text:1", &source.title);
            ensure!(!passages.is_empty(), "Empty text source");
            Ok((passages, json!({"format":"plain-text"})))
        }
        _ => bail!("Unsupported extraction format"),
    }
}

pub fn ingest(path: &Path, limit: Option<usize>) -> Result<Value> {
    let root = root(path, false, true)?;
    let acquisition = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(root.join("acquisition.lock"))?;
    FileExt::try_lock_shared(&acquisition)
        .context("Acquisition is writing; retry ingestion after its bounded batch")?;
    let _lock = writer_lock(&root)?;
    let cap = crate::load_manifest(&root)?.byte_cap;
    let fixture = !root.starts_with("/srv/media");
    let mut db = db::initialise(&root)?;
    let pending = {
        let mut statement=db.prepare("SELECT id,metadata,object FROM sources WHERE status='verified' ORDER BY json_extract(metadata,'$.category'),json_extract(metadata,'$.title')")?;
        statement
            .query_map([], |r| {
                Ok((
                    r.get::<_, String>(0)?,
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?
    };
    let mut completed = 0;
    let mut failures = Vec::new();
    for (id, metadata, relative) in pending {
        if limit.is_some_and(|limit| completed >= limit) {
            break;
        }
        let source: Source = serde_json::from_str(&metadata)?;
        let object = safe_file(&root, &relative)?;
        let result = (|| -> Result<()> {
            crate::acquire::check_growth(&root, (256 << 20).min(cap / 8), cap, fixture)?;
            if source.format == "zim" {
                let info = bounded_command(
                    "zimdump",
                    &["info".into(), object.to_string_lossy().into_owned()],
                    256 * 1024,
                    30,
                )?;
                let probe = bounded_command(
                    "kiwix-search",
                    &[object.to_string_lossy().into_owned(), "water".into()],
                    512 * 1024,
                    30,
                )?;
                ensure!(
                    !probe.to_ascii_lowercase().contains("no fulltext index")
                        && !probe.to_ascii_lowercase().contains("no full-text index"),
                    "ZIM lacks a usable full-text index"
                );
                db.execute(
                    "UPDATE sources SET status='native',error=NULL,text_notes=? WHERE id=?",
                    params![
                        json!({"format":"zim","native_search_verified":true,"info":info})
                            .to_string(),
                        id
                    ],
                )?;
            } else {
                let (passages, mut notes) = extract(&object, &source, &root.join("scratch"))?;
                notes["passages"] = json!(passages.len());
                notes["text_bytes"] =
                    json!(passages.iter().map(|p| p.body.len() as u64).sum::<u64>());
                crate::acquire::check_growth(
                    &root,
                    notes["text_bytes"]
                        .as_u64()
                        .unwrap_or(0)
                        .saturating_mul(5)
                        .saturating_add(4 << 20),
                    cap,
                    fixture,
                )?;
                let transaction = db.transaction()?;
                transaction.execute("DELETE FROM passages WHERE source_id=?", [&id])?;
                for (sequence, passage) in passages.iter().enumerate() {
                    transaction.execute("INSERT INTO passages(source_id,locator,sequence,title,heading,body) VALUES(?1,?2,?3,?4,?5,?6)",params![id,passage.locator,sequence as i64,source.title,passage.heading,passage.body])?;
                }
                transaction.execute(
                    "UPDATE sources SET status='indexed',error=NULL,text_notes=? WHERE id=?",
                    params![notes.to_string(), id],
                )?;
                transaction.commit()?;
            }
            ensure!(
                crate::acquire::used_bytes(&root)? <= cap,
                "Library footprint cap reached during indexing; stop maintenance and inspect the growth estimate"
            );
            Ok(())
        })();
        match result {
            Ok(()) => {
                completed += 1;
                eprintln!("indexed {} ({id})", source.title);
            }
            Err(error) => {
                db.execute(
                    "UPDATE sources SET error=?1 WHERE id=?2",
                    params![format!("extraction: {error:#}"), id],
                )?;
                failures
                    .push(json!({"source_id":id,"title":source.title,"error":error.to_string()}));
                eprintln!("extraction failed: {}: {error:#}", source.title);
            }
        }
    }
    let report = json!({"indexed_this_run":completed,"failures":failures});
    atomic_json(&root.join("ingestion-report.json"), &report)?;
    ensure!(
        failures.is_empty(),
        "Extraction has {} failures; see ingestion-report.json",
        failures.len()
    );
    Ok(report)
}
