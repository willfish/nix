{
  lib,
  pkgs,
  hostName ? null,
  ...
}:
let
  inherit (pkgs) stdenv;
  configDir = ../config;
  mkHerdrTheme = darkName: lightName: {
    name = darkName;
    auto_switch = true;
    dark_name = darkName;
    light_name = lightName;
  };
  defaultHerdrTheme = mkHerdrTheme "rose-pine" "rose-pine-dawn";
  herdrThemes = {
    andromeda = defaultHerdrTheme;
    foundation = mkHerdrTheme "tokyo-night" "tokyo-night-day";
    starfish = mkHerdrTheme "solarized" "solarized-light";
    terminus = mkHerdrTheme "catppuccin" "catppuccin-latte";
    relay = mkHerdrTheme "gruvbox" "gruvbox-light";
  };
  herdrTheme =
    if hostName != null && builtins.hasAttr hostName herdrThemes then
      herdrThemes.${hostName}
    else
      defaultHerdrTheme;
  herdrConfig = builtins.fromTOML (builtins.readFile "${configDir}/herdr/config.toml");
  herdrConfigFile = (pkgs.formats.toml { }).generate "herdr-config.toml" (
    lib.recursiveUpdate herdrConfig { theme = herdrTheme; }
  );
  defaultImageViewer = "org.gnome.Loupe.desktop";
  browserDesktop = "brave-browser.desktop";
  telegramDesktop = "org.telegram.desktop.desktop";
  imageMimeTypes = [
    "image/avif"
    "image/bmp"
    "image/gif"
    "image/heic"
    "image/jpeg"
    "image/jxl"
    "image/png"
    "image/qoi"
    "image/svg+xml"
    "image/svg+xml-compressed"
    "image/tiff"
    "image/vnd.microsoft.icon"
    "image/vnd-ms.dds"
    "image/vnd.radiance"
    "image/webp"
    "image/x-dds"
    "image/x-exr"
    "image/x-portable-anymap"
    "image/x-portable-bitmap"
    "image/x-portable-graymap"
    "image/x-portable-pixmap"
    "image/x-qoi"
    "image/x-tga"
  ];
  existingMimeDefaults = {
    "x-scheme-handler/mailto" = browserDesktop;
    "x-scheme-handler/tg" = telegramDesktop;
    "x-scheme-handler/tonsite" = telegramDesktop;
  };
  existingMimeAssociations = {
    "x-scheme-handler/tg" = telegramDesktop;
    "x-scheme-handler/tonsite" = telegramDesktop;
  };
  sourceFile = source: {
    inherit source;
    force = true;
  };
