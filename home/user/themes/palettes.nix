# Discover bundled themes and all omarchy-theme-* flake inputs automatically.
let
  inputs = import ./inputs.nix;
  source = inputs.omarchy.outPath;
  directories = builtins.readDir "${source}/themes";
  names = builtins.filter (
    name:
    directories.${name} == "directory" && builtins.pathExists "${source}/themes/${name}/colors.toml"
  ) (builtins.attrNames directories);
  bundled = map (name: {
    inherit name;
    value = import ./import-theme.nix {
      inherit name;
      source = "${source}/themes/${name}";
      licenseSource = source;
    };
  }) names;
  communityNames = (builtins.fromJSON (builtins.readFile ./community.json)).themes;
  communityInputs = builtins.filter (name: name != "omarchy") (builtins.attrNames inputs);
  community = map (
    input:
    let
      slug = builtins.substring (builtins.stringLength "omarchy-theme-") (-1) input;
      name = "community-${slug}";
    in
    {
      inherit name;
      value = import ./import-theme.nix {
        inherit name;
        displayName = "${
          communityNames.${slug}.label or (builtins.replaceStrings [ "-" ] [ " " ] slug)
        } (Community)";
        source = inputs.${input}.outPath;
      };
    }
  ) communityInputs;
  entries = bundled ++ community;
  catalogue = builtins.listToAttrs entries;
in
assert builtins.length entries == builtins.length (builtins.attrNames catalogue);
catalogue
