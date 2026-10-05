use crate::{MAX_FILE, MAX_OUTPUT, Result, truncate};
use cap_std::{
    ambient_authority,
    fs::{Dir, OpenOptions, OpenOptionsExt},
};
use serde_json::{Value, json};
use std::{
    fs,
    io::{Read, Write},
    path::{Component, Path, PathBuf},
};
use unicode_casefold::UnicodeCaseFold;
use walkdir::WalkDir;

const PRIVATE_NAMES: &[&str] = &[
    ".git",
    ".ssh",
    ".aws",
    ".gnupg",
    "secrets",
    "node_modules",
    ".venv",
];

pub fn sensitive(path: &Path) -> bool {
    path.components().any(|part| {
        let name = part.as_os_str().to_string_lossy();
        PRIVATE_NAMES.contains(&name.as_ref())
            || name.starts_with(".env")
            || name.starts_with("id_rsa")
            || name.starts_with("id_ed25519")
            || name.ends_with(".pem")
            || name.ends_with(".key")
    })
}

// Like Path.resolve(strict=False), including symlink-before-.. semantics and
// paths whose final components do not exist yet.
fn canonical(path: &Path, links: usize) -> Result<PathBuf> {
    if links > 40 {
        return Err("Too many symbolic links");
    }
    let mut result = PathBuf::new();
    for part in path.components() {
        match part {
            Component::RootDir => result.push("/"),
            Component::CurDir => {}
            Component::ParentDir => {
                result.pop();
            }
            Component::Normal(name) => {
                result.push(name);
                match fs::symlink_metadata(&result) {
                    Ok(metadata) if metadata.file_type().is_symlink() => {
                        let target =
                            fs::read_link(&result).map_err(|_| "Could not resolve path")?;
                        result.pop();
                        result = canonical(&result.join(target), links + 1)?;
                    }
                    Ok(_) => {}
                    Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                    Err(_) => return Err("Could not resolve path"),
                }
            }
            _ => return Err("Unsupported filesystem path"),
        }
    }
    Ok(result)
}

pub fn lines(text: &str) -> Vec<&str> {
    let mut result = Vec::new();
    let mut start = 0;
    let mut chars = text.char_indices().peekable();
    while let Some((index, ch)) = chars.next() {
        if matches!(
            ch,
            '\n' | '\r' | '\u{b}' | '\u{c}' | '\u{1c}'
                ..='\u{1e}' | '\u{85}' | '\u{2028}' | '\u{2029}'
        ) {
            let mut end = index + ch.len_utf8();
            if ch == '\r' && chars.peek().is_some_and(|(_, next)| *next == '\n') {
                chars.next();
                end += 1;
            }
            result.push(&text[start..end]);
            start = end;
        }
    }
    if start < text.len() {
        result.push(&text[start..]);
    }
    result
}

pub struct Workspace {
    pub read_roots: Vec<PathBuf>,
    pub write_root: PathBuf,
    home: PathBuf,
}

impl Workspace {
    pub fn new(read_roots: Vec<PathBuf>, write_root: PathBuf, home: PathBuf) -> Result<Self> {
        let cwd = std::env::current_dir().map_err(|_| "Could not resolve filesystem scope")?;
        let write_root = canonical(&cwd.join(write_root), 0)?;
        let mut read_roots = read_roots
            .into_iter()
            .map(|root| canonical(&cwd.join(root), 0))
            .collect::<Result<Vec<_>>>()?;
        read_roots.push(write_root.clone());
        Ok(Self {
            read_roots,
            write_root,
            home,
        })
    }

    pub fn scope(&self) -> Value {
        json!({"read": self.read_roots, "write": self.write_root})
    }

    pub fn resolve(&self, path: &str, writing: bool) -> Result<PathBuf> {
        let path = if path == "~" {
            self.home.clone()
        } else if let Some(path) = path.strip_prefix("~/") {
            self.home.join(path)
        } else {
            PathBuf::from(path)
        };
        let path = self.write_root.join(path);
        if sensitive(&path) {
            return Err("Credential files and private directories are excluded");
        }
        let target = canonical(&path, 0)?;
        let permitted = if writing {
            target.starts_with(&self.write_root)
        } else {
            self.read_roots.iter().any(|root| target.starts_with(root))
        };
        if sensitive(&target) || !permitted {
            return Err("Path is outside the permitted filesystem scope");
        }
        Ok(target)
    }

    fn directory(&self, target: &Path, writing: bool) -> Result<(Dir, PathBuf)> {
        let root = if writing {
            &self.write_root
        } else {
            self.read_roots
                .iter()
                .find(|root| target.starts_with(root))
                .ok_or("Path is outside the permitted filesystem scope")?
        };
        let dir = Dir::open_ambient_dir(root, ambient_authority())
            .map_err(|_| "Could not open filesystem scope")?;
        let relative = target
            .strip_prefix(root)
            .map_err(|_| "Path is outside the permitted filesystem scope")?;
        Ok((
            dir,
            if relative.as_os_str().is_empty() {
                PathBuf::from(".")
            } else {
                relative.to_path_buf()
            },
        ))
    }

