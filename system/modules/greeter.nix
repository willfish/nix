{
  config,
  lib,
  pkgs,
  ...
}:
let
  hostDefaults = import ../../home/user/themes/host-defaults.nix;
  rendered = import ./greeter-themes.nix { inherit lib pkgs; };
  selectionDir = "/var/lib/desktop-theme";
  selectionName = "william";
  selectionFile = "${selectionDir}/${selectionName}";
  runtimeDir = "/run/desktop-login";
  fallback = hostDefaults.forHost config.networking.hostName;
  manifest = pkgs.writeText "sddm-themes.json" (
    builtins.toJSON {
      inherit
        fallback
        selectionDir
        selectionName
        runtimeDir
        ;
      themes = lib.genAttrs rendered.names (name: {
        path = rendered.greeterThemePath name;
      });
    }
  );
in
{
  assertions = [
    {
      assertion = rendered.catalogue ? ${fallback};
      message = "login host default ${fallback} must be in the theme catalogue";
    }
    {
      assertion = !(config.services.displayManager.sddm.settings ? Autologin);
      message = "the login theme must not bypass password authentication";
    }
  ];

  services.displayManager = {
    autoLogin.enable = false;
    defaultSession = "hyprland";
    sddm = {
      enable = true;
      package = pkgs.kdePackages.sddm;
      theme = "${runtimeDir}/omarchy";
      settings.Users.EnableAvatars = false;
    };
  };
  services.libinput.enable = true;

  # Refresh only the allowlisted asset link, never restart a login session.
  systemd.services.desktop-login-theme = {
    description = "Select immutable login theme assets";
    after = [ "systemd-tmpfiles-setup.service" ];
    before = [ "display-manager.service" ];
    unitConfig.RequiresMountsFor = selectionDir;
    serviceConfig = {
      Type = "oneshot";
      ExecStart = "${pkgs.python3}/bin/python3 ${./greeter_select.py} ${manifest}";
      User = "root";
      UMask = "0022";
      NoNewPrivileges = true;
      PrivateTmp = true;
      ProtectHome = true;
      ProtectSystem = "strict";
      ReadWritePaths = [ runtimeDir ];
      RestrictAddressFamilies = [ "AF_UNIX" ];
    };
  };
  systemd.services.display-manager = {
    requires = [ "desktop-login-theme.service" ];
    after = [ "desktop-login-theme.service" ];
  };
  systemd.paths.desktop-login-theme = {
    wantedBy = [ "multi-user.target" ];
    pathConfig = {
      PathChanged = selectionFile;
      Unit = "desktop-login-theme.service";
    };
  };

  # Keep both directories root-owned. The desktop can write only the bounded
  # ID file, never a path, QML file, or privileged executable.
  systemd.tmpfiles.settings.desktop-theme = {
    ${runtimeDir}.d = {
      mode = "0755";
      user = "root";
      group = "root";
    };
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
