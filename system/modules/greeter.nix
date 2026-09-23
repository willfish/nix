{
  config,
  lib,
  pkgs,
  ...
}:
let
  hostDefaults = import ../../home/user/themes/host-defaults.nix;
  rendered = import ./greeter-themes.nix {
    inherit lib pkgs;
    timeZone = config.time.timeZone;
  };
  selectionDir = "/var/lib/desktop-theme";
  selectionName = "william";
  selectionFile = "${selectionDir}/${selectionName}";
  fallback = hostDefaults.forHost config.networking.hostName;
  fallbackTheme = rendered.catalogue.${fallback};
  themes = lib.genAttrs rendered.names (name: {
    nativeMode = rendered.nativeMode name;
    config = "${rendered.configFile name}";
    css = "${rendered.cssFile name}";
  });
  manifest = pkgs.writeText "greeter-themes.json" (
    builtins.toJSON {
      inherit fallback selectionDir selectionName;
      sessionShare = "${config.services.displayManager.sessionData.desktops}/share";
      dbus = lib.getExe' pkgs.dbus "dbus-run-session";
      cage = lib.getExe pkgs.cage;
      cageArgs = config.programs.regreet.cageArgs;
      regreet = lib.getExe config.programs.regreet.package;
      inherit themes;
    }
  );
  launcher = pkgs.writeShellScript "regreet-themed" ''
    exec ${pkgs.python3}/bin/python3 ${./greeter_select.py} ${manifest}
  '';
in
{
  assertions = [
    {
      assertion = rendered.catalogue ? ${fallback};
      message = "greeter host default ${fallback} is not in the Omarchy catalogue";
    }
  ];

  programs.regreet = {
    enable = true;
    theme = {
      package = pkgs.adw-gtk3;
      name = rendered.gtkThemeName fallbackTheme.nativeMode;
    };
    font = {
      package = pkgs."ubuntu-classic";
      name = rendered.configuredAppearance.font;
      size = rendered.configuredAppearance.fontSize;
    };
    settings = rendered.settingsFor fallback;
  };

  services.greetd.settings.default_session.command = lib.mkForce launcher;
  security.pam.services.greetd.enableGnomeKeyring = true;
  services.libinput.enable = true;

  # Type f creates the file only when it is missing. It must not be f+, which
  # would reset a saved ID on the next boot. An empty or rejected file falls
  # back in the reader; nothing privileged rewrites it.
  systemd.tmpfiles.settings.desktop-theme = {
    ${selectionDir}.d = {
      mode = "0755";
      user = "root";
      group = "root";
    };
    ${selectionFile}.f = {
      mode = "0644";
      user = "william";
      group = config.users.users.william.group;
      argument = "${fallback}\n";
    };
  };
}
