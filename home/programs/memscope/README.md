# memscope

A small, one-shot Linux memory report. The headline shows used and total RAM;
the second chart zooms into named mappings and lists the processes attached to
them. It uses C17 and libc, without a terminal UI framework or background service.

```sh
memscope
memscope --limit 10 --full-paths
memscope --ascii --color never
```

Home Manager installs it on Linux through `home/user/packages.nix`.

## Read the two scales

**RAM used** is `MemTotal - MemAvailable`, using the kernel's availability
estimate. It is not a memory-pressure measurement. Missing availability is
reported as unavailable, not replaced with free memory.

**Observed file-named mappings** are ranked by proportional set size (PSS).
Shared resident pages contribute a proportional share rather than counting in
full for each mapping. The section header shows its total as a percentage of
machine RAM. Each row's bar and percentage instead use the total PSS of all
observed eligible mappings. Changing `--limit` does not change that denominator;
`Other observed mappings` retains omitted rows.

The file chart does **not** partition RAM used. File-named mappings may include
anonymous private copies after writes. Unmapped page cache is absent. Process
PSS below the chart overlaps those mappings and must not be added to them.
Anonymous resident memory in the headline is also overlapping context.

Mappings are grouped by device and inode, not by filename. Aliases share a row;
deleted mappings retain a tag. Duplicate display names receive identity labels.
Identifiable shared-memory mappings are tagged. Special mappings marked by the
kernel as I/O or PFN mappings are excluded from the ordinary chart. This is not
a complete device, GPU, huge-page or physical-memory inventory. A pathname is
not proof that its backing is an ordinary file.

## Scope and presentation

Only visible, readable processes contribute. Denied, vanished and malformed
samples are counted separately; they are never estimated as zero or folded into
Other. Kernel threads have no user address space. Collection spans an interval,
not an atomic snapshot. PID namespaces and procfs restrictions can hide processes
even when no read fails. The program never elevates privileges.

Filenames and bars use ANSI blue, process names cyan, percentage markers
magenta, available memory green, and partial-coverage badges yellow. Percentage
numbers stay bold in the default foreground so light themes remain legible.
Values and headings are bold, with terminal-default text and background
elsewhere. These roles add visual hierarchy without relying on colour alone.
Detailed accounting notes live in `--help`, rather than filling the report.
Omarchy and this repository's theme menu already configure that palette.
No application theme file is needed. `NO_COLOR` overrides colour options;
pipes and `TERM=dumb` default to unstyled ASCII. Small terminals drop file bars
before dropping values. Paths and process names escape non-printable and
non-ASCII bytes to keep terminal output safe and column widths deterministic.
Use `--full-paths` to expand shortened display labels and attachment lists.

## Development

From this repository's environment:

```sh
direnv exec . make -C home/programs/memscope check
```

`--proc-root PATH` accepts an offline procfs fixture tree for reproducible checks.
It does not provide remote access or change privileges. Tests cover the CLI,
accounting, terminal widths, permission failures and live shared/private mmap
behavior. Build dependencies are a C17 compiler and make; manual tests also use
Node 24 or newer and a noninstalled C helper for PTY and mmap operations. The
suite is not run by package builds, flake checks or commit hooks. Set
`MEMSCOPE_BIN` to exercise a packaged binary with the same manual driver.
