pub mod server;
pub mod web;
pub mod workspace;

pub const MAX_FILE: usize = 1024 * 1024;
pub const MAX_OUTPUT: usize = 24_000;
pub type Result<T> = std::result::Result<T, &'static str>;

pub fn truncate(text: &str, limit: usize) -> String {
    text.chars().take(limit).collect()
}
