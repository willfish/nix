let
  flake = builtins.getFlake (toString ../.);
  inspect =
    name:
    let
      c = flake.homeConfigurations.${name}.config;
      graphical = c.services.walker.enable;
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
