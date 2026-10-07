# Launcher projects

`launcher-projects [--home HOME] list` emits Elephant menu JSON. `open ID
terminal|editor|files` rescans before opening a known opaque project ID.

Discovery uses directory metadata only, never Git, environment files or repository
code. It includes a real `.dotfiles` directory and projects beneath `Repositories`,
with a maximum depth of three, 200 results and a global budget of 4,000 directory
entries. Hidden/dependency directories and symlinked entries or Git markers are
skipped. Results are byte-sorted; IDs hash the original filesystem bytes while
invalid UTF-8 in display text becomes question marks.

Opening rechecks every repository path component and Git marker, then launches
Ghostty or xdg-open through a detached systemd user scope. Paths remain individual
arguments, never shell commands. Child standard streams use `/dev/null`, inherited
descriptors close and the working directory is `/`.

## Manual fixtures

Fixtures are default-off, noninstalled and not registered with Meson tests or Nix
checks. Run them explicitly:

```sh
nix develop .#launcher-projects --command env NIX_HARDENING_ENABLE= \
  meson setup /tmp/launcher-projects-checks programs/launcher-projects \
  -Dfixtures=true --buildtype=debugoptimized
nix develop .#launcher-projects --command meson compile -C /tmp/launcher-projects-checks
/tmp/launcher-projects-checks/projects-checks
```

`LAUNCHER_PROJECTS_BIN` and `LAUNCHER_PROJECTS_FIXTURE` override the sibling
binaries. The detached-launch stand-in is a C program, not Node.

The driver uses disposable homes and stub commands, not live graphical launches.
For optional retired-implementation parity, set `LAUNCHER_PROJECTS_LEGACY` and
`LAUNCHER_PROJECTS_PYTHON`. Python is not a package dependency. Sanitizer builds can
use `-Db_sanitize=address,undefined -Db_pie=false -Dc_link_args=-no-pie`.
