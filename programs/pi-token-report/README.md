# Pi token report

`pi-token-report --from-run RUN_ID` rebuilds private HTML/JSON token reports.
`--capture`, or no arguments, starts the fixed three-turn capture workload;
real capture requires explicit authorization through the token-measurement skill.

Provider token totals and character-share cost estimates remain distinct.
Reports omit arbitrary arguments, private query text, URL credentials and
unexpected probe output. Raw captures stay private. Python is retained only for
the workload's model-generated `add.py`, not the reporting implementation.

Build with `nix build .#pi-token-report`. Manual fixtures use synthetic traces and
a stub capture process in the
[native skill command collection](../collections/skill-tools), not inference.
