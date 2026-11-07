use crate::{Manifest, Source, TIB, acquire, atomic_json, digest, now, root, writer_lock};
use anyhow::{Context, Result, ensure};
use reqwest::blocking::Client;
use roxmltree::Node;
use serde_json::{Value, json};
use std::{
    collections::{HashMap, HashSet},
    fs,
    io::Read,
    path::Path,
    thread,
    time::Duration,
};

fn text(node: Node<'_, '_>, name: &str) -> String {
    node.children()
        .find(|n| n.is_element() && n.tag_name().name() == name)
        .and_then(|n| n.text())
        .unwrap_or_default()
        .trim()
        .to_owned()
}

fn cached(client: &Client, root: &Path, key: &str, url: &str) -> Result<String> {
    let path = root.join("manifests").join("discovery").join(key);
    if path.exists() {
        return Ok(fs::read_to_string(path)?);
    }
    eprintln!("resolve {url}");
    let response = client.get(url).send()?.error_for_status()?;
    let mut body = String::new();
    response
        .take(64 * 1024 * 1024 + 1)
        .read_to_string(&mut body)?;
    ensure!(
        body.len() <= 64 * 1024 * 1024,
        "Metadata response exceeded 64 MiB"
    );
    let mut file = tempfile::NamedTempFile::new_in(path.parent().unwrap())?;
    std::io::Write::write_all(&mut file, body.as_bytes())?;
    file.persist(path).map_err(|e| e.error)?;
    thread::sleep(Duration::from_secs(2));
    Ok(body)
}

fn openstax(client: &Client, root: &Path) -> Result<Vec<Source>> {
    let mut output = Vec::new();
    let mut offset = 0;
    loop {
        let url = format!(
            "https://openstax.org/apps/cms/api/v2/pages/?type=books.Book&limit=100&offset={offset}&fields=title,book_state,publish_date,authors,license_name,license_url,pdf_url,last_updated_pdf,book_subjects"
        );
        let page: Value = serde_json::from_str(&cached(
            client,
            root,
            &format!("openstax-{offset}.json"),
            &url,
        )?)?;
        let books = page["items"]
            .as_array()
            .context("OpenStax returned no books array")?;
        for book in books {
            if book["book_state"] != "live" || book["meta"]["locale"] != "en" {
                continue;
            }
            let Some(url) = book["pdf_url"]
                .as_str()
                .filter(|s| s.starts_with("https://"))
            else {
                continue;
            };
            let licence_url = book["license_url"].as_str().unwrap_or_default();
            if !licence_url.contains("creativecommons.org/") {
                continue;
            }
            let authors = book["authors"]
                .as_array()
                .map(|a| {
                    a.iter()
                        .filter_map(|a| a["value"]["name"].as_str())
                        .collect::<Vec<_>>()
                        .join("; ")
                })
                .unwrap_or_default();
            output.push(
                Source {
                    id: String::new(),
                    provider: "openstax".into(),
                    title: book["title"].as_str().unwrap_or("OpenStax textbook").into(),
                    authors,
                    category: "education".into(),
                    language: "eng".into(),
                    edition: book["last_updated_pdf"]
                        .as_str()
                        .or_else(|| book["publish_date"].as_str())
                        .unwrap_or("catalogue snapshot")
                        .into(),
                    licence: book["license_name"]
                        .as_str()
                        .unwrap_or("Creative Commons, see edition notice")
                        .into(),
                    licence_url: licence_url.into(),
                    origin: book["meta"]["html_url"]
                        .as_str()
                        .context("OpenStax book lacks canonical URL")?
                        .into(),
                    url: url.into(),
                    format: "pdf".into(),
                    size: None,
                    upstream_sha256: None,
                    metadata: book.clone(),
                }
                .identify(),
            );
        }
        offset += books.len();
        if books.is_empty() || offset >= page["meta"]["total_count"].as_u64().unwrap_or(0) as usize
        {
            break;
        }
    }
    ensure!(
        !output.is_empty(),
        "OpenStax selection is empty; inspect catalogue/locale fields"
    );
    Ok(output)
}

