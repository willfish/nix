#define _DEFAULT_SOURCE
#include "theme.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool enabled(void) {
  return !g_strcmp0(g_getenv("THEME_MENU_PUBLISH"), "1");
}
static bool which(const char *name) {
  char *path = g_find_program_in_path(name);
  bool found = path != NULL;
  g_free(path);
  return found;
}
static char *detail(Command *c) {
  return trim(*c->err
                  ? c->err
                  : (c->failure ? c->failure
                                : "Command returned a non-zero exit status"));
}
static void warning_detail(Themes *t, const char *message, Command *c) {
  char *d = detail(c), *s = g_strconcat(message, d, NULL);
  warn(t, s);
  g_free(s);
  g_free(d);
}
static Command *run(const char *const *args, guint timeout) {
  return command(args, NULL, timeout, false);
}
GHashTable *theme_variables(const char *text) {
  GHashTable *values =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  char **rows = lines(text);
  for (size_t i = 0; rows[i]; i++) {
    char *s = trim(rows[i]), *eq = strchr(s, '=');
    if (g_str_has_prefix(s, "$theme_") && eq) {
      *eq = 0;
      g_hash_table_replace(values, trim(s + 1), trim(eq + 1));
    }
    g_free(s);
  }
  g_strfreev(rows);
  return values;
}
static char *theme_id(Themes *t, const char *selected) {
  const char *key = selected;
  if (!strcmp(selected, "default"))
    key = yyjson_get_str(field(t->catalogue, "default"));
  yyjson_val *palettes = field(t->catalogue, "palettes");
  if (key && !strcmp(key, "solarized") && !field(palettes, key) &&
      field(palettes, "osaka-jade"))
    key = "osaka-jade";
  return key && field(palettes, key) ? g_strdup(key) : NULL;
}
void publish_greeter(Themes *t, const char *selected) {
  if (!enabled())
    return;
  char *id = theme_id(t, selected);
  if (!id) {
    warn(t, "Refused to publish an unknown greeter theme ID.");
    return;
  }
  size_t len = strlen(id);
  bool valid = len > 0 && len + 1 <= 64;
  bool previous_hyphen = true;
  for (size_t i = 0; i < len && valid; i++) {
    unsigned char c = (unsigned char)id[i];
    bool letter = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!letter && (c != '-' || previous_hyphen))
      valid = false;
    previous_hyphen = c == '-';
  }
  if (previous_hyphen)
    valid = false;
  if (!valid) {
    warn(t, "Refused to publish a greeter theme ID outside the catalogue.");
    g_free(id);
    return;
  }
  struct stat st;
  if (lstat(t->greeter, &st)) {
    g_free(id);
    return;
  }
  if (!S_ISREG(st.st_mode)) {
    warn(t, "Greeter theme path is not a regular file; left unchanged.");
    g_free(id);
    return;
  }
  int fd = open(t->greeter, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno != ENOENT)
      warn(t, errno == ELOOP || errno == EMLINK
                  ? "Greeter theme path is a symlink; left unchanged."
                  : "Could not update the greeter theme ID.");
    g_free(id);
    return;
  }
  if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    warn(t, "Greeter theme path is not a regular file; left unchanged.");
    close(fd);
    g_free(id);
    return;
  }
  char *payload = g_strconcat(id, "\n", NULL);
  size_t done = 0;
  bool ok = true;
  while (done < len + 1) {
    ssize_t n = write(fd, payload + done, len + 1 - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    done += (size_t)n;
  }
  if (ok && ftruncate(fd, (off_t)(len + 1)))
    ok = false;
  if (!ok)
    warn(t, "Could not update the greeter theme ID.");
  close(fd);
  g_free(payload);
  g_free(id);
}
static bool reset_wallpaper(Themes *t) {
  char *selected = selection(t), *mode = current_mode(t);
  yyjson_val *p = selected ? resolve(t, selected) : NULL,
             *s = p && mode ? field(session(p, mode), "wallpaper.png") : NULL;
  g_free(selected);
  g_free(mode);
  if (t->error)
    return false;
  const char *source = yyjson_get_str(s);
  char *live = path_join(t->state, "active/wallpaper-live.png"),
       *current = path_join(t->state, "wallpaper-current"),
       *stamp = path_join(t->state, "wallpaper-theme"),
       *link = path_join(t->state, "wallpaper-source");
  bool ok = true;
  if (!source || !g_str_has_prefix(source, "nix-theme:")) {
    ok = remove_file(t, live) && remove_file(t, current) &&
         remove_file(t, stamp);
    if (ok && (exists(link) || path_is_symlink(link)))
      ok = remove_file(t, link);
  } else {
    const char *name = source + 10;
    if (*name && !strchr(name, '/') && !strstr(name, "..") &&
        !strchr(name, ' ')) {
      char *previous = exists(stamp) ? read_text(t, stamp) : g_strdup(""),
           *old = previous ? trim(previous) : NULL;
      g_free(previous);
      if (old && strcmp(old, name)) {
        char *value = g_strconcat(name, "\n", NULL);
        GBytes *data = g_bytes_new_take(value, strlen(value));
        ok = remove_file(t, live) && remove_file(t, current) &&
             atomic_write(t, stamp, data);
        g_bytes_unref(data);
      }
      g_free(old);
      if (!t->error && ok) {
        const char *flake = g_getenv("THEME_WALLPAPER_FLAKE");
        char *default_flake =
                 flake ? NULL : path_join(g_get_home_dir(), ".dotfiles"),
             *target = g_strdup_printf("%s#theme-%s",
                                       flake ? flake : default_flake, name);
        const char *argv[] = {"nix", "build", "--out-link", link, target, NULL};
        Command *c = command(argv, NULL, 120000, true);
        ok = checked(t, c);
        command_free(c);
        g_free(default_flake);
        g_free(target);
      }
    }
  }
  g_free(live);
  g_free(current);
  g_free(stamp);
  g_free(link);
  return ok && !t->error;
}
static bool hyprland(void) {
  const char *signature = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (signature && *signature)
    return true;
  char *desktop = g_utf8_strdown(
      g_getenv("XDG_CURRENT_DESKTOP") ? g_getenv("XDG_CURRENT_DESKTOP") : "",
      -1);
  for (char *s = desktop; *s; s++)
    if (*s == ':')
      *s = ';';
  char **parts = g_strsplit(desktop, ";", -1);
  bool found = false;
  for (size_t i = 0; parts[i]; i++) {
    char *s = trim(parts[i]);
    if (!strcmp(s, "hyprland"))
      found = true;
    g_free(s);
  }
  g_strfreev(parts);
  g_free(desktop);
  return found;
}
static const char *variable(GHashTable *values, const char *key,
                            const char *fallback) {
  const char *value = g_hash_table_lookup(values, key);
  return value ? value : fallback;
}
static void publish_gtk(Themes *t, const char *mode, GHashTable *v) {
  bool dark = !strcmp(mode, "dark");
  const char *keys[] = {"color-scheme", "gtk-theme", "font-name",
                        "monospace-font-name"};
  char *font = g_strdup_printf("%s %s",
                               variable(v, "theme_font", "Liberation Sans"),
                               variable(v, "theme_font_size", "12")),
       *mono = g_strdup_printf(
           "%s %s", variable(v, "theme_mono_font", "JetBrainsMono Nerd Font"),
           variable(v, "theme_font_size", "12"));
  const char *values[] = {
      variable(v, "theme_color_scheme", dark ? "prefer-dark" : "prefer-light"),
      variable(v, "theme_gtk", dark ? "adw-gtk3-dark" : "adw-gtk3"), font,
      mono};
  for (size_t i = 0; i < 4; i++) {
    const char *args[] = {"gsettings", "set",     "org.gnome.desktop.interface",
                          keys[i],     values[i], NULL};
    Command *c = run(args, 5000);
    bool ok = c->ok;
    if (!ok)
      warning_detail(t,
                     "Could not publish org.gnome.desktop.interface. Needs "
                     "gsettings (glib), gsettings-desktop-schemas, and dconf. ",
                     c);
    command_free(c);
    if (!ok)
      break;
  }
  g_free(font);
  g_free(mono);
}
static void install_gtk_css(Themes *t) {
  char *source = path_join(t->state, "active/gtk.css");
  if (!exists(source)) {
    g_free(source);
    return;
  }
  GBytes *content = read_bytes(t, source);
  g_free(source);
  if (!content)
    return;
  const char *paths[] = {"gtk-3.0/gtk.css", "gtk-4.0/gtk.css"};
  for (size_t i = 0; i < 2 && !t->error; i++) {
    char *dest = path_join(t->config, paths[i]);
    if (path_is_symlink(dest)) {
      g_free(dest);
      continue;
    }
    if (exists(dest)) {
      GBytes *old = read_bytes(t, dest);
      if (!old) {
        g_free(dest);
        break;
      }
      gsize len;
      const char *data = g_bytes_get_data(old, &len);
      const char *marker = "Shared GTK and Brave colours and fonts";
      size_t marker_len = strlen(marker);
      bool owned = false;
      for (size_t offset = 0; offset <= len && len - offset >= marker_len;
           offset++)
        if (!memcmp(data + offset, marker, marker_len)) {
          owned = true;
          break;
        }
      g_bytes_unref(old);
      if (!owned) {
        char *s = g_strdup_printf(
            "Preserved custom %s; it overrides desktop colours.", paths[i]);
        warn(t, s);
        g_free(s);
        g_free(dest);
        continue;
      }
    }
    if (!atomic_write(t, dest, content)) {
      char *s = g_strdup_printf("Could not install %s: %s", paths[i], t->error);
      warn(t, s);
      g_free(s);
      g_clear_pointer(&t->error, g_free);
    }
    g_free(dest);
  }
  g_bytes_unref(content);
}
static void reload_hyprland(Themes *t, GHashTable *values) {
  if (!which("hyprctl")) {
    warn(t, "hyprctl is not installed. Hyprland colours apply on the next "
            "reload. Package: hyprland.");
    return;
  }
  const char *keys[] = {"theme_active_border", "theme_inactive_border",
                        "theme_background", "theme_rounding",
                        "theme_border_size"},
             *keywords[] = {"general:col.active_border",
                            "general:col.inactive_border",
                            "misc:background_color", "decoration:rounding",
                            "general:border_size"};
  for (size_t i = 0; i < 5; i++) {
    const char *value = g_hash_table_lookup(values, keys[i]);
    if (!value || !*value)
      continue;
    const char *args[] = {"hyprctl", "keyword", keywords[i], value, NULL};
    Command *c = run(args, 5000);
    bool ok = c->ok;
    if (!ok)
      warning_detail(t, "hyprctl could not apply theme colours. ", c);
    command_free(c);
    if (!ok)
      break;
  }
}
static void reload_waybar(Themes *t) {
  if (!which("systemctl")) {
    warn(t, "systemctl is not installed, so Waybar was not reloaded. Package: "
            "systemd.");
    return;
  }
  const char *args[] = {"systemctl",        "--user",         "kill",
                        "--signal=SIGUSR2", "waybar.service", NULL};
  Command *c = run(args, 5000);
  if (!c->ok)
    warning_detail(t,
                   "Could not signal user unit waybar.service. The Hyprland "
                   "module should run Waybar as that unit. ",
                   c);
  command_free(c);
}
bool publish_session(Themes *t) {
  if (!enabled())
    return true;
  if (!reset_wallpaper(t))
    return false;
  char *selected = selection(t);
  if (!selected)
    return false;
  publish_greeter(t, selected);
  g_free(selected);
  if (!hyprland())
    return true;
  char *mode = current_mode(t),
       *conf = path_join(t->state, "active/hyprland.conf");
  char *text = read_text(t, conf);
  if (!text && !exists(conf) && !path_is_symlink(conf)) {
    g_clear_pointer(&t->error, g_free);
    text = g_strdup("");
    warn(t, "Hyprland theme file is missing. Run theme-menu --reapply.");
  }
  g_free(conf);
  if (!text || !mode) {
    g_free(text);
    g_free(mode);
    return false;
  }
  GHashTable *variables = theme_variables(text);
  g_free(text);
  publish_gtk(t, mode, variables);
  install_gtk_css(t);
  if (!t->error) {
    reload_hyprland(t, variables);
    reload_waybar(t);
  }
  g_hash_table_destroy(variables);
  g_free(mode);
  if (t->error)
    return false;
  if (which("walker")) {
    const char *args[] = {"systemctl", "--user", "try-restart",
                          "walker.service", NULL};
    Command *c = run(args, 10000);
    if (!c->ok)
      warn(t, "Launcher will use the theme on next startup.");
    command_free(c);
  }
  const char *reset[] = {"systemctl", "--user", "reset-failed",
                         "hypr-wallpaper.service", NULL},
             *restart[] = {"systemctl", "--user", "restart",
                           "hypr-wallpaper.service", NULL};
  Command *c = run(reset, 10000);
  bool available = !c->failure && !c->timed_out;
  command_free(c);
  if (available) {
    c = run(restart, 10000);
    if (!c->ok)
      warn(t, "Could not refresh the Hyprland wallpaper.");
    command_free(c);
  } else
    warn(t, "Could not refresh the Hyprland wallpaper.");
  const char *mako[] = {"makoctl", "reload", NULL};
  c = run(mako, 5000);
  bool ok = c->ok;
  command_free(c);
  if (!ok) {
    const char *omapager[] = {"systemctl", "--user",           "is-active",
                              "--quiet",   "omapager.service", NULL};
    c = run(omapager, 5000);
    if (c->failure || c->timed_out) {
      checked(t, c);
      command_free(c);
      return false;
    }
    if (c->status)
      warn(t, "Notifications will use the selected theme on next startup.");
    command_free(c);
  }
  return true;
}
static void reload_btop(const char *root) {
  GDir *directory = g_dir_open(root, 0, NULL);
  if (!directory)
    return;
  const char *name;
  while ((name = g_dir_read_name(directory))) {
    bool digits = *name;
    for (const char *s = name; *s && digits; s = g_utf8_next_char(s))
      digits = g_unichar_isdigit(g_utf8_get_char(s));
    if (!digits)
      continue;
    char *entry = path_join(root, name), *path = path_join(entry, "comm");
    g_free(entry);
    char *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL) &&
        g_utf8_validate(data, -1, NULL)) {
      char *comm = trim(data);
      if (!strcmp(comm, "btop")) {
        char *end;
        long pid = strtol(name, &end, 10);
        if (!*end && pid > 0)
          kill((pid_t)pid, SIGUSR2);
      }
      g_free(comm);
    }
    g_free(path);
    g_free(data);
  }
  g_dir_close(directory);
}
void reload_apps(Themes *t) {
  if (enabled())
    reload_btop(t->proc_root);
  const char *owner[] = {"gdbus",
                         "call",
                         "--session",
                         "--dest",
                         "org.freedesktop.DBus",
                         "--object-path",
                         "/org/freedesktop/DBus",
                         "--method",
                         "org.freedesktop.DBus.NameHasOwner",
                         "com.mitchellh.ghostty",
                         NULL};
  Command *c = run(owner, 5000);
  bool ok = c->ok;
  if (ok && strstr(c->out, "true")) {
    command_free(c);
    const char *reload[] = {"gdbus",
                            "call",
                            "--session",
                            "--dest",
                            "com.mitchellh.ghostty",
                            "--object-path",
                            "/com/mitchellh/ghostty",
                            "--method",
                            "org.gtk.Actions.Activate",
                            "reload-config",
                            "[]",
                            "{}",
                            NULL};
    c = run(reload, 5000);
    ok = c->ok;
  }
  if (!ok)
    warn(t, "Reload Ghostty configuration manually (Ctrl+Shift+,).");
  command_free(c);
  const char *herdr[] = {"herdr", "server", "reload-config", NULL};
  c = run(herdr, 10000);
  if (!c->ok)
    warn(t,
         "Herdr was not reloaded. If running, use herdr server reload-config.");
  command_free(c);
}
