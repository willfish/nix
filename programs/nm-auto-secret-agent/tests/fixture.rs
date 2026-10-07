use std::{env, path::PathBuf};
fn main() {
    let args: Vec<_> = env::args().collect();
    let status = match args.get(1).map(String::as_str) {
        Some("run") if args.len() == 3 => {
            nm_auto_secret_agent::fixture_run(PathBuf::from(&args[2]))
        }
        Some("lookup") if args.len() == 6 => {
            match nm_auto_secret_agent::fixture_secret(
                &PathBuf::from(&args[2]),
                &args[3],
                &args[4],
                &args[5],
            ) {
                Ok(value) => {
                    println!("{value}");
                    0
                }
                Err(message) => {
                    eprintln!("{message}");
                    1
                }
            }
        }
        _ => 2,
    };
    std::process::exit(status);
}