fn metalink_size(document: &roxmltree::Document<'_>) -> Result<u64> {
    let file = document
        .descendants()
        .find(|n| n.is_element() && n.tag_name().name() == "file")
        .context("Metalink has no file record")?;
    let size: u64 = text(file, "size")
        .parse()
        .context("Metalink has no exact byte length")?;
    ensure!(size > 0, "Empty archive in Metalink");
    Ok(size)
}

fn kiwix(
    client: &Client,
    root: &Path,
    policy: &Value,
    excluded: &mut Vec<Value>,
) -> Result<Vec<Source>> {
    let mut output = Vec::new();
    for collection in policy["kiwix"].as_array().context("Missing Kiwix policy")? {
        let name = collection["name"]
            .as_str()
            .context("Missing archive name")?;
        let flavour = collection["flavour"].as_str().unwrap_or_default();
        let url =
            format!("https://library.kiwix.org/catalog/v2/entries?lang=eng&count=100&name={name}");
        let xml = cached(
            client,
            root,
            &format!("kiwix-{}-{}.xml", digest(name.as_bytes()), flavour),
            &url,
        )?;
        let doc = roxmltree::Document::parse(&xml)?;
        let mut entries: Vec<_> = doc
            .descendants()
            .filter(|n| {
                n.is_element()
                    && n.tag_name().name() == "entry"
                    && text(*n, "name") == name
                    && text(*n, "flavour") == flavour
            })
            .collect();
        entries.sort_by_key(|n| text(*n, "updated"));
        let Some(entry) = entries.last().copied() else {
            excluded.push(json!({"collection":name,"flavour":flavour,"reason":"No matching official English archive"}));
            continue;
        };
        if !text(entry, "tags").contains("_ftindex:yes") {
            excluded.push(
                json!({"collection":name,"reason":"Archive does not advertise a full-text index"}),
            );
            continue;
        }
        let link = entry
            .children()
            .find(|n| {
                n.is_element()
                    && n.tag_name().name() == "link"
                    && n.attribute("type") == Some("application/x-zim")
            })
            .context("Missing ZIM acquisition link")?;
        let metalink = link.attribute("href").context("Missing acquisition URL")?;
        let mut download = metalink
            .strip_suffix(".meta4")
            .unwrap_or(metalink)
            .to_owned();
        let mut sha = None;
        let mut size = None;
        if metalink.ends_with(".meta4") {
            let meta = cached(
                client,
                root,
                &format!("kiwix-{}-{}.meta4", digest(name.as_bytes()), flavour),
                metalink,
            )?;
            let document = roxmltree::Document::parse(&meta)?;
            // OPDS rounds to KiB; Metalink binds the exact byte length to its checksum.
            size = Some(metalink_size(&document)?);
            sha = document
                .descendants()
                .find(|n| {
                    n.is_element()
                        && n.tag_name().name() == "hash"
                        && matches!(n.attribute("type"), Some("sha-256" | "sha256"))
                })
                .and_then(|n| n.text())
                .map(|s| s.trim().to_owned());
            if let Some(url) = document
                .descendants()
                .filter(|n| n.is_element() && n.tag_name().name() == "url")
                .filter_map(|n| n.text())
                .find(|u| u.starts_with("https://download.kiwix.org/"))
            {
                download = url.into();
            }
        }
        output.push(Source {
            id:String::new(),provider:"kiwix".into(),title:text(entry,"title"),authors:text(entry,"author"),category:collection["category"].as_str().context("Missing category")?.into(),language:"eng".into(),edition:text(entry,"updated"),
            licence:collection["licence"].as_str().context("Missing licence")?.into(),licence_url:collection["licence_url"].as_str().context("Missing licence URL")?.into(),
            origin:entry.children().find(|n|n.is_element() && n.tag_name().name()=="link" && n.attribute("type")==Some("text/html")).and_then(|n|n.attribute("href")).unwrap_or("https://library.kiwix.org/").into(),url:download,format:"zim".into(),size,upstream_sha256:sha,
            metadata:json!({"archive_name":name,"flavour":flavour,"uuid":text(entry,"id"),"tags":text(entry,"tags"),"article_count":text(entry,"articleCount"),"portable":collection["portable"]}),
        }.identify());
    }
    Ok(output)
}

