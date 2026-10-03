{
  lib,
  pkgs,
  mkHome,
}:
let
  home = mkHome {
    username = "new-user";
    homeDirectory = "/srv/homes/new-user";
    # Hostname collisions must not enable owner hardware/services.
    hostName = "andromeda";
  };
  c = home.config;
  caps = c.dotfiles.capabilities;
  servers = (builtins.fromJSON c.home.file.".config/mcp/mcp.json".text).mcpServers;
  absentFile = path: !(builtins.hasAttr path c.home.file);
  tui = builtins.fromJSON c.home.file.".config/opencode/tui.json".text;
in
assert c.home.username == "new-user";
assert c.home.homeDirectory == "/srv/homes/new-user";
assert !c.dotfiles.privateEnabled;
assert c.dotfiles.role == "legacy";
assert caps.desktop && caps.development && caps.localTerminal;
assert builtins.all (name: !caps.${name}) [
  "work"
  "knowledgeBase"
  "accounting"
  "email"
  "hermes"
  "telegram"
  "personal"
];
assert c.sops.secrets == { };
assert !c.home.file.".agents/AGENTS.md".force;
assert !c.xdg.configFile."btop/btop.conf".force;
assert !pkgs.stdenv.isLinux || !c.xdg.configFile."mimeapps.list".force;
assert !(builtins.hasAttr "applications/switchboard.desktop" c.xdg.dataFile);
assert !(c ? privateConfig);
assert !(c.programs ? pi-agent-bus);
assert !(c.programs.git.settings ? user);
assert c.programs.git.signing.signByDefault != true;
assert
  builtins.attrNames servers == [
    "browser"
    "dap"
    "filesystem"
    "nixos"
  ];
assert builtins.all absentFile [
  ".local/bin/mcp-github"
  ".local/bin/mcp-web-search"
  ".local/bin/mcp-slack"
  ".local/bin/mcp-arxiv"
  ".local/bin/mcp-telegram"
  ".local/bin/mcp-himalaya"
  ".pi/agent/models.json"
  ".pi/agent/extensions/pi-voice.ts"
];
assert !(c.home.activation ? configureGithubCliAuth);
assert !(c.home.activation ? migrateAgentSkillDirectories);
assert builtins.all (name: !(builtins.hasAttr name c.systemd.user.services)) [
  "knowledge-base"
  "daily-agenda-refresh"
  "ssh-add-default"
  "wifi-auto-reconnect"
];
assert !lib.hasInfix "read-sops-secret" c.home.file.".local/bin/pi".text;
assert !lib.hasInfix "read-sops-secret" c.home.file.".local/bin/opencode".text;
assert !lib.hasInfix "taile09696" c.home.file.".config/opencode/opencode.json".text;
assert !lib.hasInfix "OPENCODE_GO_KEY" c.home.file.".config/opencode/opencode.json".text;
assert tui.keybinds.editor_open == "ctrl+g,<leader>e";
assert tui.keybinds.messages_first == "home";
assert !(builtins.hasAttr ".config/opencode/plugins/agent-bus.ts" c.home.file);
pkgs.runCommand "public-home-boundaries" { } ''
  touch "$out"
''
