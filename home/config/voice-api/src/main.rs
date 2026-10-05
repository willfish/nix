mod backend;
mod config;

use backend::{ApiError, ApiResult, Backend, LIMIT};
use config::Config;
use http_body_util::{BodyExt, Full, Limited};
use hyper::{
    Request, Response,
    body::{Bytes, Incoming},
    server::conn::http1,
    service::service_fn,
};
use hyper_util::rt::{TokioIo, TokioTimer};
use serde_json::json;
use std::{convert::Infallible, sync::Arc, time::Duration};
use tokio::{
    net::TcpListener,
    signal::unix::{SignalKind, signal},
    sync::{Mutex, oneshot},
    task::JoinSet,
    time::{Instant, interval, timeout},
};

type Reply = Response<Full<Bytes>>;
type Shared = Arc<Mutex<Backend>>;

fn reply(status: u16, kind: &str, body: Vec<u8>) -> Reply {
    let mut response = Response::new(Full::new(Bytes::from(body)));
    *response.status_mut() = hyper::StatusCode::from_u16(status).expect("static status");
    response
        .headers_mut()
        .insert("Content-Type", kind.parse().expect("static content type"));
    response
        .headers_mut()
        .insert("Connection", "close".parse().unwrap());
    response
}

fn error(error: ApiError) -> Reply {
    let data = if error.1 == "INVALID_UPSTREAM_RESPONSE" {
        json!({"err_code": error.1})
    } else {
        json!({"err_code": "VOICE_ERROR", "err_msg": error.1})
    };
    reply(error.0, "application/json", data.to_string().into_bytes())
}

async fn post(req: Request<Incoming>, backend: Shared, queue_timeout: Duration) -> ApiResult {
    if req.headers().contains_key("Origin") || req.headers().contains_key("Transfer-Encoding") {
        return Err(ApiError(
            403,
            "Browser and transfer-encoded requests are not supported",
        ));
    }
    if req.uri().scheme().is_some() || !matches!(req.uri().path(), "/v1/listen" | "/v1/speak") {
        return Err(ApiError(404, "Unknown endpoint"));
    }
    let lengths: Vec<_> = req.headers().get_all("Content-Length").iter().collect();
    if lengths.len() != 1
        || lengths[0].is_empty()
        || !lengths[0].as_bytes().iter().all(u8::is_ascii_digit)
    {
        return Err(ApiError(411, "A single Content-Length is required"));
    }
    let length = lengths[0]
        .to_str()
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|&n| n > 0 && n <= LIMIT)
        .ok_or(ApiError(413, "Request exceeds size limit or is empty"))?;
    let listen = req.uri().path() == "/v1/listen";
    let query = form_urlencoded::parse(req.uri().query().unwrap_or_default().as_bytes())
        .filter(|(_, v)| !v.is_empty())
        .map(|(k, v)| (k.into_owned(), v.into_owned()))
        .collect();
    let context = req
        .headers()
        .get("X-Voice-Context-Words")
        .map(|v| v.to_str().unwrap_or("invalid").to_owned());
    let mut engine = timeout(queue_timeout, backend.lock())
        .await
        .map_err(|_| ApiError(429, "Local inference is busy"))?;
    let result = async {
        let body = timeout(
            Duration::from_secs(10),
            Limited::new(req.into_body(), LIMIT).collect(),
        )
        .await
        .map_err(|_| ApiError(400, "Incomplete request body"))?
        .map_err(|_| ApiError(400, "Incomplete request body"))?
        .to_bytes();
        if body.len() != length {
            return Err(ApiError(400, "Incomplete request body"));
        }
        if listen {
            engine.listen(body.to_vec(), &query).await
        } else {
            engine
                .speak(body.to_vec(), &query, context.as_deref())
                .await
        }
    }
    .await;
    engine.last_used = Instant::now();
    result
}