fn oapen(client: &Client, root: &Path, limit: usize) -> Result<Vec<Source>> {
    let identify = cached(
        client,
        root,
        "oapen-identify.xml",
        "https://library.oapen.org/oai/request?verb=Identify",
    )?;
    let identify = roxmltree::Document::parse(&identify)?;
    let timestamp = |tag: &str| -> Result<i64> {
        let value = identify
            .descendants()
            .find(|n| n.has_tag_name(tag))
            .and_then(|n| n.text())
            .with_context(|| format!("OAPEN Identify has no {tag}"))?;
        Ok(chrono::DateTime::parse_from_rfc3339(value)?.timestamp())
    };
    let mut intervals = vec![(timestamp("earliestDatestamp")?, timestamp("responseDate")?)];
    let mut output = Vec::new();
    let mut identities = HashSet::new();
    while let Some((from, until)) = intervals.pop() {
        let date = |epoch| {
            chrono::DateTime::from_timestamp(epoch, 0)
                .unwrap()
                .format("%Y-%m-%dT%H:%M:%SZ")
                .to_string()
        };
        let (from_date, until_date) = (date(from), date(until));
        let mut token = String::new();
        let mut page = 0;
        let mut received = 0;
        let mut expected = None;
        loop {
            let mut url = url::Url::parse("https://library.oapen.org/oai/request")?;
            url.query_pairs_mut().append_pair("verb", "ListRecords");
            if token.is_empty() {
                url.query_pairs_mut()
                    .append_pair("metadataPrefix", "oai_dc")
                    .append_pair("from", &from_date)
                    .append_pair("until", &until_date);
            } else {
                url.query_pairs_mut().append_pair("resumptionToken", &token);
            }
            let xml = cached(
                client,
                root,
                &format!("oapen-{from}-{until}-{page:05}.xml"),
                url.as_str(),
            )?;
            let doc = roxmltree::Document::parse(&xml)?;
            if let Some(error) = doc.descendants().find(|n| n.has_tag_name("error")) {
                ensure!(
                    page == 0 && error.attribute("code") == Some("noRecordsMatch"),
                    "OAPEN harvest stopped before completeness: {}",
                    error.text().unwrap_or("OAI error")
                );
                break;
            }
            let total = doc
                .descendants()
                .find(|n| n.has_tag_name("resumptionToken"))
                .and_then(|n| n.attribute("completeListSize"))
                .and_then(|s| s.parse::<usize>().ok());
            if page == 0 {
                expected = total;
                // The provider's unfiltered OAI cursor stops at 25,000. Partition
                // by second-granularity modification dates before reaching it.
                if total.is_some_and(|count| count > 24000) {
                    ensure!(
                        from < until,
                        "OAPEN has over 24,000 records at one timestamp; cannot harvest completely"
                    );
                    let middle = from + (until - from) / 2;
                    intervals.push((middle + 1, until));
                    intervals.push((from, middle));
                    break;
                }
            }
            received += doc
                .descendants()
                .filter(|n| n.has_tag_name("record"))
                .count();
            for source in oapen_records(&doc)? {
                if identities.insert(source.id.clone()) {
                    output.push(source);
                }
                if limit > 0 && output.len() >= limit {
                    return Ok(output);
                }
            }
            token = doc
                .descendants()
                .find(|n| n.has_tag_name("resumptionToken"))
                .and_then(|n| n.text())
                .unwrap_or_default()
                .trim()
                .to_owned();
            if token.is_empty() {
                ensure!(
                    expected.is_none_or(|count| received == count),
                    "OAPEN harvest count differs from its advertised coverage"
                );
                break;
            }
            page += 1;
            ensure!(page <= 240, "OAPEN partition exceeded its bounded cursor");
        }
    }
    Ok(output)
}