    fn text(&self, target: &Path) -> Result<String> {
        let (dir, path) = self.directory(target, false)?;
        let file = dir.open(path).map_err(|_| "Could not read file")?;
        if !file
            .metadata()
            .map_err(|_| "Could not inspect file")?
            .is_file()
        {
            return Err("Expected a text file");
        }
        let mut bytes = Vec::new();
        file.take((MAX_FILE + 1) as u64)
            .read_to_end(&mut bytes)
            .map_err(|_| "Could not read file")?;
        if bytes.len() > MAX_FILE {
            return Err("File exceeds the 1 MiB text-file limit");
        }
        String::from_utf8(bytes).map_err(|_| "Expected a UTF-8 text file")
    }

    pub fn read(&self, path: &str, start: usize, end: usize) -> Result<String> {
        if start == 0 || end < start || end - start >= 500 {
            return Err("Request 1 to 500 lines, using 1-based line numbers");
        }
        let text = self.text(&self.resolve(path, false)?)?;
        // Python text-file reads normalize CRLF and CR to LF before splitlines.
        let text = text.replace("\r\n", "\n").replace('\r', "\n");
        Ok(truncate(
            &lines(&text)
                .into_iter()
                .skip(start - 1)
                .take(end - start + 1)
                .collect::<String>(),
            MAX_OUTPUT,
        ))
    }

    pub fn list(&self, path: &str) -> Result<Value> {
        let root = self.resolve(path, false)?;
        let (dir, relative) = self.directory(&root, false)?;
        let mut children = dir
            .read_dir(relative)
            .map_err(|_| "Could not list directory")?
            .map(|child| {
                child
                    .map(|child| root.join(child.file_name()))
                    .map_err(|_| "Could not list directory")
            })
            .collect::<Result<Vec<_>>>()?;
        children.sort();
        let mut result = Vec::new();
        for child in children {
            let Ok(target) = self.resolve(&child.to_string_lossy(), false) else {
                continue;
            };
            result.push(json!({"path": child, "directory": target.is_dir()}));
            if result.len() == 100 {
                break;
            }
        }
        Ok(result.into())
    }

    pub fn search(&self, query: &str, path: &str) -> Result<Value> {
        if query.trim().is_empty() {
            return Err("Supply a nonempty search phrase");
        }
        let root = self.resolve(path, false)?;
        let mut result = Vec::new();
        if !root.is_dir() {
            return Ok(result.into());
        }
        let query: String = query.case_fold().collect();
        let mut visited = 0;
        for entry in WalkDir::new(&root)
            .follow_links(false)
            .into_iter()
            .filter_entry(|entry| !entry.file_type().is_dir() || !sensitive(entry.path()))
        {
            let Ok(entry) = entry else {
                continue;
            };
            if entry.depth() == 0
                || entry.file_type().is_dir()
                || (entry.file_type().is_symlink() && entry.path().is_dir())
            {
                continue;
            }
            visited += 1;
            if visited > 2000 {
                break;
            }
            let Ok(target) = self.resolve(&entry.path().to_string_lossy(), false) else {
                continue;
            };
            let Ok(text) = self.text(&target) else {
                continue;
            };
            for (index, line) in lines(&text).into_iter().enumerate() {
                let line = line.trim_end_matches([
                    '\n', '\r', '\u{b}', '\u{c}', '\u{1c}', '\u{1d}', '\u{1e}', '\u{85}',
                    '\u{2028}', '\u{2029}',
                ]);
                if line.case_fold().collect::<String>().contains(&query) {
                    result.push(
                        json!({"path": target, "line": index + 1, "text": truncate(line, 300)}),
                    );
                    if result.len() == 50 {
                        return Ok(result.into());
                    }
                }
            }
        }
        Ok(result.into())
    }

    pub fn write(&self, path: &str, content: &str, overwrite: bool) -> Result<Value> {
        let target = self.resolve(path, true)?;
        if content.len() > MAX_FILE {
            return Err("Content exceeds the 1 MiB limit");
        }
        // Bootstrap the declared scope, not a caller-supplied path, before
        // opening the capability used to create its descendants.
        fs::create_dir_all(&self.write_root).map_err(|_| "Could not create workspace directory")?;
        let (dir, path) = self.directory(&target, true)?;
        dir.create_dir_all(path.parent().unwrap_or(Path::new(".")))
            .map_err(|_| "Could not create workspace directory")?;
        let mut options = OpenOptions::new();
        options
            .write(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW);
        if overwrite {
            options.create(true).truncate(true);
        } else {
            options.create_new(true);
        }
        let mut file = dir.open_with(path, &options).map_err(|error| {
            if error.kind() == std::io::ErrorKind::AlreadyExists {
                "File exists; use overwrite=true to replace it"
            } else {
                "Could not write workspace file"
            }
        })?;
        file.write_all(content.as_bytes())
            .map_err(|_| "Could not write workspace file")?;
        Ok(json!({"path": target, "bytes": content.len()}))
    }
}