async fn handle(
    req: Request<Incoming>,
    backend: Shared,
    queue_timeout: Duration,
) -> Result<Reply, Infallible> {
    let response = match req.method().as_str() {
        "GET" => {
            if *req.uri() == "/health" {
                reply(200, "application/json", b"{\"status\":\"ready\"}".to_vec())
            } else {
                reply(404, "application/json", b"{}".to_vec())
            }
        }
        "POST" => match post(req, backend, queue_timeout).await {
            Ok((kind, body)) => reply(200, kind, body),
            Err(err) => error(err),
        },
        _ => error(ApiError(405, "Unsupported method")),
    };
    Ok(response)
}

async fn serve(config: Config) -> Result<(), Box<dyn std::error::Error>> {
    let queue_timeout = Duration::from_secs_f64(config.queue_timeout);
    let idle_timeout = Duration::from_secs_f64(config.idle_timeout);
    let mut term = signal(SignalKind::terminate())?;
    let mut interrupt = signal(SignalKind::interrupt())?;
    let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, config.port)).await?;
    let backend = Arc::new(Mutex::new(Backend::new(config)));
    let mut connections = JoinSet::new();
    let (stop_sweeper, mut stopping) = oneshot::channel();
    let idle_backend = Arc::clone(&backend);
    let sweeper = tokio::spawn(async move {
        let mut sweep = interval(Duration::from_secs(1));
        loop {
            tokio::select! {
                _ = &mut stopping => break,
                _ = sweep.tick() => {
                    if let Ok(mut engine) = idle_backend.try_lock()
                        && engine.last_used.elapsed() >= idle_timeout {
                        engine.stop_engines().await;
                    }
                },
            }
        }
    });
    loop {
        tokio::select! {
            _ = term.recv() => break,
            _ = interrupt.recv() => break,
            _ = connections.join_next(), if !connections.is_empty() => {},
            accepted = listener.accept() => {
                let (stream, _) = match accepted { Ok(value) => value, Err(_) => continue };
                // Bound idle/header-only clients as well as queued inference.
                if connections.len() >= 128 { continue; }
                let backend = Arc::clone(&backend);
                connections.spawn(async move {
                    // A client that stops reading must not hold shutdown open.
                    // Do not use a read-idle timer during inference: the engine
                    // may legitimately take longer than ten seconds to reply.
                    let mut stream = tokio_io_timeout::TimeoutStream::new(stream);
                    stream.set_write_timeout(Some(Duration::from_secs(10)));
                    let _ = http1::Builder::new().keep_alive(false).max_headers(64)
                        .timer(TokioTimer::new()).header_read_timeout(Duration::from_secs(10))
                        .serve_connection(TokioIo::new(Box::pin(stream)), service_fn(move |req| handle(req, Arc::clone(&backend), queue_timeout)))
                        .await;
                });
            },
        }
    }
    drop(listener);
    let _ = stop_sweeper.send(());
    // Drain accepted requests before releasing engines, including on SIGTERM.
    while connections.join_next().await.is_some() {}
    let _ = sweeper.await;
    backend.lock().await.stop_engines().await;
    Ok(())
}

#[tokio::main]
async fn main() -> std::process::ExitCode {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() != 2 || args[0] != "--config" {
        eprintln!("usage: pi-voice-api --config FILE");
        return std::process::ExitCode::FAILURE;
    }
    let config = std::fs::read(&args[1])
        .ok()
        .and_then(|data| serde_json::from_slice::<Config>(&data).ok())
        .filter(|config| config.validate().is_ok());
    let Some(config) = config else {
        eprintln!("pi-voice-api: invalid configuration");
        return std::process::ExitCode::FAILURE;
    };
    if serve(config).await.is_err() {
        // Never print config values, URLs, transcripts or engine response bodies.
        eprintln!("pi-voice-api: could not start local service");
        return std::process::ExitCode::FAILURE;
    }
    std::process::ExitCode::SUCCESS
}