fn oapen_records(doc: &roxmltree::Document<'_>) -> Result<Vec<Source>> {
    let mut output = Vec::new();
    for record in doc
        .descendants()
        .filter(|n| n.is_element() && n.tag_name().name() == "record")
    {
        let values = |name: &str| -> Vec<String> {
            record
                .descendants()
                .filter(|n| n.is_element() && n.tag_name().name() == name)
                .filter_map(|n| n.text())
                .map(|s| s.trim().to_owned())
                .collect()
        };
        if !values("language").iter().any(|s| s == "eng" || s == "en")
            || !values("resourceType").iter().any(|s| s == "book")
        {
            continue;
        }
        let classifications: Vec<_> = record
            .descendants()
            .filter(|n| {
                n.is_element()
                    && n.tag_name().name() == "subject"
                    && n.attribute("type") == Some("classification")
            })
            .filter_map(|n| n.text())
            .collect();
        if !classifications.iter().any(|s| {
            ["::P ", "::M ", "::T ", "::U ", "::R "]
                .iter()
                .any(|prefix| s.contains(prefix))
        }) {
            continue;
        }
        let Some(licence) = record.descendants().find(|n| {
            n.is_element()
                && n.tag_name().name() == "licenseCondition"
                && n.attribute("uri").is_some_and(|u| {
                    u.starts_with("https://creativecommons.org/")
                        || u.starts_with("http://creativecommons.org/")
                })
        }) else {
            continue;
        };
        let Some(origin) = values("identifier")
            .into_iter()
            .find(|u| u.starts_with("https://library.oapen.org/handle/"))
        else {
            continue;
        };
        let Some(filename) = values("source").into_iter().find(|s| {
            s.to_ascii_lowercase().ends_with(".pdf") && !s.contains('/') && !s.contains('\\')
        }) else {
            continue;
        };
        let handle = origin
            .strip_prefix("https://library.oapen.org/handle/")
            .unwrap();
        let mut download = url::Url::parse("https://library.oapen.org/bitstream/handle/")?;
        download
            .path_segments_mut()
            .unwrap()
            .pop_if_empty()
            .extend(handle.split('/'))
            .push(&filename);
        download
            .query_pairs_mut()
            .append_pair("sequence", "1")
            .append_pair("isAllowed", "y");
        output.push(Source {
                id:String::new(),provider:"oapen".into(),title:values("title").first().context("OAPEN title missing")?.clone(),authors:values("creator").join("; "),category:"practical".into(),language:"eng".into(),edition:values("datestamp").first().cloned().unwrap_or_default(),
                licence:licence.text().unwrap_or("Creative Commons; see original edition").trim().into(),licence_url:licence.attribute("uri").unwrap().into(),origin,url:download.into(),format:"pdf".into(),size:None,upstream_sha256:None,
                metadata:json!({"subjects":values("subject"),"classifications":classifications,"dates":values("date"),"publishers":values("publisher"),"pages":values("pages"),"metadata_route":"public OAI-PMH; not the restricted REST API"}),
            }.identify());
    }
    Ok(output)
}

