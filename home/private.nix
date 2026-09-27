{ config, lib, ... }:
let
  cfg = config.dotfiles;
  full = builtins.elem cfg.role [
    "workstation"
    "legacy"
  ];
in
{
  # Imported only in the authenticated composition, never by a public home.
  privateConfig = {
    secretGroups = [
      "coding"
      "search"
    ]
    ++ lib.optional cfg.capabilities.work "work"
    ++ lib.optional cfg.capabilities.accounting "accounting"
    ++ lib.optional cfg.capabilities.email "email"
    ++ lib.optional cfg.capabilities.telegram "telegram"
    ++ lib.optional cfg.capabilities.personal "personal"
    ++ lib.optional (cfg.capabilities.nas || full) "nas"
    ++ lib.optional full "legacy";
    hermesEnvironment = cfg.capabilities.hermes;
    darwinSystemService = cfg.darwinSystemServices;
  };
  programs.pi-agent-bus.enable = lib.mkDefault true;
}
