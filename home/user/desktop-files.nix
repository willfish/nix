{
  lib,
  pkgs,
  isGraphicalLinux,
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
  defaultImageViewer = "org.gnome.Loupe.desktop";
  pdfDesktop = "org.gnome.Evince.desktop";
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
  additionalMimeDefaults =
    lib.genAttrs [
      "text/html"
      "application/xhtml+xml"
      "x-scheme-handler/http"
      "x-scheme-handler/https"
    ] (_: browserDesktop)
    // lib.genAttrs [
      "application/msword"
      "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
      "application/vnd.openxmlformats-officedocument.wordprocessingml.template"
      "application/vnd.ms-word.document.macroEnabled.12"
      "application/vnd.oasis.opendocument.text"
      "application/vnd.oasis.opendocument.text-template"
      "application/rtf"
      "text/rtf"
    ] (_: "writer.desktop")
    // lib.genAttrs [
      "application/vnd.ms-excel"
      "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
      "application/vnd.openxmlformats-officedocument.spreadsheetml.template"
      "application/vnd.ms-excel.sheet.macroEnabled.12"
      "application/vnd.oasis.opendocument.spreadsheet"
      "application/vnd.oasis.opendocument.spreadsheet-template"
      "text/csv"
      "text/tab-separated-values"
    ] (_: "calc.desktop")
    // lib.genAttrs [
      "application/vnd.ms-powerpoint"
      "application/vnd.openxmlformats-officedocument.presentationml.presentation"
      "application/vnd.openxmlformats-officedocument.presentationml.template"
      "application/vnd.openxmlformats-officedocument.presentationml.slideshow"
      "application/vnd.ms-powerpoint.presentation.macroEnabled.12"
      "application/vnd.oasis.opendocument.presentation"
      "application/vnd.oasis.opendocument.presentation-template"
    ] (_: "impress.desktop")
    // lib.genAttrs [
      "application/x-bittorrent"
      "x-scheme-handler/magnet"
    ] (_: "org.qbittorrent.qBittorrent.desktop")
    // lib.genAttrs [
      "text/plain"
      "application/json"
      "application/yaml"
      "application/x-yaml"
      "text/yaml"
      "text/x-yaml"
    ] (_: "neovim-ghostty.desktop")
    // lib.genAttrs [
      "audio/aac"
      "audio/flac"
      "audio/mp4"
      "audio/mpeg"
      "audio/ogg"
      "audio/opus"
      "audio/wav"
      "audio/x-flac"
      "audio/x-matroska"
      "audio/x-wav"
      "video/mp4"
      "video/mpeg"
      "video/ogg"
      "video/quicktime"
      "video/webm"
      "video/x-matroska"
      "video/x-msvideo"
      "application/ogg"
    ] (_: "mpv.desktop")
    // lib.genAttrs [
      "application/zip"
      "application/x-tar"
      "application/gzip"
      "application/x-gzip"
      "application/x-compressed-tar"
      "application/x-bzip"
      "application/x-bzip2"
      "application/x-bzip-compressed-tar"
      "application/x-xz"
      "application/x-xz-compressed-tar"
      "application/zstd"
      "application/x-zstd-compressed-tar"
      "application/x-7z-compressed"
      "application/vnd.rar"
      "application/x-rar"
      "application/x-rar-compressed"
    ] (_: "org.gnome.FileRoller.desktop");
  existingMimeDefaults = {
    "inode/directory" = "org.gnome.Nautilus.desktop";
    "application/pdf" = pdfDesktop;
    "x-scheme-handler/mailto" = browserDesktop;
    "x-scheme-handler/tg" = telegramDesktop;
    "x-scheme-handler/tonsite" = telegramDesktop;
  };
  existingMimeAssociations = {
    "application/pdf" = pdfDesktop;
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
            pkgs.jq
            pkgs.rustc
            pkgs.stdenv.cc
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
  };

  xdg.dataFile = lib.optionalAttrs isGraphicalLinux {
    "applications/mimeapps.list".force = true;
  };

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
      exec = "ghostty -e cliamp";
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
    defaultApplications =
      existingMimeDefaults
      // additionalMimeDefaults
      // lib.genAttrs imageMimeTypes (_: defaultImageViewer);
    associations.added = existingMimeAssociations // additionalMimeDefaults;
  };
}
