# Chromium queries synthetic GTK nodes, not just GTK's named colours. Keep
# these rules in the named theme, not the once-loaded user gtk.css provider.
{ lib, pkgs }:
let
  selection = (import ./render.nix { inherit lib; }).selectionPair;
  css =
    {
      mode,
      palette,
      shared,
      version,
    }:
    let
      base = if mode == "dark" then "adw-gtk3-dark" else "adw-gtk3";
      highlight = selection palette;
    in
    ''
      @import url("${pkgs.adw-gtk3}/share/themes/${base}/gtk-${version}/gtk.css");
      ${shared}

      /* Chromium samples rendered GTK backgrounds (gtk_util.cc). A solid
         image also covers a background-color cached in the user provider,
         allowing existing browser processes to migrate without a restart. */
      window.background.chromium {
        background-image: linear-gradient(#${palette.base00}, #${palette.base00});
        background-color: #${palette.base00};
        color: #${palette.base05};
      }
      window.background.chromium label,
      window.background.chromium button,
      window.background.chromium textview text {
        color: #${palette.base05};
      }
      window.background.chromium label:disabled,
      window.background.chromium button:disabled label,
      window.background.chromium menu menuitem:disabled label {
        color: #${palette.base04};
      }
      window.background.chromium headerbar,
      window.background.chromium menubar,
      window.background.chromium menu,
      tooltip.background,
      window.background.chromium button,
      window.background.chromium entry,
      window.background.chromium textview.view {
        background-image: none;
        background-color: #${palette.base01};
        color: #${palette.base05};
        border-color: #${palette.base03};
      }
      window.background.chromium headerbar:backdrop,
      window.background.chromium menubar:backdrop {
        background-color: #${palette.base00};
      }
      window.background.chromium entry:focus,
      window.background.chromium entry:focus-within {
        border-color: #${palette.base0D};
      }
      window.background.chromium label.link:link {
        color: #${palette.base0D};
      }
      window.background.chromium selection,
      window.background.chromium treeview.view.cell:selected,
      window.background.chromium menu menuitem:hover {
        background-color: #${highlight.bg};
        color: #${highlight.fg};
      }
      window.background.chromium treeview.view.cell:selected label,
      window.background.chromium menu menuitem:hover label {
        color: #${highlight.fg};
      }
      window.background.chromium menu menuitem label,
      tooltip.background label {
        color: #${palette.base05};
      }
      window.background.chromium separator {
        background-color: #${palette.base03};
        border-color: #${palette.base03};
      }
    '';
in
{
  name =
    args:
    "dotfiles-${args.mode}-${
      builtins.substring 0 16 (builtins.hashString "sha256" (css (args // { version = "3.0"; })))
    }";
  stylesheet = css;
}
