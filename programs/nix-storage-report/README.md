# Nix storage report

`nix-storage-report` accounts for direct, shared and unique Nix store closure
sizes, producing grouped summaries, CSV and Markdown. It queries local store
data and skips unrealized inventory roots rather than treating them as free.

`scripts/nix-storage-costs` supplies the public configuration-selection and build
workflow. Home Manager installs the reporter through the shell integration.
Build with `nix build .#nix-storage-report`, or invoke the wrapper using
`nix shell .#nix-storage-report -c scripts/nix-storage-costs ...`.

Manual accounting and wrapper checks use stub Nix responses in the
[repository command collection](../collections/repo-tools).
