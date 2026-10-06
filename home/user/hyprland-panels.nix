{
  config,
  lib,
  pkgs,
  isGraphicalLinux,
  ...
}:
let
  settings = import ../config/hyprland/settings.nix;
  source = import ./themes/omarchy-source.nix;
  local = ../config/hyprland/controls;
  agents = ../config/hyprland/agents;
  # Pinned bar-widget plugin. The sync scripts stay out: they expect Google
  # OAuth, and the rail reads the existing private iCal export instead.
  calendarPlugin = pkgs.fetchFromGitHub {
    owner = "tmn73";
    repo = "omarchy-calendar";
    rev = "15509fca18f14b9e7da5d7bf85cdf3d9da665bae";
    hash = "sha256-XfCeAmSyWltREKuDn9/6OdwyvjdtlQWH+9QLRnEOmlY=";
  };
  panelSettings = import ./panel-settings-package.nix { inherit pkgs; };
  calendarSettings = pkgs.writeShellApplication {
    name = "hypr-calendar-settings";
    text = ''
      exec ${panelSettings}/bin/hypr-calendar-settings \
        ${lib.escapeShellArg "${config.xdg.configHome}/hyprland/calendar-settings.json"} "$@"
    '';
  };
  # Waybar owns the bar, so the plugin's bar pill stays out. The panel and its
  # one service mount in the existing controls shell, same as the calendar.
  weatherPlugin = pkgs.fetchFromGitHub {
    owner = "eduardodallecort";
    repo = "omarchy-weather-radar";
    rev = "7234f1abc6f47738f37d13d8d354c62aca7d82f7";
    hash = "sha256-BA4PgDY2PCJ+5upQCbnWxh0ig3vE7627ZPOWKeGDMds=";
  };
  weatherSettings = pkgs.writeShellApplication {
    name = "hypr-weather-settings";
    text = ''
      exec ${panelSettings}/bin/hypr-weather-settings \
        ${lib.escapeShellArg "${config.xdg.configHome}/hyprland/weather-settings.json"} "$@"
    '';
  };
  # The panel's city picker. Same file the service already watches.
  weatherLocation = pkgs.writeShellApplication {
    name = "omarchy-weather-location";
    runtimeInputs = [
      pkgs.jq
      pkgs.coreutils
    ];
    text = ''
      loc_file="''${XDG_STATE_HOME:-$HOME/.local/state}/omarchy/settings/weather.json"
      coords_pattern='^(-?[0-9]+(\.[0-9]+)?),(-?[0-9]+(\.[0-9]+)?)$'
      case "''${1:-}" in
      --set)
        [[ -n ''${2:-} ]] || {
          echo 'Usage: omarchy-weather-location --set <name> [lat,lon]' >&2
          exit 1
        }
        if [[ -n ''${3:-} ]]; then
          [[ ''${3} =~ $coords_pattern ]] || {
            echo "Invalid coordinates: ''${3} (expected lat,lon)" >&2
            exit 1
          }
          location=$(jq -n --arg name "''${2}" --argjson latitude "''${3%,*}" --argjson longitude "''${3#*,}" '{$name, $latitude, $longitude}')
        else
          location=$(jq -n --arg name "''${2}" '{$name}')
        fi
        install -d -m 700 -- "$(dirname "$loc_file")"
        umask 077
        printf '%s\n' "$location" >"$loc_file"
        ;;
      --clear)
        rm -f -- "$loc_file"
        ;;
      *)
        echo 'Usage: omarchy-weather-location --set <name> [lat,lon] | --clear' >&2
        exit 1
        ;;
      esac
    '';
  };
  weatherNotify = pkgs.writeShellApplication {
    name = "omarchy-weather-notify";
    runtimeInputs = [ pkgs.libnotify ];
    text = ''
      urgency=normal
      while [[ $# -gt 0 ]]; do
        case "$1" in
        --app-name)
          shift 2
          ;;
        -g)
          shift 2
          ;;
        -u | --urgency)
          urgency=$2
          shift 2
          ;;
        *) break ;;
        esac
      done
      [[ $# -ge 1 ]] || exit 2
      headline=$1
      shift
      description=
      if [[ $# -gt 0 && $1 != -* ]]; then
        description=$1
      fi
      case $urgency in
      low | normal | critical) ;;
      *) urgency=normal ;;
      esac
      exec notify-send -a 'Weather Radar' -u "$urgency" -- "$headline" "$description"
    '';
  };
  # Drop-in for the stock emoji overlay. Waybar is not involved: this is a
  # centred picker, opened with Super+Ctrl+E.
  emojiPlugin = pkgs.fetchFromGitHub {
    owner = "Wessel-Boers";
    repo = "omarchy-better-emojis";
    rev = "e945ae85ec0f55eb6432c13b8e8e23907ca0d21d";
    hash = "sha256-kJD1RSuEwvNkQMAUsgP1r9xhzc00peXl3Dsc1Cfu7ZM=";
  };
  # Remember the window that had focus when the shortcut was pressed.
  # Focus follows the mouse, so the click that picks an emoji would otherwise
  # paste into whatever is under the pointer.
  emojiRemember = pkgs.writeShellApplication {
    name = "hypr-emoji-remember";
    runtimeInputs = [
      pkgs.hyprland
      pkgs.jq
      pkgs.coreutils
    ];
    text = ''
      dest="''${XDG_STATE_HOME:-$HOME/.local/state}/omarchy/emoji-target"
      install -d -m 700 -- "$(dirname "$dest")"
      umask 077
      hyprctl activewindow -j | jq -r '[.address // "", .class // ""] | .[]' >"$dest"
    '';
  };
  emojiOpen = pkgs.writeShellApplication {
    name = "hypr-emojis";
    runtimeInputs = [
      emojiRemember
      omarchyShell
    ];
    text = ''
      hypr-emoji-remember
      exec omarchy-shell shell toggle omarchy.emojis
    '';
  };
  # Ghostty ignores synthetic keys, including Shift+Insert and Ctrl+Shift+V.
  # A click there can only copy. Pi reads that clipboard on Ctrl+V.
  # Other apps still get a real paste, because they accept those keys.
  emojiInsert = pkgs.writeShellApplication {
    name = "omarchy-menu-emoji-insert";
    runtimeInputs = [
      pkgs.wl-clipboard
      pkgs.wtype
      pkgs.coreutils
      pkgs.hyprland
      pkgs.jq
      pkgs.libnotify
    ];
    text = ''
      emoji=''${1:-}
      [[ -n $emoji ]] || exit 0
      # The picker is not a normal window. Whatever Hyprland still calls
      # active is the app that should receive the emoji. A saved address from
      # an earlier shortcut must not steal a Telegram paste.
      class=$(hyprctl activewindow -j | jq -r '.class // empty')
      address=$(hyprctl activewindow -j | jq -r '.address // empty')
      if [[ -z $class ]]; then
        target="''${XDG_STATE_HOME:-$HOME/.local/state}/omarchy/emoji-target"
        if [[ -f $target ]]; then
          {
            IFS= read -r address
            IFS= read -r class
          } <"$target"
        fi
        if [[ -n $address ]]; then
          hyprctl dispatch focuswindow "address:$address" >/dev/null || true
          sleep 0.12
          class=$(hyprctl activewindow -j | jq -r '.class // empty')
        fi
      fi
      case $class in
        *ghostty* | *Ghostty*)
          printf '%s' "$emoji" | wl-copy --type text/plain
          notify-send -a Emojis 'Copied' 'Ctrl+V pastes into Pi'
          ;;
        *)
          printf '%s' "$emoji" | wl-copy --type text/plain --foreground &
          clip=$!
          printf '%s' "$emoji" | wl-copy --primary --type text/plain --foreground &
          primary=$!
          # Let the picker release the keyboard before the paste key.
          sleep 0.2
          wtype -M shift -k Insert -m shift || true
          sleep 0.3
          kill "$clip" "$primary" 2>/dev/null || true
          ;;
      esac
    '';
  };
  # Pinned arXiv bar widget. Marketplace install is unavailable and expects
  # Omarchy's own bar. Waybar stays the bar; this timer and the popup live here.
  arxivPlugin = pkgs.fetchFromGitHub {
    owner = "linuskelsey";
    repo = "arxiv-scanner";
    rev = "5f4e8db6590a96743aea271365a49439919e18c5";
    hash = "sha256-g9972zsbtZ9PEtBAsEprmsiWb8ec0M2GaUICgQorNec=";
  };
  arxivScripts =
    pkgs.runCommand "arxiv-scanner-scripts"
      {
        nativeBuildInputs = [ pkgs.patch ];
      }
      ''
        mkdir -p "$out/bin"
        cp ${arxivPlugin}/bin/poll.py ${arxivPlugin}/bin/check-authors.py \
          ${arxivPlugin}/bin/save-settings.sh "$out/bin/"
        chmod -R u+w "$out"
        substituteInPlace "$out/bin/poll.py" \
          --replace-fail '["claude", "-p"' '["${pkgs.claude-code}/bin/claude", "-p"' \
          --replace-fail '["omarchy", "notification", "send", "--app-name", "arXiv Scanner", "-u", "normal", title, body],' \
          '["${pkgs.libnotify}/bin/notify-send", "-a", "arXiv Scanner", "-u", "normal", "--", title, body],'
        patch --batch -d "$out/bin" -p1 < ${../config/hyprland/arxiv/timer-dropin.patch}
        rm -f "$out/bin/"*.orig
        patchShebangs "$out/bin"
      '';
  arxivConfig = pkgs.writeText "arxiv-scanner-config.json" ''
    {
      "category": "cs.IR",
      "interestAreas": [
        "hybrid lexical and dense retrieval, including reciprocal rank fusion",
        "query rewriting or expansion for short product and goods descriptions",
        "embedding models, cross-encoders, and rerankers for product search",
        "hierarchical or legal classification of goods, HS codes, and customs tariffs",
        "clarifying questions that narrow an ambiguous product description",
        "sentence-transformer classifiers for product categorization"
      ],
      "watchedAuthors": [],
      "maxAreaMatches": 4,
      "maxWatchedMatches": 3,
      "pollTime": "07:30"
    }
  '';
  # The first shared seed. Activation replaces this exact file and nothing else.
  arxivPreviousConfig = pkgs.writeText "arxiv-scanner-config-previous.json" ''
    {
      "category": "quant-ph",
      "interestAreas": [],
      "watchedAuthors": [],
      "maxAreaMatches": 3,
      "maxWatchedMatches": 3,
      "pollTime": "07:30"
    }
  '';
  bundle = pkgs.runCommand "omarchy-panels" { nativeBuildInputs = [ pkgs.patch ]; } ''
    mkdir -p "$out/shell/plugins/panels/calendar" "$out/shell/plugins/panels/weather"
    cp -r ${source}/shell/Ui ${source}/shell/Commons "$out/shell/"
    for panel in audio bluetooth network tailscale; do
      cp -r ${source}/shell/plugins/panels/"$panel" "$out/shell/plugins/panels/"
    done
    cp ${calendarPlugin}/LICENSE ${calendarPlugin}/Model.js ${calendarPlugin}/manifest.json \
      ${calendarPlugin}/*.qml "$out/shell/plugins/panels/calendar/"
    cp -r ${weatherPlugin}/ui ${weatherPlugin}/lib ${weatherPlugin}/data "$out/shell/plugins/panels/weather/"
    cp ${weatherPlugin}/LICENSE ${weatherPlugin}/manifest.json ${weatherPlugin}/Panel.qml \
      ${weatherPlugin}/Service.qml "$out/shell/plugins/panels/weather/"
    chmod -R u+w "$out/shell/plugins/panels/weather"
    substituteInPlace "$out/shell/plugins/panels/weather/Panel.qml" \
      --replace-fail '"omarchy-weather-location"' '"${weatherLocation}/bin/omarchy-weather-location"'
    substituteInPlace "$out/shell/plugins/panels/weather/Service.qml" \
      --replace-fail '"omarchy-notification-send"' '"${weatherNotify}/bin/omarchy-weather-notify"'
    mkdir -p "$out/shell/plugins/arxiv"
    cp ${arxivPlugin}/BarWidget.qml ${arxivPlugin}/LICENSE "$out/shell/plugins/arxiv/"
    chmod -R u+w "$out/shell/plugins/arxiv"
    substituteInPlace "$out/shell/plugins/arxiv/BarWidget.qml" \
      --replace-fail 'function close() { popupOpen = false }' \
      'function close() { popupOpen = false } function closeForPopoutSwitch() { close() }' \
      --replace-fail 'readonly property string pluginDir: Quickshell.env("HOME") + "/.config/omarchy/plugins/prometheus.arxiv-scanner/"' \
      'readonly property string pluginDir: "${arxivScripts}/"'
    mkdir -p "$out/shell/plugins/emojis"
    cp ${emojiPlugin}/BetterEmojis.qml ${emojiPlugin}/EmojiData.js ${emojiPlugin}/emojis.json \
      ${emojiPlugin}/LICENSE "$out/shell/plugins/emojis/"
    chmod -R u+w "$out/shell/plugins/emojis"
    substituteInPlace "$out/shell/plugins/emojis/BetterEmojis.qml" \
      --replace-fail 'root.omarchyPath + "/bin/omarchy-menu-emoji-insert"' \
      '"${emojiInsert}/bin/omarchy-menu-emoji-insert"'
    cp -r ${source}/shell/plugins/agents "$out/shell/plugins/"
    chmod -R u+w "$out/shell/plugins/agents"
    cp ${agents}/grok.svg ${agents}/grok-light.svg ${agents}/opencode.svg ${agents}/opencode-light.svg \
      "$out/shell/plugins/agents/assets/"
    cp -r ${source}/shell/plugins/polkit "$out/shell/plugins/"
    cp -r ${source}/shell/plugins/menu "$out/shell/plugins/"
    chmod -R u+w "$out"
    printf '%s\n' 'module OmarchyMenu' 'Menu 1.0 Menu.qml' > "$out/shell/plugins/menu/qmldir"
    patch --batch -d "$out" -p1 < ${local}/nixos.patch
    # Select mode must show before the unused menu catalogue loads.
    substituteInPlace "$out/shell/plugins/menu/Menu.qml" \
      --replace-fail 'cursorActive = mode !== "input"' \
      'cursorActive = mode !== "input"; root.rowsLoaded = true'
    # pkexec realpath()s its program. The packaged tailscale path is a symlink to
    # tailscaled, so the command starts the daemon and exits before setting an operator.
    substituteInPlace "$out/shell/plugins/panels/tailscale/Service.qml" \
      --replace-fail '["pkexec", "tailscale", "set", "--operator=" + userName]' \
      '["/run/wrappers/bin/pkexec", "${tailscaleCli}", "set", "--operator=" + userName]'
    cp ${local}/shell.qml "$out/shell/shell.qml"
    cp ${source}/LICENSE "$out/LICENSE"
  '';
  # Only these reviewed helpers enter the panel process's PATH, not Omarchy's
  # installer, updater, session launcher or plugin loader.
  helpers = pkgs.runCommand "omarchy-panel-helpers" { } ''
    mkdir -p "$out/bin"
    for name in audio-output-set-default audio-input-set-default audio-output-sink \
      audio-sink-availability network-status network-band cmd-present bluetooth-power; do
      cp ${source}/bin/omarchy-"$name" "$out/bin/"
    done
    chmod -R u+w "$out"
    substituteInPlace "$out/bin/omarchy-audio-sink-availability" \
      --replace-fail 'fronted="$(omarchy-audio-tuning fronted-sink 2>/dev/null || true)"' 'fronted=""'
    patchShebangs "$out/bin"
  '';
  pythonWithGio = pkgs.python3.withPackages (ps: [ ps.pygobject3 ]);
  fileSelect = pkgs.writeShellApplication {
    name = "omarchy-file-select";
    runtimeInputs = [
      pythonWithGio
      pkgs.glib
    ];
    text = ''
      export GI_TYPELIB_PATH="${pkgs.glib}/lib/girepository-1.0''${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}"
      exec ${pythonWithGio}/bin/python3 ${source}/bin/omarchy-file-select "$@"
    '';
  };
  notify = pkgs.writeShellApplication {
    name = "omarchy-notification-send";
    runtimeInputs = [ pkgs.libnotify ];
    text = ''
      urgency=low
      while [[ $# -gt 0 ]]; do
        case "$1" in
          -u | --urgency)
            urgency=$2
            shift 2
            ;;
          -g | --glyph)
            shift 2
            ;;
          *) break ;;
        esac
      done
      headline=
      description=
      [[ $# -ge 1 ]] || exit 2
      headline=$1
      shift
      if [[ $# -gt 0 && $1 != -* ]]; then
        description=$1
      fi
      exec notify-send -a Tailscale -u "$urgency" -- "$headline" "$description"
    '';
  };
  # A real file, not the packaged symlink. Forces CLI argv0 after pkexec's shebang reset.
  tailscaleCli = pkgs.writeShellScript "tailscale-cli" ''
    exec -a tailscale ${pkgs.tailscale}/bin/.tailscaled-wrapped "$@"
  '';
  tailscaleSend = pkgs.writeShellApplication {
    name = "omarchy-tailscale-send";
    runtimeInputs = [
      pkgs.tailscale
      pkgs.coreutils
      fileSelect
      notify
    ];
    text = lib.removePrefix "#!/bin/bash\n" (builtins.readFile "${source}/bin/omarchy-tailscale-send");
  };
  bluetoothDevice = pkgs.writeShellApplication {
    name = "omarchy-bluetooth-device";
    runtimeInputs = [
      pkgs.bluez
      pkgs.coreutils
      pkgs.blueman
    ];
    text = ''
      action=''${1:-}
      address=''${2:-}
      [[ $address =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]] || exit 2
      case "$action" in
        connect|disconnect) exec timeout 20 bluetoothctl "$action" "$address" ;;
        forget) exec timeout 20 bluetoothctl remove "$address" ;;
        pair) exec blueman-manager ;;
        *) exit 2 ;;
      esac
    '';
  };
  browser = pkgs.writeShellApplication {
    name = "omarchy-launch-browser";
    runtimeInputs = [ pkgs.xdg-utils ];
    text = ''exec xdg-open "$@"'';
  };
  laptopClosed = pkgs.writeShellApplication {
    name = "omarchy-hw-laptop-closed";
    runtimeInputs = [ pkgs.gnugrep ];
    text = ''
      for state in /proc/acpi/button/lid/*/state; do
        [ -r "$state" ] || continue
        grep -q closed "$state" && exit 0
      done
      exit 1
    '';
  };
  # Reviewed collectors only. The pinned panel watches their JSON records.
  # Codex and Grok use Pi's OAuth logins; OpenCode Go uses Pi's stored key.
  # Omarchy's installer, plugin loader, and agent launcher stay out.
  agentUpdate = pkgs.writeShellScript "omarchy-agent-usage-update" ''
    here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    export OMARCHY_PATH=$(CDPATH= cd -- "$here/.." && pwd)
    export PATH="${pkgs.jq}/bin:${pkgs.bash}/bin:$PATH"
    usage="''${XDG_STATE_HOME:-$HOME/.local/state}/omarchy/agents/usage"
    mkdir -p "$usage"
    chmod 700 "$usage" || true
    ${pkgs.bash}/bin/bash "$OMARCHY_PATH/libexec/omarchy-agent-usage-update" "$@"
    status=$?
    chmod 600 "$usage"/*.json 2>/dev/null || true
    exit $status
  '';
  agentUsage = import ./agent-usage-package.nix { inherit pkgs; };
  agentCollectors =
    pkgs.runCommand "omarchy-agent-collectors" { nativeBuildInputs = [ pkgs.bash ]; }
      ''
        mkdir -p "$out/bin" "$out/libexec"
        for name in codex grok opencode; do
          ln -s ${agentUsage}/bin/omarchy-agent-usage-"$name" "$out/bin/omarchy-agent-usage-$name"
        done
        install -m755 ${source}/bin/omarchy-agent-usage-update "$out/libexec/omarchy-agent-usage-update"
        install -m755 ${agentUpdate} "$out/bin/omarchy-agent-usage-update"
        patchShebangs "$out/libexec/omarchy-agent-usage-update"
      '';
  runtime = with pkgs; [
    bash
    coreutils
    curl
    gawk
    gnused
    gnugrep
    networkmanager
    networkmanagerapplet
    iproute2
    iw
    iputils
    jq
    wireplumber
    pulseaudio
    bluez
    util-linux
    fontconfig
    wl-clipboard
    blueman
    libnotify
    systemd
    which
    tailscale
    helpers
    bluetoothDevice
    browser
    laptopClosed
    tailscaleSend
    calendarSettings
    weatherSettings
    agentCollectors
    pkgs.findutils
    quickshell
  ];
  shell = pkgs.writeShellApplication {
    name = "hypr-controls-shell";
    runtimeInputs = runtime;
    text = ''
      # Keep the setuid pkexec ahead of any store copy a runtime input might add.
      export PATH="/run/wrappers/bin:$PATH"
      export HYPR_CONTROLS_THEME=${lib.escapeShellArg "${config.xdg.stateHome}/theme-menu/active"}
      export HYPR_CALENDAR_SETTINGS=${lib.escapeShellArg "${config.xdg.configHome}/hyprland/calendar-settings.json"}
      export HYPR_WEATHER_SETTINGS=${lib.escapeShellArg "${config.xdg.configHome}/hyprland/weather-settings.json"}
      export HYPR_AGENTS_SETTINGS=${
        lib.escapeShellArg (
          builtins.toJSON {
            providers = {
              claude.enabled = false;
              fireworks.enabled = false;
              codex.enabled = true;
              grok.enabled = true;
              opencode.enabled = true;
            };
            refreshIntervalSec = 900;
            syncMode = "Off";
          }
        )
      }
      export OMARCHY_PATH=${agentCollectors}
      export HYPR_CONTROLS_SETTINGS=${
        lib.escapeShellArg (
          builtins.toJSON {
            inherit (settings.bar) position width;
            font = settings.appearance.monoFont;
          }
        )
      }
      export OMARCHY_MENU_FONT=${lib.escapeShellArg settings.appearance.monoFont}
      exec quickshell --no-color --path ${bundle}/shell
    '';
  };
  absent = pkgs.writeShellScriptBin "omarchy-cmd-present" ''
    exit 1
  '';
  menuSelect = pkgs.runCommand "omarchy-menu-select" { } ''
    mkdir -p "$out/bin"
    cp ${source}/bin/omarchy-menu-select "$out/bin/omarchy-menu-select"
    chmod u+w "$out/bin/omarchy-menu-select"
    patchShebangs "$out/bin/omarchy-menu-select"
  '';
  keybindingsScript = pkgs.runCommand "omarchy-menu-keybindings-script" { } ''
    install -Dm755 ${source}/bin/omarchy-menu-keybindings "$out/omarchy-menu-keybindings"
    substituteInPlace "$out/omarchy-menu-keybindings" \
      --replace-fail 'echo "SHIFT ALT,L,Copy URL from Web App,sendshortcut,SHIFT ALT,L,"' 'return 0' \
      --replace-fail 'echo "SHIFT ALT,D,Download Video from Web App,sendshortcut,SHIFT ALT,D,"' 'return 0'
    patchShebangs "$out/omarchy-menu-keybindings"
  '';
  omarchyShell = pkgs.writeShellApplication {
    name = "omarchy-shell";
    runtimeInputs = [
      pkgs.quickshell
      pkgs.systemd
      pkgs.gnugrep
      pkgs.coreutils
    ];
    text = ''
      config=${lib.escapeShellArg "${bundle}/shell"}
      quiet=0
      if [[ ''${1:-} == -q ]]; then
        quiet=1
        shift
      fi
      fail() {
        if (( quiet )); then
          exit 0
        fi
        printf '%s\n' "''$1" >&2
        exit 1
      }
      if (( ''$# < 2 )); then
        fail "Usage: omarchy-shell <target> <method> [args...]"
      fi
      if [[ ''$1 == shell && ( ''$2 == summon || ''$2 == toggle ) && ''$# -eq 3 ]]; then
        set -- "''$1" "''$2" "''$3" "{}"
      fi
      ready() {
        quickshell ipc --path "''$config" show 2>/dev/null | grep -qx 'target shell'
      }
      systemctl --user start hyprland-panels.service
      for _ in {1..30}; do
        ready && break
        sleep 0.1
      done
      if ! ready; then
        systemctl --user restart hyprland-panels.service
        for _ in {1..30}; do
          ready && break
          sleep 0.1
        done
      fi
      ready || fail "omarchy-shell is not running"
      if ! output=$(quickshell ipc --path "''$config" call "''$@"); then
        fail "omarchy-shell is not running"
      fi
      case ''$output in
        "Target not found." | "Function not found." | "Too few arguments provided"* | "Too many arguments provided"* | unknown)
          fail "''$output"
          ;;
      esac
      if (( ! quiet )) && [[ -n ''$output ]]; then
        printf '%s\n' "''$output"
      fi
    '';
  };
  keybindings = pkgs.writeShellApplication {
    name = "omarchy-menu-keybindings";
    runtimeInputs = [
      pkgs.hyprland
      pkgs.gawk
      pkgs.gnugrep
      pkgs.jq
      pkgs.perl
      pkgs.libxkbcommon
      pkgs.coreutils
      absent
      menuSelect
      omarchyShell
    ];
    text = ''
      exec ${keybindingsScript}/omarchy-menu-keybindings "''$@"
    '';
  };
  launcher = pkgs.writeShellApplication {
    name = "hypr-controls";
    runtimeInputs = [
      pkgs.quickshell
      pkgs.systemd
      pkgs.jq
      pkgs.gnugrep
      pkgs.coreutils
      pkgs.libnotify
      pkgs.procps
      agentCollectors
    ];
    text = ''
      export HYPR_CONTROLS_CONFIG=${bundle}/shell
      ${builtins.readFile (local + "/launch.sh")}
    '';
  };
in
{
  config = lib.mkIf isGraphicalLinux {
    # Keep nm-applet out of the profile's XDG autostart directories. The
    # connection editor remains available to the panel through its wrapper.
    home.packages = [
      launcher
      keybindings
      omarchyShell
      emojiOpen
      pkgs.quickshell
      pkgs.blueman
      pkgs.claude-code
    ];
    # Shared default for a new machine. A panel edit stays local; only the
    # untouched quantum-physics seed is replaced.
    home.activation.arxivScannerConfig = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      config_file=${lib.escapeShellArg "${config.xdg.configHome}/omarchy-arxiv-scanner/config.json"}
      if [[ -L $config_file ]]; then
        :
      elif [[ ! -e $config_file ]] || ${pkgs.diffutils}/bin/cmp -s ${arxivPreviousConfig} "$config_file"; then
        run install -D -m 0600 ${arxivConfig} "$config_file"
      fi
    '';
    systemd.user.services.omarchy-arxiv-scanner = {
      Unit.Description = "arXiv new-submissions scan";
      Service = {
        Type = "oneshot";
        ExecStart = "${arxivScripts}/bin/poll.py";
        ExecStartPost = "-${pkgs.procps}/bin/pkill -RTMIN+10 waybar";
        UMask = "0077";
        TimeoutStartSec = 400;
      };
    };
    systemd.user.timers.omarchy-arxiv-scanner = {
      Unit.Description = "Daily arXiv scan";
      Timer = {
        OnCalendar = "*-*-* 07:30:00";
        Persistent = true;
        AccuracySec = "5min";
      };
      Install.WantedBy = [ "timers.target" ];
    };
    # Start its pairing agent on demand via D-Bus, not a second login applet.
    xdg.configFile."autostart/blueman.desktop".text = ''
      [Desktop Entry]
      Type=Application
      Name=Blueman Applet
      Hidden=true
    '';
    # Authentication prompts remain visible even when notifications are muted.
    dconf.settings."org/blueman/general".notification-daemon = false;
    systemd.user.services.hypr-agent-usage = {
      Unit.Description = "Refresh AI subscription usage for the bar";
      Service = {
        Type = "oneshot";
        ExecStart = "${agentCollectors}/bin/omarchy-agent-usage-update";
        UMask = "0077";
        TimeoutStartSec = 120;
      };
    };
    systemd.user.timers.hypr-agent-usage = {
      Unit.Description = "Refresh AI subscription usage every fifteen minutes";
      Timer = {
        OnStartupSec = "45s";
        OnUnitActiveSec = "15min";
      };
      Install.WantedBy = [ "hyprland-session.target" ];
    };
    systemd.user.services.hyprland-panels = {
      Unit = {
        Description = "Themed control panels and authentication prompt";
        PartOf = [ "hyprland-session.target" ];
        After = [ "hyprland-session.target" ];
        ConditionEnvironment = "WAYLAND_DISPLAY";
        StartLimitIntervalSec = 30;
        StartLimitBurst = 3;
      };
      Service = {
        # The shell prompt must be the only authentication agent.
        ExecStartPre = [
          "-${pkgs.systemd}/bin/systemctl --user stop hyprpolkitagent.service"
        ];
        ExecStart = "${shell}/bin/hypr-controls-shell";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install.WantedBy = [ "hyprland-session.target" ];
    };
  };
}