in
{
  home.file = {
    ".config/herdr/config.toml" = sourceFile herdrConfigFile;
  }
  // lib.optionalAttrs stdenv.isDarwin {
    ".aerospace.toml" = sourceFile "${configDir}/aerospace/aerospace.toml";
  };

  home.activation = {
    installHerdrPlugins = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      herdrBin="${pkgs.herdr}/bin/herdr"
      if [ -x "$herdrBin" ]; then
        # Plugin installs may download release assets or cargo-build when no
        # prebuilt binary matches. HM activation can omit tools like awk/curl/tar,
        # so pin download deps and a cargo toolchain.
        herdrPluginPath="${
          lib.makeBinPath [
            pkgs.bash
            pkgs.cargo
            pkgs.coreutils
            pkgs.curl
            pkgs.gawk
            pkgs.git
            pkgs.gnugrep
            pkgs.gnutar
            pkgs.gzip
            pkgs.rustc
            pkgs.stdenv.cc
          ]
        }:$PATH"
        export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
        export NIX_SSL_CERT_FILE="$SSL_CERT_FILE"
        export PATH="$herdrPluginPath"

        installHerdrPlugin() {
          if ! "$herdrBin" plugin install "$1" --yes >/dev/null; then
            echo "warning: failed to install Herdr plugin $1; continuing Home Manager activation" >&2
            return 1
          fi
        }

        # Retired plugins / IDs from earlier experiments.
        "$herdrBin" plugin unlink fish.herdr-workspacex >/dev/null 2>&1 || true
        "$herdrBin" plugin uninstall rmarganti.herdr-pluck >/dev/null 2>&1 || true
        "$herdrBin" plugin unlink rmarganti.herdr-pluck >/dev/null 2>&1 || true

        if ! "$herdrBin" plugin list --plugin willfish.herdr-workspacex --json 2>/dev/null | grep -Fq '"kind":"github"'; then
          "$herdrBin" plugin unlink willfish.herdr-workspacex >/dev/null 2>&1 || true
          installHerdrPlugin willfish/herdr-workspacex || true
        fi
        if ! "$herdrBin" plugin list --plugin willfish.herdr-navigator --json 2>/dev/null | grep -Fq '"kind":"github"'; then
          "$herdrBin" plugin unlink willfish.herdr-navigator >/dev/null 2>&1 || true
          installHerdrPlugin willfish/herdr-navigator || true
        fi
        if ! "$herdrBin" plugin list --plugin hotchpotch.herdr-tiny-fingers --json 2>/dev/null | grep -Fq '"kind":"github"'; then
          "$herdrBin" plugin unlink hotchpotch.herdr-tiny-fingers >/dev/null 2>&1 || true
          installHerdrPlugin hotchpotch/herdr-tiny-fingers || true
        fi
        if ! "$herdrBin" plugin list --plugin persiyanov.reviewr --json 2>/dev/null | grep -Fq '"kind":"github"'; then
          "$herdrBin" plugin unlink persiyanov.reviewr >/dev/null 2>&1 || true
          installHerdrPlugin persiyanov/herdr-reviewr || true
        fi
        "$herdrBin" server reload-config >/dev/null 2>&1 || true
      fi
    '';

    # cosmic-screenshot crashes on launch when CosmicPortal remembers Window mode
    # (NixOS/nixpkgs#409441). Reset only that broken persisted choice.
    fixCosmicScreenshotPortalConfig = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      portalScreenshot="$HOME/.config/cosmic/com.system76.CosmicPortal/v1/screenshot"
      if [ -f "$portalScreenshot" ] && grep -q 'choice: Window' "$portalScreenshot"; then
        ${pkgs.gnused}/bin/sed -i 's/choice: Window/choice: Rectangle/' "$portalScreenshot"
        echo "Reset cosmic screenshot mode from Window to Rectangle (avoids crash loop)"
      elif [ ! -f "$portalScreenshot" ]; then
        mkdir -p "$(dirname "$portalScreenshot")"
        cp ${configDir}/cosmic/portal-screenshot "$portalScreenshot"
        echo "Installed default cosmic screenshot portal config"
      fi
    '';
  };

  xdg.configFile = {
    "cosmic/com.system76.CosmicSettings.Shortcuts/v1/custom" = {
      source =
        if
          builtins.elem hostName [
            "andromeda"
            "foundation"
          ]
        then
          pkgs.writeText "cosmic-shortcuts-with-voice" (
            builtins.replaceStrings
              [
                ''
                  key: "space",
                      ): Disable,''
                "modifiers: [\n            Super,\n        ],\n        key: \"r\",\n    ): Disable,"
              ]
              [
                ''
                  key: "space",
                      ): Spawn("codex-voice record"),
                      (
                          modifiers: [Super, Shift],
                          key: "space",
                      ): Spawn("codex-voice send"),''
                "modifiers: [\n            Super,\n        ],\n        key: \"r\",\n    ): Spawn(\"codex-voice read\"),"
              ]
              (builtins.readFile "${configDir}/cosmic/shortcuts")
          )
        else
          "${configDir}/cosmic/shortcuts";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/autotile" = {
      source = "${configDir}/cosmic/autotile";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/autotile_behavior" = {
      source = "${configDir}/cosmic/autotile_behavior";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/active_hint" = {
      source = "${configDir}/cosmic/active_hint";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/focus_follows_cursor" = {
      source = "${configDir}/cosmic/focus_follows_cursor";
      force = true;
    };
    "cosmic/com.system76.CosmicComp/v1/cursor_follows_focus" = {
      source = "${configDir}/cosmic/cursor_follows_focus";
      force = true;
    };
    "cosmic/com.system76.CosmicPanel.Panel/v1" = {
      source = "${configDir}/cosmic/panel";
      recursive = true;
      force = true;
    };
    "cosmic/com.system76.CosmicPanel.Dock/v1" = {
      source = "${configDir}/cosmic/dock";
      recursive = true;
      force = true;
    };
    "cosmic/com.system76.CosmicPanel/v1/entries" = {
      source = "${configDir}/cosmic/panel-entries";
      force = true;
    };
    "cosmic/com.system76.CosmicTheme.Mode/v1/is_dark" = {
      source = "${configDir}/cosmic/theme-mode";
      force = true;
    };
    "cosmic/com.system76.CosmicTheme.Dark/v1" = {
      source = "${configDir}/cosmic/theme-dark";
      recursive = true;
      force = true;
    };
    "xdg-terminal-exec/default".text = "com.mitchellh.ghostty.desktop";
  }
  // lib.optionalAttrs stdenv.isLinux {
    "mimeapps.list".force = true;
  };

  xdg.dataFile = lib.optionalAttrs stdenv.isLinux {
    "applications/mimeapps.list".force = true;
  };

  xdg.mimeApps = lib.mkIf stdenv.isLinux {
    enable = true;
    defaultApplications = existingMimeDefaults // lib.genAttrs imageMimeTypes (_: defaultImageViewer);
    associations.added = existingMimeAssociations;
  };
}
