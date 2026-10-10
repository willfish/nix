# Content-addressed copy of supported theme files. The store path follows the
# locked git tree, not nixpkgs. Image conversion stays with the tool that
# needs it, so a nixpkgs bump does not rebuild this path.
{ lib }:
theme:
let
  root = theme.source;
  relative = path: lib.removePrefix "${toString root}/" (toString path);
  image = file: builtins.match ".*\\.(png|jpg|jpeg|webp|PNG|JPG|JPEG|WEBP)" file != null;
  license = file: builtins.match "(LICENSE|COPYING|NOTICE)(\\..*)?" file != null;
in
builtins.path {
  name = "omarchy-theme-${theme.name}";
  path = root;
  filter =
    path: type:
    let
      base = baseNameOf path;
      rel = relative path;
    in
    toString path == toString root
    || (type == "directory" && rel == "backgrounds")
    || (
      type == "regular"
      && (
        rel == "colors.toml"
        || rel == "unlock.png"
        || rel == "btop.theme"
        || (license base && !lib.hasInfix "/" rel)
        || (lib.hasPrefix "backgrounds/" rel && image base)
      )
    );
}