fn gutenberg(client: &Client, root: &Path, policy: &Value) -> Result<Vec<Source>> {
    let catalogue = cached(
        client,
        root,
        "gutenberg-catalogue.csv",
        "https://www.gutenberg.org/cache/epub/feeds/pg_catalog.csv",
    )?;
    let mut reader = csv::Reader::from_reader(catalogue.as_bytes());
    let columns = reader.headers()?.clone();
    let column = |name: &str| {
        columns
            .iter()
            .position(|c| c == name)
            .with_context(|| format!("Missing Gutenberg column {name}"))
    };
    let id_col = column("Text#")?;
    let type_col = column("Type")?;
    let language_col = column("Language")?;
    let authors_col = column("Authors")?;
    let title_col = column("Title")?;
    let allowed = policy["gutenberg_authors"]
        .as_array()
        .context("Missing author policy")?;
    let mut counts = HashMap::<String, usize>::new();
    let mut output = Vec::new();
    for row in reader.records() {
        let row = row?;
        if row.get(type_col) != Some("Text") || row.get(language_col) != Some("en") {
            continue;
        }
        let authors = row.get(authors_col).unwrap_or_default();
        let title = row.get(title_col).unwrap_or_default();
        if ["letters", "diaries", "notebook"]
            .iter()
            .any(|s| title.to_ascii_lowercase().contains(s))
        {
            continue;
        }
        let Some(author) = allowed
            .iter()
            .find(|a| authors.starts_with(a["search"].as_str().unwrap_or("!")))
        else {
            continue;
        };
        let author_name = author["search"].as_str().unwrap();
        if authors.contains(';') || *counts.get(author_name).unwrap_or(&0) >= 20 {
            continue;
        }
        let id = row.get(id_col).context("Missing Gutenberg ID")?;
        ensure!(
            id.bytes().all(|b| b.is_ascii_digit()),
            "Invalid Gutenberg ID"
        );
        let rdf = cached(
            client,
            root,
            &format!("gutenberg-{id}.rdf"),
            &format!("https://www.gutenberg.org/ebooks/{id}.rdf"),
        )?;
        let doc = roxmltree::Document::parse(&rdf)?;
        if !doc.descendants().any(|n| {
            n.is_element()
                && n.tag_name().name() == "rights"
                && n.text() == Some("Public domain in the USA.")
        }) {
            continue;
        }
        let creators: Vec<_> = doc
            .descendants()
            .filter(|n| {
                n.is_element()
                    && n.tag_name().name() == "agent"
                    && n.parent().is_some_and(|p| p.tag_name().name() == "creator")
            })
            .collect();
        if creators.is_empty()
            || !creators.iter().all(|n| {
                text(*n, "name").starts_with(author_name)
                    && text(*n, "deathdate").parse::<i64>().ok() == author["death_year"].as_i64()
            })
        {
            continue;
        }
        if doc
            .descendants()
            .any(|n| n.is_element() && matches!(n.tag_name().name(), "trl" | "edt" | "adp"))
        {
            continue;
        }
        let Some(file) = doc.descendants().find(|n| {
            n.is_element()
                && n.tag_name().name() == "file"
                && n.attributes().any(|a| {
                    a.name() == "about"
                        && a.value().starts_with("https://www.gutenberg.org/")
                        && a.value().ends_with(".txt.utf-8")
                })
        }) else {
            continue;
        };
        let url = file
            .attributes()
            .find(|a| a.name() == "about")
            .unwrap()
            .value();
        output.push(Source {id:String::new(),provider:"gutenberg".into(),title:title.into(),authors:authors.into(),category:"recreation".into(),language:"eng".into(),edition:format!("Project Gutenberg {id}, {}",text(file,"modified")),licence:"Public-domain English original selected using creator death dates and edition metadata; Gutenberg distribution notice retained".into(),licence_url:"https://www.gutenberg.org/policy/license.html".into(),origin:format!("https://www.gutenberg.org/ebooks/{id}"),url:url.into(),format:"txt".into(),size:text(file,"extent").parse().ok(),upstream_sha256:None,metadata:json!({"catalogue_id":id,"creator_death_year":author["death_year"],"scope":"Published English originals; plain text only, no illustrations, translations or modern editorial contributions","uk_rights_basis":"Selected creators died by 1955; current year 2026; uncertain editions excluded"})}.identify());
        *counts.entry(author_name.into()).or_default() += 1;
    }
    output.sort_by(|a, b| a.title.cmp(&b.title));
    output.dedup_by(|a, b| a.id == b.id);
    Ok(output)
}

