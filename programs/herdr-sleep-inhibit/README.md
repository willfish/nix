# Herdr sleep inhibitor

`herdr-agent-awake` inhibits system sleep while Herdr reports working agents.
If Herdr cannot be reached, the helper allows sleep instead of holding an
indefinite inhibitor. It is a Linux user service wired by `home/user/hyprland.nix`.

`default.nix` supplies the pinned systemd inhibitor and sleep commands at build
time. Build with `direnv exec . nix build .#herdr-sleep-inhibit`. Do not start a
second live inhibitor merely to verify packaging.
