{
  config,
  lib,
  pkgs,
  isGraphicalLinux,
  ...
}:
let
  enabled = isGraphicalLinux && config.dotfiles.capabilities.work;
  package = import ../../programs/newrelic-brief { inherit pkgs; };
  brief = pkgs.writeShellApplication {
    name = "newrelic-brief";
    runtimeInputs = [
      pkgs.systemd
      pkgs.ghostty
      pkgs.less
    ];
    text = ''
      export PATH="$HOME/.local/bin:${config.home.profileDirectory}/bin:$PATH:/run/current-system/sw/bin"
      exec ${package}/bin/newrelic-brief "$@"
    '';
  };
  configDir = "${config.xdg.configHome}/newrelic-brief";
  readTools = [
    "get_account_details"
    "list_apm_applications"
    "list_open_incidents"
    "run_nrql_query"
    "run_nerdgraph_query"
    "get_entity_details"
    "list_deployments_rest"
  ];
in
lib.mkIf enabled {
  home.packages = [ brief ];

  xdg.configFile = {
    "newrelic-brief/config.json".text = builtins.toJSON {
      provider = "github-copilot";
      model = "claude-opus-5.5";
      thinking = "medium";
      timeout_seconds = 600;
      pi_command = "${config.home.homeDirectory}/.local/bin/pi";
      pi_extension = "${config.home.homeDirectory}/.pi/agent/extensions/mcp";
      mcp_config = "${configDir}/mcp.json";
      prompt_file = "${configDir}/prompt.md";
    };
    "newrelic-brief/prompt.md".source = ../config/newrelic-brief/prompt.md;
    # Exclusive adapter configuration plus a direct-tool CLI allowlist. Neither
    # the general MCP gateway nor shell/file tools are available to the model.
    "newrelic-brief/mcp.json".text = builtins.toJSON {
      mcpServers.newrelic = {
        command = "${config.home.homeDirectory}/.local/bin/mcp-newrelic";
        env.NEW_RELIC_READ_ONLY = "1";
        lifecycle = "eager";
        directTools = true;
        includeTools = readTools;
        requestTimeoutMs = 120000;
      };
      settings = {
        hostConfigDiscovery = "off";
        toolPrefix = "none";
        directTools = false;
        namespaceProxyTools = false;
        scriptMode = false;
        idleTimeout = 10;
      };
    };
  };

  systemd.user.services.newrelic-brief = {
    Unit = {
      Description = "Gather a read-only New Relic brief and show new findings";
      After = [ "graphical-session.target" ];
    };
    Service = {
      Type = "oneshot";
      ExecStart = "${brief}/bin/newrelic-brief run";
      UMask = "0077";
      TimeoutStartSec = 660;
    };
  };
  systemd.user.timers.newrelic-brief = {
    Unit.Description = "New Relic briefs at 09:00, 13:00 and 16:00 on weekdays";
    Timer = {
      OnCalendar = [
        "Mon..Fri *-*-* 09:00:00 Europe/London"
        "Mon..Fri *-*-* 13:00:00 Europe/London"
        "Mon..Fri *-*-* 16:00:00 Europe/London"
      ];
      Persistent = true;
    };
    Install.WantedBy = [ "timers.target" ];
  };

  # Retention still runs over the weekend, without a Pi/model or network call.
  systemd.user.services.newrelic-brief-prune = {
    Unit.Description = "Remove New Relic reports and logs older than five calendar days";
    Service = {
      Type = "oneshot";
      ExecStart = "${brief}/bin/newrelic-brief prune";
      UMask = "0077";
    };
  };
  systemd.user.timers.newrelic-brief-prune = {
    Unit.Description = "Expire the New Relic notebook daily";
    Timer = {
      OnCalendar = "*-*-* 00:10:00 Europe/London";
      Persistent = true;
    };
    Install.WantedBy = [ "timers.target" ];
  };
}