pub fn lock(
    path: &Path,
    policy_path: Option<&Path>,
    oapen_limit: usize,
    fixture: bool,
) -> Result<Value> {
    let root = root(path, true, fixture)?;
    let _lock = writer_lock(&root)?;
    ensure!(
        !root.join("manifest.json").exists(),
        "Source lock already exists; acquisition is frozen. Resolve a new generation in a separate staging root, never overwrite this lock."
    );
    fs::create_dir_all(root.join("manifests/discovery"))?;
    let policy_text = match policy_path {
        Some(path) => fs::read_to_string(path)?,
        None => include_str!("../assets/sources.json").to_owned(),
    };
    let policy: Value = serde_json::from_str(&policy_text)?;
    atomic_json(&root.join("manifests/policy.json"), &policy)?;
    let cap = policy["byte_cap"].as_u64().context("Missing byte cap")?;
    ensure!(cap <= TIB, "Policy exceeds the approved 1 TiB cap");
    let client = acquire::client()?;
    let mut selected = Vec::new();
    let mut exclusions = Vec::new();
    if policy["openstax"] == true {
        let books = openstax(&client, &root)?;
        selected.extend(books.into_iter().filter(|book| {
            policy["openstax_titles"].as_array().is_none_or(|titles| {
                titles
                    .iter()
                    .any(|title| title.as_str() == Some(book.title.as_str()))
            })
        }));
    }
    selected.extend(kiwix(&client, &root, &policy, &mut exclusions)?);
    if policy["gutenberg"] == true {
        selected.extend(gutenberg(&client, &root, &policy)?);
    }
    if policy["oapen"] == true {
        selected.extend(oapen(&client, &root, oapen_limit)?);
    }
    let mut identities = HashSet::new();
    selected.retain(|source| identities.insert(source.id.clone()));
    let known = selected.iter().filter_map(|s| s.size).sum::<u64>();
    let unknown = selected.iter().filter(|s| s.size.is_none()).count();
    let estimated = known
        .saturating_add(unknown as u64 * (32 << 20))
        .saturating_add(192 << 30);
    let report = json!({"selected":selected.len(),"known_original_bytes":known,"unknown_size_objects":unknown,"estimated_peak_bytes":estimated,"byte_cap":cap,"exclusions":exclusions,"oapen_selection_limit":oapen_limit,"requires_actual_space_checks":true});
    atomic_json(&root.join("selection-report.json"), &report)?;
    ensure!(
        !selected.is_empty() && estimated <= cap,
        "Selection is empty or estimated footprint exceeds capacity; review selection-report.json before freezing"
    );
    let manifest = Manifest {
        version: 1,
        created: now(),
        policy_sha256: digest(policy_text.as_bytes()),
        byte_cap: cap,
        sources: selected,
        exclusions,
    };
    atomic_json(&root.join("manifest.json"), &manifest)?;
    let hash = crate::file_hash(&root.join("manifest.json"))?;
    atomic_json(
        &root.join("manifests").join(format!("{hash}.json")),
        &manifest,
    )?;
    acquire::seed(&root, &manifest)?;
    Ok(report)
}

pub fn check_updates(path: &Path, destination: &Path) -> Result<Value> {
    ensure!(
        !destination.exists(),
        "Update check needs a new staging destination; the live lock is never replaced"
    );
    let old = crate::load_manifest(path)?;
    let policy = path.join("manifests/policy.json");
    lock(
        destination,
        if policy.is_file() {
            Some(policy.as_path())
        } else {
            None
        },
        0,
        false,
    )?;
    let new = crate::load_manifest(destination)?;
    let old_ids: HashSet<_> = old.sources.iter().map(|s| s.id.as_str()).collect();
    let new_ids: HashSet<_> = new.sources.iter().map(|s| s.id.as_str()).collect();
    let added: Vec<_> = new
        .sources
        .iter()
        .filter(|s| !old_ids.contains(s.id.as_str()))
        .map(|s| json!({"id":s.id,"title":s.title,"edition":s.edition}))
        .collect();
    let removed: Vec<_> = old
        .sources
        .iter()
        .filter(|s| !new_ids.contains(s.id.as_str()))
        .map(|s| json!({"id":s.id,"title":s.title,"edition":s.edition}))
        .collect();
    Ok(
        json!({"metadata_only":true,"live_manifest_unchanged":true,"staging_root":destination,"added_or_new_edition":added,"no_longer_selected":removed}),
    )
}

#[cfg(test)]
mod tests {
    #[test]
    fn metalink_uses_exact_bytes_not_the_rounded_opds_length() {
        let document=roxmltree::Document::parse("<metalink xmlns='urn:ietf:params:xml:ns:metalink'><file name='radio.zim'><size>77108284</size></file></metalink>").unwrap();
        assert_eq!(super::metalink_size(&document).unwrap(), 77_108_284);
        assert_ne!(super::metalink_size(&document).unwrap(), 77_109_248);
    }
}
