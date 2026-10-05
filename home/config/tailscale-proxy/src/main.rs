use std::{env, fs, process::ExitCode, sync::Arc};
use tailscale_open_proxy::{proxy_connection, validate_key};
use tokio::{net::TcpListener, sync::Semaphore};

struct Config {
    listen: (String, u16),
    upstream: (String, u16),
    key_file: String,
}

fn config() -> Result<Option<Config>, &'static str> {
    let mut listen_host = "0.0.0.0".to_string();
    let mut upstream_host = "127.0.0.1".to_string();
    let mut listen_port = None;
    let mut upstream_port = None;
    let mut key_file = None;
    let mut args = env::args().skip(1);
    while let Some(flag) = args.next() {
        if flag == "--help" || flag == "-h" {
            println!(
                "tailscale-open-proxy --listen-port PORT --upstream-port PORT --key-file PATH [--listen-host HOST] [--upstream-host HOST]"
            );
            return Ok(None);
        }
        let value = args.next().ok_or("missing option value")?;
        match flag.as_str() {
            "--listen-host" => listen_host = value,
            "--upstream-host" => upstream_host = value,
            "--listen-port" => {
                listen_port = Some(value.parse().map_err(|_| "invalid listen port")?)
            }
            "--upstream-port" => {
                upstream_port = Some(value.parse().map_err(|_| "invalid upstream port")?)
            }
            "--key-file" => key_file = Some(value),
            _ => return Err("unknown option"),
        }
    }
    Ok(Some(Config {
        listen: (listen_host, listen_port.ok_or("missing listen port")?),
        upstream: (upstream_host, upstream_port.ok_or("missing upstream port")?),
        key_file: key_file.ok_or("missing key file")?,
    }))
}

async fn serve(config: Config) -> Result<(), &'static str> {
    let key = fs::read_to_string(config.key_file).map_err(|_| "could not read API key")?;
    let key = Arc::new(
        validate_key(&key)
            .map_err(|_| "invalid API key")?
            .to_string(),
    );
    let listener = TcpListener::bind((config.listen.0.as_str(), config.listen.1))
        .await
        .map_err(|_| "could not bind listener")?;
    let upstream = Arc::new(config.upstream);
    let slots = Arc::new(Semaphore::new(512));
    loop {
        let permit = slots
            .clone()
            .acquire_owned()
            .await
            .map_err(|_| "listener stopped")?;
        let (client, peer) = listener
            .accept()
            .await
            .map_err(|_| "could not accept connection")?;
        let key = key.clone();
        let upstream = upstream.clone();
        tokio::spawn(async move {
            let _permit = permit;
            proxy_connection(client, &upstream, &key, peer.ip()).await;
        });
    }
}

#[tokio::main]
async fn main() -> ExitCode {
    let result = match config() {
        Ok(Some(config)) => serve(config).await,
        Ok(None) => return ExitCode::SUCCESS,
        Err(error) => Err(error),
    };
    if let Err(error) = result {
        // Never print supplied arguments, credential contents or request data.
        eprintln!("tailscale-open-proxy: {error}");
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}
