{
  config,
  lib,
  pkgs,
  hostName,
  isGraphicalLinux,
  omarchy-local-ai,
  ...
}:
let
  enabled =
    isGraphicalLinux
    && builtins.elem hostName [
      "andromeda"
      "foundation"
    ];
  isAndromeda = hostName == "andromeda";
  settings = import ../config/hyprland/settings.nix;
  omarchySource = import ./themes/omarchy-source.nix;
  source = omarchy-local-ai;
  manifest = builtins.fromJSON (builtins.readFile "${source}/manifest.json");
  strata = import ./strata.nix {
    inherit config lib pkgs;
    contextSize = 131072;
  };
  strataRecipe = pkgs.writeText "strata-recipe.json" (
    builtins.toJSON {
      id = "qwen3.8-flash-next.strata.128k.rtx-5090-32gb";
      name = "Qwen3.8 Flash-Next abliterated (Strata)";
      family = "qwen";
      format = "GGUF · UD-Q4_K_XL abliterated + MTP";
      engine = "strata";
      servedName = strata.alias;
      sizeGb = 111.6;
      cards = 1;
      weights = [ ];
      asset = null;
      scratch = null;
      launch = {
        entrypoint = null;
        arguments = [ ];
        environment = { };
        port = 8081;
        shm = "1g";
      };
      serving = {
        ctxTokens = 131072;
        kvTokens = 262144;
      };
      capabilities = {
        chat = true;
        reasoning = true;
        tools = true;
        vision = false;
      };
      needs = {
        host_ram_gb = 80;
      };
    }
  );
  localRecipes = pkgs.runCommand "local-ai-strata-recipe" { nativeBuildInputs = [ pkgs.jq ]; } ''
    jq --rawfile image ${strata.imageId} '. + {image: ($image | rtrimstr("\n"))}' ${strataRecipe} > "$out"
  '';
  terminal = pkgs.writeShellApplication {
    name = "omarchy-launch-tui";
    runtimeInputs = [
      pkgs.ghostty
      pkgs.coreutils
    ];
    text = ''
      app_id=org.local-ai
      if [[ ''${1:-} == --app-id=* ]]; then
        app_id=''${1#--app-id=}
        shift
      fi
      ghostty --class="$app_id" -e "$@" </dev/null >/dev/null 2>&1 &
      pid=$!
      sleep 0.3
      if ! kill -0 "$pid" 2>/dev/null; then wait "$pid"; fi
    '';
  };
  present = pkgs.writeShellScriptBin "omarchy-cmd-present" ''command -v "$1" >/dev/null 2>&1'';
  browser = pkgs.writeShellScriptBin "omarchy-launch-browser" ''exec ${pkgs.xdg-utils}/bin/xdg-open "$@"'';
  notify = pkgs.writeShellScriptBin "omarchy-notification-send" ''exec ${pkgs.libnotify}/bin/notify-send "$@"'';
  runtime = with pkgs; [
    bash
    coreutils
    curl
    docker_29
    jq
    util-linux
    gnugrep
    gnused
    gawk
    findutils
    procps
    pciutils
    systemd
    less
    tailscale
    wl-clipboard
    terminal
    present
    browser
    notify
  ];
  backend = pkgs.stdenvNoCC.mkDerivation {
    pname = "omarchy-local-ai";
    inherit (manifest) version;
    src = source;
    patches = [ ../config/hyprland/local-ai/nix.patch ];
    nativeBuildInputs = [ pkgs.makeWrapper ];
    dontBuild = true;
    installPhase = ''
      runHook preInstall
      mkdir -p "$out/share/local-ai" "$out/bin"
      cp -r bin lib agents *.qml *.js *.json *.svg LICENSE "$out/share/local-ai/"
      cp ${../config/hyprland/local-ai/nix.sh} "$out/share/local-ai/lib/nix.sh"
      ${lib.optionalString isAndromeda ''
        cp ${localRecipes} "$out/share/local-ai/strata-recipe.json"
      ''}
      # The Omarchy installer/remover must never be offered as Nix package management.
      rm "$out/share/local-ai/bin/omarchy-install-ai-local" "$out/share/local-ai/bin/omarchy-remove-ai-local"
      patchShebangs "$out/share/local-ai/bin"
      # Store mtimes are normalised. Scope mutable catalogues to the pinned source,
      # so an old refresh cannot override a newly installed plugin's recipes.
      substituteInPlace "$out/share/local-ai/bin/omarchy-local-ai" \
        --replace-fail 'CATALOG=$HOME/.cache/omarchy/local-ai/v3/recipes.json' \
          'CATALOG=$HOME/.cache/omarchy/local-ai/v3/${source.rev}/recipes.json'
      makeWrapper "$out/share/local-ai/bin/omarchy-local-ai" "$out/bin/omarchy-local-ai" \
        --prefix PATH : ${lib.makeBinPath runtime} \
        --set LOCAL_AI_STRATA ${if isAndromeda then "1" else "0"} \
        --set LOCAL_AI_STRATA_KEY ${lib.escapeShellArg "${config.xdg.configHome}/local-llm/api-key"} \
        ${lib.optionalString isAndromeda ''
          --set LOCAL_AI_STRATA_IMAGE ${strata.image} \
          --set LOCAL_AI_STRATA_IMAGE_ID ${strata.imageId} \
          --set LOCAL_AI_STRATA_DATA ${lib.escapeShellArg strata.dataDir} \
          --set LOCAL_AI_STRATA_READY ${lib.escapeShellArg strata.readyPath} \
        ''} \
        --set LOCAL_AI_NIX 1
      runHook postInstall
    '';
  };
  patchTools = import ../../programs/omapager-tools {
    pkgs = pkgs.buildPackages;
    withIcons = false;
  };
  bundle = pkgs.runCommand "local-ai-shell" { nativeBuildInputs = [ patchTools ]; } ''
    mkdir -p "$out/shell/Commons" "$out/shell/Ui" "$out/shell/localai"
    cp -r ${omarchySource}/shell/Commons/. "$out/shell/Commons/"
    cp -r ${omarchySource}/shell/Ui/. "$out/shell/Ui/"
    chmod -R u+w "$out"
    omapager-prepare-shell "$out"
    cp ${../config/hyprland/local-ai/shell.qml} "$out/shell/shell.qml"
    cp ${backend}/share/local-ai/{*.qml,*.js,*.svg} "$out/shell/localai/"
    chmod -R u+w "$out/shell/localai"
    substituteInPlace "$out/shell/localai/Panel.qml" \
      --replace-fail 'decodeURIComponent(String(Qt.resolvedUrl("bin/omarchy-local-ai")).replace(/^file:\/\//, ""))' \
        '"${backend}/bin/omarchy-local-ai"'
    printf 'module LocalAi\nPanel 1.0 Panel.qml\nFullScreen 1.0 FullScreen.qml\n' > "$out/shell/localai/qmldir"
  '';
  shell = pkgs.writeShellApplication {
    name = "hypr-local-ai-shell";
    runtimeInputs = runtime ++ [ pkgs.quickshell ];
    text = ''
      export HYPR_CONTROLS_THEME=${lib.escapeShellArg "${config.xdg.stateHome}/theme-menu/active"}
      export HYPR_LOCAL_AI_BAR=${
        lib.escapeShellArg (builtins.toJSON { inherit (settings.bar) position width; })
      }
      export OMARCHY_MENU_FONT=${lib.escapeShellArg settings.appearance.monoFont}
      exec quickshell --no-color --path ${bundle}/shell
    '';
  };
  launcher = pkgs.writeShellApplication {
    name = "hypr-local-ai";
    runtimeInputs = with pkgs; [
      coreutils
      gnugrep
      hyprland
      jq
      libnotify
      quickshell
      systemd
    ];
    text = ''
      systemctl --user is-active --quiet hyprland-session.target || {
        echo 'Local AI requires an active Hyprland session.' >&2
        exit 1
      }
      systemctl --user start hyprland-local-ai.service
      cursor=$(hyprctl -j cursorpos)
      mapfile -t anchor < <(hyprctl -j monitors | jq -r --argjson p "$cursor" '
        . as $monitors |
        [.[] |
          ((if .transform % 2 == 0 then .width else .height end) / .scale) as $w |
          ((if .transform % 2 == 0 then .height else .width end) / .scale) as $h |
          select($p.x >= .x and $p.x < .x + $w and $p.y >= .y and $p.y < .y + $h)] |
        (.[0] // ($monitors | map(select(.focused))[0]) // $monitors[0]) |
        .name, ($p.x - .x), ($p.y - .y)')
      for _ in {1..40}; do
        if quickshell ipc --path ${bundle}/shell show 2>/dev/null | grep -q local-ai-host; then
          exec quickshell ipc --path ${bundle}/shell call local-ai-host toggle "''${anchor[@]}"
        fi
        sleep 0.1
      done
      notify-send 'Local AI' 'The panel could not start. Check hyprland-local-ai.service.'
      exit 1
    '';
  };
in
{
  config = lib.mkIf enabled {
    home.packages = [
      backend
      shell
      launcher
    ];
    xdg.dataFile."applications/local-ai.desktop".text = ''
      [Desktop Entry]
      Type=Application
      Name=Local AI
      Comment=Hardware-matched local models in Docker
      Exec=hypr-local-ai
      Icon=applications-science
      Terminal=false
      Categories=Development;Utility;
    '';
    systemd.user.services.hyprland-local-ai = {
      Unit = {
        Description = "Local AI model panel";
        PartOf = [ "hyprland-session.target" ];
        After = [ "hyprland-session.target" ];
        ConditionEnvironment = "WAYLAND_DISPLAY";
        StartLimitIntervalSec = 30;
        StartLimitBurst = 3;
      };
      Service = {
        ExecStart = "${shell}/bin/hypr-local-ai-shell";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install.WantedBy = [ "hyprland-session.target" ];
    };
  };
}
