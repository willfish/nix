# Shared program support

These are source dependencies, not executable packages:

- `repo-data`: JSON access, text normalization and file helpers used by the
  repository data commands and native skill commands.
- `command-support`: numeric argument and text helpers used by contrast,
  YouTube, audiobook and token-report commands.

Packages include only the support files they use through explicit Nix filesets
and Meson sources. Do not add a global runtime or build framework here.
The audiobook commands also reuse the Pi config JSON value codec explicitly.
