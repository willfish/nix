use local_assistant_tools::{MAX_FILE, MAX_OUTPUT, workspace::Workspace};
use std::{
    fs,
    os::unix::fs::{PermissionsExt, symlink},
    path::PathBuf,
};
use tempfile::TempDir;

struct Fixture {
    _dir: TempDir,
    repo: PathBuf,
    output: PathBuf,
    workspace: Workspace,
}
fn fixture() -> Fixture {
    let dir = tempfile::tempdir().unwrap();
    let repo = dir.path().join("repos");
    let output = dir.path().join("output");
    fs::create_dir(&repo).unwrap();
    fs::create_dir(&output).unwrap();
    fs::write(repo.join("README.md"), "A useful project\n").unwrap();
    let workspace =
        Workspace::new(vec![repo.clone()], output.clone(), dir.path().to_owned()).unwrap();
    Fixture {
        _dir: dir,
        repo,
        output,
        workspace,
    }
}

#[test]
fn reads_lists_and_searches_allowed_files() {
    let f = fixture();
    assert!(
        f.workspace
            .read(f.repo.join("README.md").to_str().unwrap(), 1, 200)
            .unwrap()
            .contains("useful")
    );
    let found = f
        .workspace
        .search("USEFUL", f.repo.to_str().unwrap())
        .unwrap();
    assert_eq!(found[0]["line"], 1);
    assert_eq!(found[0]["text"], "A useful project");
    assert_eq!(
        f.workspace.list(f.repo.to_str().unwrap()).unwrap()[0]["directory"],
        false
    );
    assert_eq!(f.workspace.scope()["read"].as_array().unwrap().len(), 2);
}

#[test]
fn first_write_bootstraps_a_workspace_that_does_not_yet_exist() {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().join("new/workspace");
    let workspace = Workspace::new(vec![], root.clone(), dir.path().to_owned()).unwrap();
    assert!(!root.exists());
    assert!(workspace.write("../outside", "no", false).is_err());
    assert!(!root.exists());
    workspace
        .write("nested/draft.txt", "first draft", false)
        .unwrap();
    assert_eq!(
        workspace.read("nested/draft.txt", 1, 200).unwrap(),
        "first draft"
    );
    assert!(root.join("nested/draft.txt").is_file());
}

#[test]
fn writes_private_workspace_files_and_requires_explicit_overwrite() {
    let f = fixture();
    let result = f
        .workspace
        .write("nested/draft.txt", "A draft 💡", false)
        .unwrap();
    assert_eq!(result["bytes"], "A draft 💡".len());
    assert_eq!(
        f.workspace.read("nested/draft.txt", 1, 200).unwrap(),
        "A draft 💡"
    );
    assert_eq!(
        fs::metadata(f.output.join("nested/draft.txt"))
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o600
    );
    assert!(
        f.workspace
            .write("nested/draft.txt", "replacement", false)
            .is_err()
    );
    assert_eq!(
        f.workspace.read("nested/draft.txt", 1, 200).unwrap(),
        "A draft 💡"
    );
    f.workspace
        .write("nested/draft.txt", "replacement", true)
        .unwrap();
    assert_eq!(
        f.workspace.read("nested/draft.txt", 1, 200).unwrap(),
        "replacement"
    );
    assert!(
        f.workspace
            .write(
                f.repo.join("README.md").to_str().unwrap(),
                "replacement",
                true
            )
            .is_err()
    );
}

#[test]
fn traversal_symlinks_and_sibling_prefixes_cannot_escape_scope() {
    let f = fixture();
    let private = f._dir.path().join("private.txt");
    fs::write(&private, "private").unwrap();
    symlink(&private, f.repo.join("escape")).unwrap();
    symlink(f._dir.path(), f.output.join("escape")).unwrap();
    let sibling = f._dir.path().join("repos-other");
    fs::create_dir(&sibling).unwrap();
    fs::write(sibling.join("file"), "private").unwrap();
    for path in [
        private,
        f.repo.join("escape"),
        sibling.join("file"),
        PathBuf::from("../private.txt"),
    ] {
        assert!(f.workspace.read(path.to_str().unwrap(), 1, 200).is_err());
    }
    assert!(
        f.workspace
            .write("escape/private.txt", "replacement", true)
            .is_err()
    );
    assert_eq!(
        fs::read_to_string(f._dir.path().join("private.txt")).unwrap(),
        "private"
    );
    assert_eq!(
        f.workspace.read("~/repos/README.md", 1, 200).unwrap(),
        "A useful project\n"
    );
}

