{ pkgs }:
pkgs.telegram-desktop.override {
  pname = "telegram-desktop-themed";
  unwrapped = pkgs.telegram-desktop.unwrapped.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./follow-theme.patch ];
  });
}
