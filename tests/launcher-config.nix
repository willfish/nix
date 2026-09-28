let
  flake = builtins.getFlake (toString ../.);
  inspect =
    name:
    let
      c = flake.homeConfigurations.${name}.config;
      graphical = c.services.walker.enable;
      lib = flake.inputs.nixpkgs.lib;
      providers = c.services.walker.settings.providers;
      session = builtins.fromTOML (
        builtins.unsafeDiscardStringContext (
          builtins.readFile c.xdg.configFile."elephant/menus/session.toml".source
        )
      );
      desktop = builtins.fromTOML (
        builtins.unsafeDiscardStringContext (
          builtins.readFile c.xdg.configFile."elephant/menus/desktop.toml".source
        )
      );
      sessionEntries = (import ../home/config/hyprland/settings.nix).session.entries;
      matchesEntry =
        entry:
        builtins.any (
          row:
          row.text == entry.label + lib.optionalString entry.confirm " (confirm)"
          &&
            row.actions."menus:default"
            == "${c.home.profileDirectory}/bin/hypr-session select ${lib.escapeShellArg entry.action}"
          && row.icon != ""
        ) session.entries;
      hasAlias =
        label: alias:
        builtins.any (row: row.text == label && builtins.elem alias row.keywords) session.entries;
    in
    assert c.services.elephant.enable == graphical;
    assert
      graphical == (builtins.elem name [
        "william@andromeda"
        "william@foundation"
        "william@starfish"
        "william-linux"
      ]);
    assert
      !graphical || c.systemd.user.services.walker.Install.WantedBy == [ "hyprland-session.target" ];
    assert
      !graphical || c.systemd.user.services.elephant.Install.WantedBy == [ "hyprland-session.target" ];
    assert !graphical || c.systemd.user.services.walker.Unit.PartOf == [ "hyprland-session.target" ];
    assert !graphical || c.systemd.user.services.elephant.Unit.PartOf == [ "hyprland-session.target" ];
    assert
      !graphical
      || builtins.any (
        entry:
        flake.inputs.nixpkgs.lib.hasInfix (builtins.unsafeDiscardStringContext "${c.services.elephant.package}/bin") entry
      ) c.systemd.user.services.walker.Service.Environment;
    assert !graphical || c.gtk.iconTheme.name == "Yaru-blue";
    assert !graphical || c.gtk.iconTheme.package.pname == "yaru";
    assert !graphical || c.services.elephant.settings == { };
    assert !graphical || c.xdg.configFile ? "elephant/elephant.toml";
    assert !graphical || c.xdg.configFile ? "fuzzel/fuzzel.ini";
    assert graphical == (c.xdg.configFile ? "elephant/menus/session.toml");
    assert !graphical || builtins.elem "menus:session" providers.default;
    assert !graphical || !(builtins.elem "menus:session" providers.empty);
    assert !graphical || session.name == "session";
    assert !graphical || session.history == false;
    assert !graphical || builtins.length session.entries == builtins.length sessionEntries;
    assert !graphical || builtins.all matchesEntry sessionEntries;
    assert !graphical || hasAlias "Suspend" "sleep";
    assert !graphical || hasAlias "Power off (confirm)" "shutdown";
    assert !graphical || hasAlias "Power off (confirm)" "shut down";
    assert !graphical || hasAlias "Log out (confirm)" "logout";
    assert !graphical || hasAlias "Reboot (confirm)" "restart";
    assert
      !graphical
      ||
        providers.actions."menus:session" == [
          {
            action = "menus:default";
            label = "select";
            default = true;
            bind = "Return";
            after = "Close";
          }
        ];
    assert
      !graphical
      || builtins.any (
        row:
        row.text == "Session menu"
        &&
          row.keywords == [
            "session"
            "system menu"
          ]
        && row.actions."menus:default" == "${c.home.profileDirectory}/bin/hypr-session menu"
      ) desktop.entries;
    {
      inherit graphical;
      providers = if graphical then c.services.walker.settings.providers.default else [ ];
      globalConfig =
        if graphical then toString c.xdg.configFile."elephant/elephant.toml".source else null;
    };
in
builtins.listToAttrs (
  map
    (name: {
      inherit name;
      value = inspect name;
    })
    [
      "william@andromeda"
      "william@foundation"
      "william@starfish"
      "william@terminus"
      "william@relay"
      "william-darwin"
      "william-linux"
    ]
)
