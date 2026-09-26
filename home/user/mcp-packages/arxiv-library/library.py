"""Disk-backed arXiv full-text library. No corpus-sized in-memory state."""

import hashlib
import json
from pathlib import Path
import re
import sqlite3
import time
from urllib.parse import quote
import zlib

SCHEMA = """
CREATE TABLE IF NOT EXISTS papers (
 id INTEGER PRIMARY KEY, paper_id TEXT NOT NULL UNIQUE,
 title TEXT NOT NULL, abstract TEXT NOT NULL, category TEXT NOT NULL,
 license TEXT, sha256 TEXT NOT NULL, text_bytes INTEGER NOT NULL,
 text_chars INTEGER NOT NULL, body BLOB NOT NULL
);
CREATE VIRTUAL TABLE IF NOT EXISTS search USING fts5(
 title, abstract, body, content='', tokenize='porter unicode61'
);
CREATE TABLE IF NOT EXISTS imports (
 shard TEXT PRIMARY KEY, rows_done INTEGER NOT NULL,
 complete INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
"""


def connect(path, write=False):
    path = Path(path)
    if write:
        path.parent.mkdir(parents=True, exist_ok=True)
        db = sqlite3.connect(path, timeout=30)
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA synchronous=FULL")
        db.execute("PRAGMA cache_size=-65536")
        db.executescript(SCHEMA)
    else:
        db = sqlite3.connect(
            path.resolve().as_uri() + "?mode=ro", uri=True, timeout=30
        )
        db.execute("PRAGMA query_only=ON")
    db.row_factory = sqlite3.Row
    return db


def add_paper(db, row):
    text = row["text"]
    raw = text.encode("utf-8")
    sha = hashlib.sha256(raw).hexdigest()
    if row.get("text_sha256") and sha != row["text_sha256"]:
        raise ValueError(f'Text checksum mismatch: {row["paper_id"]}')
    prior = db.execute(
        "SELECT sha256 FROM papers WHERE paper_id=?", (row["paper_id"],)
    ).fetchone()
    if prior:
        if prior["sha256"] != sha:
            raise ValueError(f'Conflicting paper: {row["paper_id"]}')
        return False
    title, abstract = row.get("title") or "", row.get("abstract") or ""
    cursor = db.execute(
        """INSERT INTO papers
      (paper_id,title,abstract,category,license,sha256,
       text_bytes,text_chars,body)
      VALUES (?,?,?,?,?,?,?,?,?)""",
        (
            row["paper_id"],
            title,
            abstract,
            row.get("primary_category") or "",
            row.get("license"),
            sha,
            len(raw),
            len(text),
            zlib.compress(raw, level=3),
        ),
    )
    db.execute(
        "INSERT INTO search(rowid,title,abstract,body) VALUES (?,?,?,?)",
        (cursor.lastrowid, title, abstract, text),
    )
    return True


def metadata(row):
    return {
        k: row[k]
        for k in (
            "paper_id",
            "title",
            "abstract",
            "category",
            "license",
            "sha256",
            "text_chars",
            "text_bytes",
        )
    } | {
        "url": "https://arxiv.org/abs/" + quote(row["paper_id"], safe="/"),
        "format": "latex",
    }


def get_paper(db, paper_id, offset=0, length=12000):
    if (
        not isinstance(offset, int)
        or offset < 0
        or not isinstance(length, int)
        or not 1 <= length <= 50000
    ):
        raise ValueError(
            "offset must be nonnegative; length must be 1..50000 characters"
        )
    row = db.execute(
        "SELECT * FROM papers WHERE paper_id=?", (paper_id,)
    ).fetchone()
    if row is None:
        raise KeyError(paper_id)
    text = zlib.decompress(row["body"]).decode("utf-8")
    end = min(len(text), offset + length)
    return metadata(row) | {
        "text": text[offset:end],
        "offset": offset,
        "next_offset": end if end < len(text) else None,
    }


def search_papers(db, query, limit=10, category=None):
    if not isinstance(limit, int) or not 1 <= limit <= 50:
        raise ValueError("limit must be 1..50")
    if not isinstance(query, str) or len(query) > 1000:
        raise ValueError("query must be a string of at most 1000 characters")
    terms = re.findall(r"\w+", query, re.UNICODE)
    if not terms or len(terms) > 32:
        raise ValueError("query must contain 1..32 words")
    # Literal AND search, not caller-supplied FTS syntax or SQL.
    expression = " AND ".join('"' + term + '"' for term in terms)
    clause, params = (
        (" AND p.category=?", [category]) if category else ("", [])
    )
    deadline = time.monotonic() + 30
    db.set_progress_handler(lambda: int(time.monotonic() > deadline), 10000)
    try:
        # Rank IDs first. Selecting compressed bodies before LIMIT copies every
        # match into the sort B-tree and makes common-term queries disk-bound.
        if category:
            hits = db.execute(
                """SELECT p.id, bm25(search,5.0,2.0,1.0) AS score
              FROM search JOIN papers p ON p.id=search.rowid
              WHERE search MATCH ?"""
                + clause
                + " ORDER BY score, p.id LIMIT ?",
                [expression, *params, limit],
            ).fetchall()
        else:
            hits = db.execute(
                """SELECT rowid AS id, bm25(search,5.0,2.0,1.0) AS score
              FROM search WHERE search MATCH ? ORDER BY score, rowid LIMIT ?""",
                [expression, limit],
            ).fetchall()
        rows = [
            dict(
                db.execute(
                    "SELECT * FROM papers WHERE id=?", (hit["id"],)
                ).fetchone()
            )
            | {"score": hit["score"]}
            for hit in hits
        ]
    finally:
        db.set_progress_handler(None, 0)
    results = []
    for row in rows:
        text = zlib.decompress(row["body"]).decode("utf-8")
        match = re.search(re.escape(terms[0]), text, flags=re.IGNORECASE)
        start = max(0, (match.start() if match else 0) - 160)
        results.append(
            metadata(row)
            | {
                "bm25": -row["score"],
                "excerpt": text[start : start + 800],
                "excerpt_offset": start,
            }
        )
    return results


def status(db):
    return {
        "papers": db.execute("SELECT count(*) FROM papers").fetchone()[0],
        "imports": [
            dict(r) for r in db.execute("SELECT * FROM imports ORDER BY shard")
        ],
        "settings": {
            r["key"]: r["value"] for r in db.execute("SELECT * FROM settings")
        },
    }
