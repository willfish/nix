use local_assistant_tools::{Result, server::Assistant};
use std::{env, process::ExitCode};

async fn run() -> Result<()> {
    let mut port = 8082;
    let mut args = env::args().skip(1);
    if let Some(arg) = args.next() {
        if arg == "--help" || arg == "-h" {
            println!(
                "local-assistant-tools [--port PORT]\nFilesystem scope and allowed origins come from LOCAL_ASSISTANT_* environment variables."
            );
            return Ok(());
        }
        if arg != "--port" {
            return Err("Unknown option");
        }
        port = args
            .next()
            .ok_or("Missing port")?
            .parse()
            .map_err(|_| "Invalid port")?;
        if args.next().is_some() {
            return Err("Unknown option");
        }
    }
    let (assistant, origins) = Assistant::from_env()?;
    let listener = tokio::net::TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, port))
        .await
        .map_err(|_| "Could not bind local assistant listener")?;
    axum::serve(listener, assistant.router(origins))
        .with_graceful_shutdown(async {
            let mut terminate =
                tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
                    .expect("SIGTERM handler");
            tokio::select! { _ = terminate.recv() => {}, _ = tokio::signal::ctrl_c() => {} }
        })
        .await
        .map_err(|_| "Local assistant server failed")
}

#[tokio::main]
async fn main() -> ExitCode {
    if let Err(error) = run().await {
        eprintln!("local-assistant-tools: {error}");
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}
