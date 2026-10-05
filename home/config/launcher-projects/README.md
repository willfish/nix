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
direnv exec . nix develop .#launcher-projects -c meson setup \
  /tmp/launcher-projects-build home/config/launcher-projects \
  --buildtype=debugoptimized -Dfixtures=true
direnv exec . nix develop .#launcher-projects -c meson compile \
  -C /tmp/launcher-projects-build
LAUNCHER_PROJECTS_BIN=/tmp/launcher-projects-build/launcher-projects \
LAUNCHER_PROJECTS_FIXTURE=/tmp/launcher-projects-build/projects-fixture \
  direnv exec . node --experimental-strip-types --test \
  home/config/launcher-projects/tests/projects.test.ts
```

The driver uses disposable homes and stub commands, not live graphical launches.
For optional retired-implementation parity, set `LAUNCHER_PROJECTS_LEGACY` and
`LAUNCHER_PROJECTS_PYTHON`. Python is not a package dependency. Sanitizer builds can
use `-Db_sanitize=address,undefined -Db_pie=false -Dc_link_args=-no-pie`.
