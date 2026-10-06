mod client;
mod portal;
mod runtime;
#[cfg(test)]
mod tests;
use runtime::{Local, Runtime};
use serde_json::{Value, json};
use std::io::{self, BufRead, Write};
#[derive(Debug)]
pub struct Error(String);
impl Error {
    pub fn new(text: impl Into<String>) -> Self {
        Self(text.into())
    }
}
pub type Result<T> = std::result::Result<T, Error>;
fn arguments<'a>(value: &'a Value, key: &str) -> Result<Option<&'a str>> {
    match value.get(key) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(s)) => Ok(Some(s)),
        _ => Err(Error::new(format!("{key} must be a string"))),
    }
}
fn call(
    name: &str,
    args: &Value,
    client: &mut dyn portal::Backend,
    rt: &mut dyn Runtime,
) -> Result<String> {
    match name {
        "status" => {
            let status = portal::status(client, rt)?;
            Ok(if status["logged_in"] == true {
                format!("Signed in until {}.", portal::text(&status, "expires_at"))
            } else {
                "Not signed in.".into()
            })
        }
        "login" => {
            if portal::status(client, rt)?["logged_in"] == true {
                Ok("Already signed in.".into())
            } else {
                Ok(if portal::login(client, rt)? {
                    "Signed in. Background tab closed."
                } else {
                    "Already signed in."
                }
                .into())
            }
        }
        "list_accounts" => {
            let accounts = portal::accounts(client, rt)?;
            Ok(if accounts.is_empty() {
                "No accounts.".into()
            } else {
                accounts
                    .iter()
                    .map(|a| format!("{} {}", a.name, a.id))
                    .collect::<Vec<_>>()
                    .join("\n")
            })
        }
        "list_roles" => portal::roles(
            client,
            rt,
            arguments(args, "account_id")?,
            arguments(args, "account_name")?,
        ),
        "export_credentials" => portal::export(
            client,
            rt,
            arguments(args, "role_name")?.unwrap_or(""),
            arguments(args, "account_id")?,
            arguments(args, "account_name")?,
            arguments(args, "region")?
                .filter(|s| !s.is_empty())
                .unwrap_or(portal::REGION),
        )?
        .display(),
        _ => Err(Error::new("unknown tool")),
    }
}
fn tool_result(text: &str, error: bool) -> Value {
    json!({"content":[{"type":"text","text":format!("{}\n",text.trim_end())}],"isError":error})
}
fn handle(message: Value) -> Option<Value> {
    let id = message.get("id")?;
    if id.is_null() {
        return None;
    }
    let definitions: Value =
        serde_json::from_str(include_str!("../tools.json")).expect("static tool definitions");
    let method = message
        .get("method")
        .and_then(Value::as_str)
        .unwrap_or("None");
    let result = match method {
        "initialize" => {
            json!({"protocolVersion":"2024-11-05","capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"aws-access-portal","version":"1"},"instructions":definitions["instructions"]})
        }
        "ping" => json!({}),
        "tools/list" => json!({"tools":definitions["tools"]}),
        "tools/call" => {
            let args = &message["params"]["arguments"];
            let name = message["params"]["name"].as_str().unwrap_or("");
            let mut rt = Local::default();
            let result =
                client::Client::new().and_then(|mut client| call(name, args, &mut client, &mut rt));
            match result {
                Ok(text) => tool_result(&text, false),
                Err(error) => tool_result(&rt.safe_error(&error.0), true),
            }
        }
        _ => {
            return Some(
                json!({"jsonrpc":"2.0","id":id,"error":{"code":-32603,"message":runtime::redact(&format!("unsupported method {method}"))}}),
            );
        }
    };
    Some(json!({"jsonrpc":"2.0","id":id,"result":result}))
}
fn main() {
    std::panic::set_hook(Box::new(|_| {
        eprintln!("AWS portal internal failure; secret values have not been logged.")
    }));
    let input = io::stdin();
    let mut output = io::stdout().lock();
    for line in input.lock().lines() {
        let Ok(line) = line else {
            break;
        };
        let Ok(message) = serde_json::from_str(&line) else {
            continue;
        };
        if let Some(reply) = handle(message) {
            if writeln!(output, "{reply}")
                .and_then(|_| output.flush())
                .is_err()
            {
                break;
            }
        }
    }
}
