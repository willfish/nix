use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use std::{
    io::Read,
    net::TcpListener,
    path::Path,
    process::{Child, Command, Stdio},
    time::{Duration, Instant},
};

struct Viewer(Child);

impl Drop for Viewer {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

pub fn parse_results(xml: &str, id: &str, limit: usize) -> Result<Value> {
    let document = roxmltree::Document::parse(xml).context("Invalid native Kiwix search XML")?;
    ensure!(
        document.root_element().has_tag_name("rss"),
        "Kiwix did not return search results"
    );
    let mut results = Vec::new();
    for item in document
        .descendants()
        .filter(|n| n.has_tag_name("item"))
        .take(limit)
    {
        let text = |name: &str| {
            item.children()
                .find(|n| n.has_tag_name(name))
                .map(|n| {
                    n.descendants()
                        .filter(|n| n.is_text())
                        .filter_map(|n| n.text())
                        .collect::<String>()
                })
                .unwrap_or_default()
        };
        let link = text("link");
        let path = link
            .strip_prefix("/content/")
            .and_then(|s| s.split_once('/'))
            .map(|(_, entry)| entry)
            .context("Kiwix result lacks an internal entry path")?;
        let locator = format!("zim:{}", percent_decode(path)?);
        let snippet = scraper::Html::parse_fragment(&text("description"))
            .root_element()
            .text()
            .collect::<Vec<_>>()
            .join(" ");
        results.push(json!({"source_id":id,"locator":locator,"title":text("title"),"snippet":snippet.chars().take(500).collect::<String>()}));
    }
    let total: usize = document
        .descendants()
        .find(|n| n.has_tag_name("totalResults"))
        .and_then(|n| n.text())
        .context("Kiwix omitted its match count")?
        .replace(',', "")
        .parse()?;
    Ok(json!({"total_matches":total,"truncated":total>results.len(),"results":results}))
}

fn percent_decode(input: &str) -> Result<String> {
    let mut bytes = Vec::new();
    let mut i = 0;
    while i < input.len() {
        if input.as_bytes()[i] == b'%' {
            let hex = input.get(i + 1..i + 3).context("Incomplete URL escape")?;
            bytes.push(u8::from_str_radix(hex, 16)?);
            i += 3;
        } else {
            bytes.push(input.as_bytes()[i]);
            i += 1;
        }
    }
    Ok(String::from_utf8(bytes)?)
}

pub fn search(path: &Path, id: &str, query: &str, limit: usize) -> Result<Value> {
    // The public CLI prints titles only. Native XML search preserves the entry path.
    // A bounded, per-call viewer listens on loopback only and dies with this process.
    let reservation = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0))?;
    let port = reservation.local_addr()?.port();
    drop(reservation);
    let mut viewer = Viewer(
        Command::new("kiwix-serve")
            .args(["--address=127.0.0.1", "--threads=1", "--blockexternal"])
            .arg(format!("--attachToProcess={}", std::process::id()))
            .arg(format!("--port={port}"))
            .arg(path)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()?,
    );
    let client = reqwest::blocking::Client::builder()
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_millis(200))
        .timeout(Duration::from_secs(6))
        .build()?;
    let mut url = url::Url::parse(&format!("http://127.0.0.1:{port}/search"))?;
    url.query_pairs_mut()
        .append_pair("pattern", query)
        .append_pair("format", "xml")
        .append_pair("pageLength", &limit.to_string());
    let start = Instant::now();
    let mut response = loop {
        ensure!(
            viewer.0.try_wait()?.is_none(),
            "Private Kiwix search process failed to start"
        );
        match client.get(url.clone()).send() {
            Ok(response) => break response.error_for_status()?,
            Err(error) if error.is_connect() && start.elapsed() < Duration::from_secs(2) => {
                std::thread::sleep(Duration::from_millis(25))
            }
            Err(error) => return Err(error.into()),
        }
    };
    let mut xml = String::new();
    (&mut response)
        .take(256 * 1024 + 1)
        .read_to_string(&mut xml)?;
    ensure!(
        xml.len() <= 256 * 1024,
        "Native archive result exceeded its output bound"
    );
    parse_results(&xml, id, limit)
}

#[cfg(test)]
mod tests {
    #[test]
    fn native_xml_keeps_decoded_entry_locations_and_plain_snippets() {
        let xml = "<rss><channel><totalResults>1,300</totalResults><item><title>A &amp; B</title><link>/content/archive/questions/tagged/radio_page%3D2</link><description>...&lt;b&gt;radio&lt;/b&gt; &amp; repair</description></item></channel></rss>";
        let hits = super::parse_results(xml, "source", 5).unwrap();
        assert_eq!(
            hits["results"][0]["locator"],
            "zim:questions/tagged/radio_page=2"
        );
        assert_eq!(hits["results"][0]["title"], "A & B");
        assert_eq!(hits["total_matches"], 1300);
        assert_eq!(hits["truncated"], true);
        assert!(
            !hits["results"][0]["snippet"]
                .as_str()
                .unwrap()
                .contains("<b>")
        );
        let mixed = xml.replace("&lt;b&gt;radio&lt;/b&gt;", "<b>radio</b>");
        assert!(
            super::parse_results(&mixed, "source", 5).unwrap()["results"][0]["snippet"]
                .as_str()
                .unwrap()
                .contains("radio")
        );
    }
}
