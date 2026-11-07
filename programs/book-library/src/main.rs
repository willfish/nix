use anyhow::{Context, Result, bail};
use book_library::{acquire, db, extract, mcp, sources};
use clap::{Parser, Subcommand};
use serde_json::json;
use std::path::PathBuf;

#[derive(Parser)]
#[command(
    version,
    about = "Offline books and references: bounded retrieval, explicit maintenance"
)]
struct Args {
    #[arg(
        long,
        global = true,
        env = "BOOK_LIBRARY_ROOT",
        default_value = "/srv/media/knowledge"
    )]
    root: PathBuf,
    #[command(subcommand)]
    command: Operation,
}

#[derive(Subcommand)]
enum Operation {
    /// Resolve the licensed source policy and write a frozen acquisition manifest.
    Lock {
        #[arg(long)]
        policy: Option<PathBuf>,
        #[arg(long, default_value_t = 0)]
        oapen_limit: usize,
        #[arg(long)]
        allow_non_zfs: bool,
    },
    /// Resolve current metadata in a NEW staging root and compare with the live lock; never download originals.
    CheckUpdates {
        destination: PathBuf,
    },
    /// Resume the frozen manifest's downloads, checking space before each object.
    Acquire {
        #[arg(long)]
        limit: Option<usize>,
        #[arg(long)]
        allow_non_zfs: bool,
    },
    /// Resume extraction/indexing of verified originals and native ZIM registration.
    Ingest {
        #[arg(long)]
        limit: Option<usize>,
    },
    /// Read-only BM25/native archive search.
    Search {
        query: String,
        #[arg(long)]
        category: Option<String>,
        #[arg(long)]
        provider: Option<String>,
        #[arg(long)]
        source_id: Option<String>,
        #[arg(long, default_value_t = 5)]
        limit: usize,
    },
    Read {
        source_id: String,
        locator: String,
        #[arg(long, default_value_t = 8000)]
        max_chars: usize,
        #[arg(long, default_value_t = 0)]
        offset: usize,
    },
    Grep {
        pattern: String,
        #[arg(long, required = true)]
        source_ids: Vec<String>,
        #[arg(long)]
        locator: Option<String>,
        #[arg(long)]
        regex: bool,
        #[arg(long, default_value_t = 20)]
        limit: usize,
    },
    SourceInfo {
        source_id: String,
    },
    Status,
    /// Independently verify original hashes, source coverage and SQLite/FTS integrity.
    Audit {
        #[arg(long)]
        quick: bool,
    },
    /// Make a consistent essentials copy without altering the live database.
    Snapshot {
        destination: PathBuf,
        #[arg(long, default_value_t = 56)]
        max_gib: u64,
    },
    /// Export one preserved original to a new local file (not an MCP operation).
    Export {
        source_id: String,
        destination: PathBuf,
    },
    /// Serve five read-only MCP tools over stdio.
    Mcp,
}

fn run(args: Args) -> Result<()> {
    let result = match args.command {
        Operation::Lock {
            policy,
            oapen_limit,
            allow_non_zfs,
        } => sources::lock(&args.root, policy.as_deref(), oapen_limit, allow_non_zfs)?,
        Operation::CheckUpdates { destination } => {
            sources::check_updates(&args.root, &destination)?
        }
        Operation::Acquire {
            limit,
            allow_non_zfs,
        } => acquire::run(&args.root, limit, allow_non_zfs)?,
        Operation::Ingest { limit } => extract::ingest(&args.root, limit)?,
        Operation::Search {
            query,
            category,
            provider,
            source_id,
            limit,
        } => db::Library::open(&args.root)?.search_scope(
            &query,
            category.as_deref(),
            provider.as_deref(),
            source_id.as_deref(),
            limit,
        )?,
        Operation::Read {
            source_id,
            locator,
            max_chars,
            offset,
        } => db::Library::open(&args.root)?.read(&source_id, &locator, offset, max_chars)?,
        Operation::Grep {
            pattern,
            source_ids,
            locator,
            regex,
            limit,
        } => db::Library::open(&args.root)?.grep_at(
            &pattern,
            &source_ids,
            locator.as_deref(),
            regex,
            limit,
        )?,
        Operation::SourceInfo { source_id } => {
            db::Library::open(&args.root)?.source_info(&source_id)?
        }
        Operation::Status => db::Library::open(&args.root)?.status()?,
        Operation::Audit { quick } => {
            let report = db::audit(&args.root, !quick)?;
            println!("{}", serde_json::to_string_pretty(&report)?);
            if report["complete"] != true {
                bail!("Corpus audit did not establish completeness");
            }
            return Ok(());
        }
        Operation::Snapshot {
            destination,
            max_gib,
        } => db::snapshot(&args.root, &destination, max_gib)?,
        Operation::Export {
            source_id,
            destination,
        } => {
            let library = db::Library::open(&args.root)?;
            let path = library.object(&source_id)?;
            let mut output = std::fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&destination)
                .context("Destination must be a new file")?;
            std::io::copy(&mut std::fs::File::open(path)?, &mut output)?;
            json!({"exported":destination})
        }
        Operation::Mcp => {
            let runtime = tokio::runtime::Runtime::new()?;
            runtime.block_on(mcp::serve(args.root))?;
            return Ok(());
        }
    };
    println!("{}", serde_json::to_string_pretty(&result)?);
    Ok(())
}

fn main() -> std::process::ExitCode {
    match run(Args::parse()) {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("book-library: {error:#}");
            std::process::ExitCode::FAILURE
        }
    }
}
