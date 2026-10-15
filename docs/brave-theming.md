# Brave desktop palette (Linux)

Brave's native toolbar, tab area, address field, menus and selection follow the
palette and light/dark mode selected by `theme-menu`. In **Settings → Appearance**,
select **Use GTK** once per Brave profile. Classic, Qt and extension themes bypass
this integration. No browser restart is needed for later palette changes.

Home Manager installs a named GTK theme for each palette and mode, based on
adw-gtk3. `theme-menu` publishes its name through the existing desktop GTK setting.
The name hashes the generated stylesheet, so even dark-to-dark palette changes
invalidate Chromium's native colour cache. Unchanged themes retain the same name.
GTK applications continue to use the same base toolkit theme and shared colours.

`home/user/themes/gtk.nix` maps desktop tokens to Chromium's synthetic GTK nodes.
These rules belong in the named theme, not the user `gtk.css` provider, which GTK
loads only once. `home/user/themes/runtime.nix` installs the themes and includes
their names in the normal atomic session bundles. There is no extension, extra
service, browser profile rewrite, runtime debugging dependency or modified Brave
binary.

This styles browser controls, not websites. Brave's window frame, WebUI pages and
some branded controls retain Brave's own colours. Private and Tor windows retain
their native identity colours. Switching to **Use Classic** opts a profile out; switching back
to **Use GTK** resumes the desktop palette.

When upgrading Brave, check the selectors in Chromium's
[`ui/gtk/gtk_ui.cc`](https://github.com/chromium/chromium/blob/main/ui/gtk/gtk_ui.cc)
and [`ui/gtk/gtk_color_mixers.cc`](https://github.com/chromium/chromium/blob/main/ui/gtk/gtk_color_mixers.cc).
The solid background image uses the rendered-colour sampling in
[`ui/gtk/gtk_util.cc`](https://github.com/chromium/chromium/blob/main/ui/gtk/gtk_util.cc)
to cover stale user-provider backgrounds in already-running browsers.
After `hmswitch`, check a dark-to-dark palette change, a light palette, address-field
selection and an open browser menu in the running GTK-themed Brave. Preserve
security indicators and private-window distinctions rather than recolouring
every control indiscriminately.
