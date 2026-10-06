# Telegram login adapter

Compiled C glue for the unchanged Telethon runtime. The existing manual wrapper
still reads SOPS credentials, selects the private file-based session and invokes
login only when explicitly run. `start`, identity reporting and unconditional
`disconnect` are called through the CPython C API, without handwritten Python.

The pinned interpreter and Telethon package directory are supplied by Nix.
Authentication prompts remain Telethon's responsibility. Errors are reported
without credential values. `--help` does not initialize Telethon or authenticate.

Build with `nix build .#telegram-login`. A real login is not a build or activation
check and requires the user's explicit request.
