{ pkgs, lib, ... }:
let
  # The nix build buries bundled extensions inside a version-specific pnpm
  # path that changes every update.  Build a tiny derivation that finds and
  # symlinks it so we get a stable store path to pass via env var.
  clawdbot-extensions = pkgs.runCommand "clawdbot-extensions" {} ''
    mkdir -p $out
    ext=$(find ${pkgs.clawdbot-gateway}/lib/clawdbot/node_modules/.pnpm \
      -path '*/node_modules/clawdbot/extensions' -type d | head -1)
    if [ -z "$ext" ]; then
      echo "ERROR: could not locate clawdbot extensions directory" >&2
      exit 1
    fi
    ln -s "$ext" $out/extensions
  '';
in
{
  programs.clawdbot = {
    documents = ../config/clawdbot/documents;
    # Explicitly set the package to our overlay-patched clawdbot so the
    # module doesn't go through clawdbotPackages.withTools (which rebuilds
    # from unpatched nixpkgs, losing our docs/reference/templates fix).
    instances.default = {
      enable = true;
      package = pkgs.clawdbot;
      providers.telegram = {
        enable = true;
        botTokenFile = "/home/william/.secrets/telegram-bot-token";
        allowFrom = [ 142683073 ];
        groups = {
          "*" = { requireMention = true; };
        };
      };
      providers.anthropic = {
        apiKeyFile = "/home/william/.secrets/anthropic-api-key";
      };
      plugins = [ ];
    };
  };

  # The nix-clawdbot module generates config with legacy keys (telegram,
  # messages.queue.byProvider) but the clawdbot binary rejects them.
  # The config is a read-only nix store symlink so doctor --fix can't patch
  # it in place. This activation script replaces the symlink with a mutable
  # copy and migrates the schema.
  home.activation.clawdbotMigrateConfig = lib.hm.dag.entryAfter [ "clawdbotConfigFiles" ] ''
    _cfg="$HOME/.clawdbot/clawdbot.json"
    if [ -L "$_cfg" ]; then
      _tmp=$(mktemp)
      cp -L "$_cfg" "$_tmp"
      rm "$_cfg"
      _token=$(${pkgs.coreutils}/bin/cat "$HOME/.secrets/clawdbot-gateway-token" 2>/dev/null || echo "")
      ${pkgs.jq}/bin/jq --arg tok "$_token" '
        # telegram -> channels.telegram
        (if .telegram then .channels.telegram = .telegram | del(.telegram) else . end)
        # messages.queue.byProvider -> messages.queue.byChannel
        | (if .messages.queue.byProvider then .messages.queue.byChannel = .messages.queue.byProvider | del(.messages.queue.byProvider) else . end)
        # inject gateway auth token
        | (if $tok != "" then .gateway.auth = {"mode":"token","token":$tok} else . end)
      ' "$_tmp" > "$_cfg"
      rm "$_tmp"
      chmod 600 "$_cfg"
    fi
  '';

  # The upstream wrapper uses bare `cat` (needs coreutils on PATH) and the
  # nix build breaks bundled plugin discovery (extensions/ dir isn't found
  # relative to the nix store entry point). Fix both via env vars.
  systemd.user.services.clawdbot-gateway.Service.Environment = lib.mkAfter [
    "PATH=${pkgs.coreutils}/bin"
    "CLAWDBOT_BUNDLED_PLUGINS_DIR=${clawdbot-extensions}/extensions"
  ];
}
