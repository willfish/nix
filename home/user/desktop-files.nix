{
  lib,
  pkgs,
  isGraphicalLinux,
  hostName ? null,
  hostTheme,
  herdrThemeFile,
  ...
}:
let
  configDir = ../config;
  renderTheme = import ./themes/render.nix { inherit lib; };
  herdrTheme = (import ./themes/herdr.nix { }).configTheme hostTheme {
    light = renderTheme.herdr hostTheme.light;
    dark = renderTheme.herdr hostTheme.dark;
  };
  herdrConfig = builtins.fromTOML (builtins.readFile "${configDir}/herdr/config.toml");
  herdrConfigFile = (pkgs.formats.toml { }).generate "herdr-config.toml" (
    lib.recursiveUpdate herdrConfig { theme = herdrTheme; }
  );
  cliampRadio = pkgs.writeShellApplication {
    name = "cliamp-radio";
    runtimeInputs = [ pkgs.cliamp ];
    text = ''
      exec cliamp --auto-play "''${XDG_CONFIG_HOME:-$HOME/.config}/cliamp/playlists/forte-radio.m3u"
    '';
  };
  defaultImageViewer = "org.gnome.Loupe.desktop";
  pdfDesktop = "org.gnome.Evince.desktop";
  browserDesktop = "brave-browser.desktop";
  webApps = [
    {
      id = "aghbiahbpaijignceidepookljebhfak";
      name = "Google Drive";
      url = "https://drive.google.com/";
    }
    {
      id = "agimnkijcaahngcdmfeangaknmldooml";
      name = "YouTube";
      url = "https://www.youtube.com/";
    }
    {
      id = "fhihpiojkbmbpdjeoajapmgkhlnakfjf";
      name = "Sheets";
      url = "https://docs.google.com/spreadsheets/";
    }
    {
      id = "fmgjjmmmlfnkbppncabfkddbjimcfncm";
      name = "Gmail";
      url = "https://mail.google.com/";
    }
    {
      id = "kefjledonklijopmnomlcbpllchaibag";
      name = "Slides";
      url = "https://docs.google.com/presentation/";
    }
    {
      id = "mpnpojknpmmopombnjdcgaaiekajbnjb";
      name = "Docs";
      url = "https://docs.google.com/document/";
    }
  ];
  mimeDefaults = import ./mime-defaults.nix {
    inherit
      lib
      browserDesktop
      pdfDesktop
      defaultImageViewer
      ;
    telegramDesktop = "org.telegram.desktop.desktop";
    includeGimp = hostName == "andromeda";
  };
  sourceFile = source: {
    inherit source;
    force = true;
  };
in
{
  home.file = {
    ".config/herdr/config.toml" = sourceFile (
      if herdrThemeFile != null then herdrThemeFile else herdrConfigFile
    );
    ".config/herdr/plugins/config/herdr-navigator/config.toml" =
      sourceFile "${configDir}/herdr/agent-picker.toml";
  };

  home.activation = {
    installHerdrPlugins = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      herdrBin="${pkgs.herdr}/bin/herdr"
      if [ -x "$herdrBin" ]; then
        # Plugin installs download release assets. Do not pin cargo or rustc:
        # that puts the compiler and LLVM in every Home Manager generation.
        # install-plugins.sh fetches a toolchain with nix shell only if a
        # prebuilt install fails.
        herdrPluginPath="${
          lib.makeBinPath [
            pkgs.bash
            pkgs.coreutils
            pkgs.curl
            pkgs.gawk
            pkgs.git
            pkgs.gnugrep
            pkgs.gnutar
            pkgs.gzip
            pkgs.jq
          ]
        }:$PATH"
        export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
        export NIX_SSL_CERT_FILE="$SSL_CERT_FILE"
        export PATH="$herdrPluginPath"

        # Retired plugins / IDs from earlier experiments.
        "$herdrBin" plugin unlink fish.herdr-workspacex >/dev/null 2>&1 || true
        "$herdrBin" plugin uninstall rmarganti.herdr-pluck >/dev/null 2>&1 || true
        "$herdrBin" plugin unlink rmarganti.herdr-pluck >/dev/null 2>&1 || true

        if ! ${pkgs.bash}/bin/bash ${configDir}/herdr/install-plugins.sh \
          "$herdrBin" ${configDir}/herdr/plugins.json; then
          echo "warning: pinned Herdr plugins need reconciliation; inspect installed state before retrying" >&2
        fi
        "$herdrBin" server reload-config >/dev/null 2>&1 || true
      fi
    '';

  };

  xdg.configFile = lib.optionalAttrs isGraphicalLinux {
    "xdg-terminal-exec/default".text = "com.mitchellh.ghostty.desktop";
    "mimeapps.list".force = true;
    # Direct file, not the config directory: a directory copy drops untracked files.
    "cliamp/radios.toml" = {
      text = builtins.readFile ../config/cliamp/radios.toml;
      force = true;
    };
    "cliamp/playlists/forte-radio.m3u" = {
      text = builtins.readFile ../config/cliamp/playlists/forte-radio.m3u;
      force = true;
    };
  };

  home.packages = lib.optional isGraphicalLinux cliampRadio;

  xdg.dataFile = lib.optionalAttrs isGraphicalLinux (
    {
      "applications/mimeapps.list".force = true;
    }
    // builtins.listToAttrs (
      map (app: {
        name = "applications/chrome-${app.id}-Default.desktop";
        value = {
          force = true;
          text = ''
            [Desktop Entry]
            Type=Application
            Name=${app.name}
            Exec=brave --app=${app.url}
            Icon=chrome-${app.id}-Default
            Terminal=false
            Categories=Network;WebBrowser;
          '';
        };
      }) webApps
    )
  );

  xdg.desktopEntries = lib.mkIf isGraphicalLinux {
    neovim-ghostty = {
      name = "Neovim";
      genericName = "Text Editor";
      exec = "ghostty -e nvim %F";
      icon = "nvim";
      terminal = false;
      categories = [
        "Utility"
        "TextEditor"
      ];
    };
    cliamp = {
      name = "CLIamp";
      genericName = "Music Player";
      exec = (import ../config/hyprland/settings.nix).cliampCommand;
      icon = "audio-x-generic";
      terminal = false;
      categories = [
        "AudioVideo"
        "Audio"
        "Player"
      ];
    };
  };

  xdg.mimeApps = lib.mkIf isGraphicalLinux {
    enable = true;
    defaultApplications = mimeDefaults;
    associations.added = mimeDefaults;
    # Himalaya also claims mailto and .eml. Mail is Gmail in Brave, so drop that claim.
    associations.removed = {
      "message/rfc822" = [ "himalaya.desktop" ];
      "x-scheme-handler/mailto" = [ "himalaya.desktop" ];
    };
  };
}