#[test]
fn canonicalization_follows_symlinks_before_parent_components() {
    let f = fixture();
    fs::create_dir_all(f.repo.join("deep/inside")).unwrap();
    fs::write(f.repo.join("deep/expected"), "correct").unwrap();
    symlink(f.repo.join("deep/inside"), f.output.join("link")).unwrap();
    assert_eq!(
        f.workspace.read("link/../expected", 1, 200).unwrap(),
        "correct"
    );
    assert!(
        f.workspace
            .write("link/../expected", "changed", true)
            .is_err()
    );
    symlink(f.repo.join("README.md"), f.output.join("allowed-link")).unwrap();
    assert!(
        f.workspace
            .read("allowed-link", 1, 200)
            .unwrap()
            .contains("useful")
    );
}

#[test]
fn credential_files_and_private_directories_are_excluded_everywhere() {
    let f = fixture();
    for name in [
        ".env",
        ".env.production",
        "id_rsa",
        "id_ed25519.pub",
        "cert.pem",
        "api.key",
    ] {
        fs::write(f.repo.join(name), "fixture-password").unwrap();
        assert!(
            f.workspace
                .read(f.repo.join(name).to_str().unwrap(), 1, 200)
                .is_err()
        );
        assert!(f.workspace.write(name, "new", false).is_err());
    }
    for name in [
        ".git",
        ".ssh",
        ".aws",
        ".gnupg",
        "secrets",
        "node_modules",
        ".venv",
    ] {
        fs::create_dir(f.repo.join(name)).unwrap();
        fs::write(f.repo.join(name).join("file"), "fixture-password").unwrap();
    }
    symlink(f.repo.join(".env"), f.repo.join("innocent-name")).unwrap();
    assert_eq!(
        f.workspace
            .list(f.repo.to_str().unwrap())
            .unwrap()
            .as_array()
            .unwrap()
            .len(),
        1
    );
    assert_eq!(
        f.workspace
            .search("fixture-password", f.repo.to_str().unwrap())
            .unwrap()
            .as_array()
            .unwrap()
            .len(),
        0
    );
}

#[test]
fn line_ranges_unicode_casefold_and_output_limits_match_the_contract() {
    let f = fixture();
    f.workspace
        .write("lines", "first\r\nStraße\rthird\u{2028}fourth", false)
        .unwrap();
    assert_eq!(
        f.workspace.read("lines", 2, 3).unwrap(),
        "Straße\nthird\u{2028}"
    );
    let found = f
        .workspace
        .search("STRASSE", f.output.to_str().unwrap())
        .unwrap();
    assert_eq!(found[0]["line"], 2);
    for (start, end) in [(0, 2), (2, 1), (1, 501)] {
        assert!(f.workspace.read("lines", start, end).is_err());
    }
    assert!(f.workspace.read("lines", 1, 500).is_ok());
    assert_eq!(f.workspace.read("lines", 100, 200).unwrap(), "");
    f.workspace
        .write("long", &"💡".repeat(MAX_OUTPUT + 1), false)
        .unwrap();
    assert_eq!(
        f.workspace.read("long", 1, 200).unwrap().chars().count(),
        MAX_OUTPUT
    );
}

#[test]
fn oversized_or_binary_files_fail_reads_and_are_skipped_by_search() {
    let f = fixture();
    fs::write(f.repo.join("large"), vec![b'x'; MAX_FILE + 1]).unwrap();
    fs::write(f.repo.join("binary"), [0xff, 0xfe]).unwrap();
    for name in ["large", "binary"] {
        assert!(
            f.workspace
                .read(f.repo.join(name).to_str().unwrap(), 1, 200)
                .is_err()
        );
    }
    assert!(
        f.workspace
            .write("large", &"x".repeat(MAX_FILE + 1), false)
            .is_err()
    );
    assert_eq!(
        f.workspace
            .search("x", f.repo.to_str().unwrap())
            .unwrap()
            .as_array()
            .unwrap()
            .len(),
        0
    );
    assert!(f.workspace.search(" ", f.repo.to_str().unwrap()).is_err());
}

#[test]
fn listing_and_searching_are_bounded_and_do_not_follow_directory_symlinks() {
    let f = fixture();
    for index in 0..120 {
        fs::write(f.output.join(format!("{index:03}.txt")), "needle\n").unwrap();
    }
    let listing = f.workspace.list(".").unwrap();
    assert_eq!(listing.as_array().unwrap().len(), 100);
    assert!(listing[0]["path"].as_str().unwrap().ends_with("000.txt"));
    assert_eq!(
        f.workspace
            .search("needle", ".")
            .unwrap()
            .as_array()
            .unwrap()
            .len(),
        50
    );
    symlink(&f.output, f.repo.join("directory-link")).unwrap();
    assert!(
        f.workspace
            .search("needle", f.repo.to_str().unwrap())
            .unwrap()
            .as_array()
            .unwrap()
            .is_empty()
    );
}
