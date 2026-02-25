{
  isLinux ? true,
  ...
}:
{
  imports = [
    ./config.nix
    ./email.nix
    ./environment.nix
    ./git.nix
    ./packages.nix
    ./programs.nix
    ./shells.nix
    ./tmux.nix
  ]
  ++ (
    if isLinux then
      [
        ./gnome.nix
        ./services.nix
      ]
    else
      [ ]
  );
}
