#include "theme.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct {
  char *path;
  GBytes *content, *previous;
  bool changed;
} Write;
static void write_free(void *raw) {
  Write *w = raw;
  g_free(w->path);
  g_bytes_unref(w->content);
  if (w->previous)
    g_bytes_unref(w->previous);
  g_free(w);
}
static void put(GPtrArray *writes, char *path, GBytes *data) {
  for (size_t i = 0; i < writes->len; i++) {
    Write *w = writes->pdata[i];
    if (!strcmp(w->path, path)) {
      g_bytes_unref(w->content);
      w->content = data;
      g_free(path);
      return;
    }
  }
  Write *w = g_new0(Write, 1);
  w->path = path;
  w->content = data;
  g_ptr_array_add(writes, w);
}
static GBytes *payload(const char *s) { return g_bytes_new(s, strlen(s)); }
yyjson_val *resolve(Themes *t, const char *selected) {
  const char *key = selected;
  if (!strcmp(selected, "default")) {
    key = string(t, field(t->catalogue, "default"));
    if (!key)
      return NULL;
  }
  yyjson_val *p = field(field(t->catalogue, "palettes"), key);
  if (!p)
    fail(t, "Unknown palette: %s. Use theme-menu default to reset.", selected);
  else if (!yyjson_is_obj(p)) {
    fail(t, "Invalid palette bundle: %s", selected);
    return NULL;
  }
  return p;
}
static char *state_text(Themes *t, const char *name, const char *fallback) {
  char *path = path_join(t->state, name), *s = NULL;
  struct stat st;
  if (stat(path, &st) && errno == ENOENT)
    s = g_strdup(fallback);
  else {
    char *data = read_text(t, path);
    if (data) {
      s = trim(data);
      g_free(data);
    }
  }
  g_free(path);
  return s;
}
char *selection(Themes *t) {
  char *s = state_text(t, "selection", "default");
  if (!s)
    return NULL;
  if (!strcmp(s, "solarized") && !field(field(t->catalogue, "palettes"), s)) {
    g_free(s);
    s = g_strdup("osaka-jade");
  }
  if (!resolve(t, s)) {
    g_free(s);
    return NULL;
  }
  return s;
}
char *current_mode(Themes *t) {
  char *s = state_text(t, "mode", "dark");
  if (s && strcmp(s, "light") && strcmp(s, "dark")) {
    g_free(s);
    s = g_strdup("dark");
  }
  return s;
}
yyjson_val *session(yyjson_val *palette, const char *mode) {
  return field(field(palette, "session"), mode);
}
static bool plain_id(const char *name) {
  return name && *name && !strchr(name, '/') && !strstr(name, "..") &&
         !strchr(name, ' ');
}
static bool base_file(const char *name) {
  return name && *name && strcmp(name, ".") && strcmp(name, "..") &&
         !strchr(name, '/');
}
static bool hex6(const char *text) {
  if (!text || strlen(text) != 6)
    return false;
  for (size_t i = 0; i < 6; i++)
    if (!g_ascii_isxdigit(text[i]))
      return false;
  return true;
}
static bool png_name(const char *name) {
  size_t n = strlen(name);
  return n > 4 && !g_ascii_strcasecmp(name + n - 4, ".png");
}
static const char *magick_bin(void) {
  const char *magick = g_getenv("THEME_MAGICK");
  return magick && *magick ? magick : "magick";
}
static GBytes *magick_file(Themes *t, const char *const *argv, char *out) {
  Command *c = command(argv, NULL, 120000, false);
  bool ok = checked(t, c);
  command_free(c);
  GBytes *bytes = ok ? read_bytes(t, out) : NULL;
  if (unlink(out) && errno != ENOENT && !t->error)
    fail(t, "Could not remove converted wallpaper");
  g_free(out);
  return bytes;
}
static GBytes *wallpaper_from_tree(Themes *t, const char *id, const char *root) {
  yyjson_val *palette = field(field(t->catalogue, "palettes"), id);
  yyjson_val *preferred = palette ? field(palette, "preferred") : NULL;
  const char *file = NULL;
  if (preferred && !yyjson_is_null(preferred)) {
    file = string(t, preferred);
    if (!file)
      return NULL;
    if (!base_file(file)) {
      fail(t, "Invalid wallpaper file: %s", file);
      return NULL;
    }
  }
  char *out = path_join(t->state, ".wallpaper-convert.png");
  char *dest = g_strdup_printf("PNG:%s", out);
  const char *magick = magick_bin();
  GBytes *bytes = NULL;
  if (!file) {
    const char *color =
        palette ? string(t, field(palette, "wallpaperColor")) : NULL;
    if (!hex6(color)) {
      fail(t, "Theme %s has no wallpaper", id);
      g_free(dest);
      g_free(out);
      return NULL;
    }
    char *solid = g_strdup_printf("xc:#%s", color);
    const char *argv[] = {magick, "-size", "1x1", solid, dest, NULL};
    bytes = magick_file(t, argv, out);
    g_free(solid);
  } else {
    char *backgrounds = path_join(root, "backgrounds"),
         *source = path_join(backgrounds, file);
    g_free(backgrounds);
    if (!g_file_test(source, G_FILE_TEST_IS_REGULAR)) {
      fail(t, "Missing wallpaper %s", file);
      g_free(source);
      g_free(dest);
      g_free(out);
      return NULL;
    }
    if (png_name(file)) {
      bytes = read_bytes(t, source);
      g_free(out);
    } else {
      const char *argv[] = {magick, source, dest, NULL};
      bytes = magick_file(t, argv, out);
    }
    g_free(source);
  }
  g_free(dest);
  return bytes;
}
static GBytes *session_asset(Themes *t, yyjson_val *value) {
  const char *source = string(t, value);
  if (!source)
    return NULL;
  if (g_str_has_prefix(source, "nix-theme:")) {
    if (g_strcmp0(g_getenv("THEME_MENU_PUBLISH"), "1"))
      return NULL;
    const char *name = source + 10;
    if (!plain_id(name)) {
      fail(t, "Invalid theme wallpaper id: %s", name);
      return NULL;
    }
    const char *flake = g_getenv("THEME_WALLPAPER_FLAKE");
    char *default_flake =
             flake ? NULL : path_join(g_get_home_dir(), ".dotfiles"),
         *target = g_strdup_printf("%s#theme-%s", flake ? flake : default_flake,
                                   name);
    const char *argv[] = {"nix",  "build", "--no-link", "--print-out-paths",
                          target, NULL};
    Command *c = command(argv, NULL, 0, false);
    GBytes *bytes = NULL;
    if (checked(t, c)) {
      char *output = trim(c->out), **rows = lines(output);
      size_t n = g_strv_length(rows);
      if (!n)
        fail(t, "Wallpaper fetch returned no output path");
      else {
        // Older packages put a rendered PNG at the root. Current packages are
        // the locked theme tree; the catalogue names the image inside it.
        char *rendered = path_join(rows[n - 1], "wallpaper.png");
        if (g_file_test(rendered, G_FILE_TEST_IS_REGULAR))
          bytes = read_bytes(t, rendered);
        else
          bytes = wallpaper_from_tree(t, name, rows[n - 1]);
        g_free(rendered);
      }
      g_strfreev(rows);
      g_free(output);
    }
    command_free(c);
    g_free(default_flake);
    g_free(target);
    return bytes;
  }
  return read_bytes(t, source);
}
static bool projected(Themes *t, GPtrArray *writes, const char *mode,
                      yyjson_val *palette) {
  yyjson_val *s = session(palette, mode);
  if (truth(s) && !yyjson_is_obj(s))
    return fail(t, "Invalid session bundle");
  GPtrArray *names = keys(s);
  char *active = path_join(t->state, "active");
  for (size_t i = 0; i < names->len && !t->error; i++) {
    const char *name = names->pdata[i];
    GBytes *bytes = session_asset(t, field(s, name));
    if (bytes)
      put(writes, path_join(active, name), bytes);
  }
  g_ptr_array_free(names, true);
  g_free(active);
  char *data = g_strdup_printf("%s\n", mode);
  put(writes, path_join(t->state, "mode"), payload(data));
  g_free(data);
  return !t->error;
}
static bool transact(Themes *t, GPtrArray *writes, const char *selected) {
  char *select_path = selected ? path_join(t->state, "selection") : NULL;
  GBytes *old_selection =
      select_path && exists(select_path) ? read_bytes(t, select_path) : NULL;
  for (size_t i = 0; i < writes->len && !t->error; i++) {
    Write *w = writes->pdata[i];
    if (exists(w->path))
      w->previous = read_bytes(t, w->path);
  }
  if (t->error) {
    g_free(select_path);
    if (old_selection)
      g_bytes_unref(old_selection);
    return false;
  }
  bool ok = true;
  for (size_t i = 0; ok && i < writes->len; i++) {
    Write *w = writes->pdata[i];
    if (!w->previous || !g_bytes_equal(w->previous, w->content) ||
        path_is_symlink(w->path)) {
      ok = atomic_write(t, w->path, w->content);
      w->changed = ok;
    }
  }
  if (ok && select_path) {
    if (!strcmp(selected, "default"))
      ok = remove_file(t, select_path);
    else {
      char *data = g_strdup_printf("%s\n", selected);
      GBytes *bytes = payload(data);
      ok = atomic_write(t, select_path, bytes);
      g_bytes_unref(bytes);
      g_free(data);
    }
  }
  if (!ok) {
    for (size_t i = writes->len; i > 0; i--) {
      Write *w = writes->pdata[i - 1];
      if (w->changed) {
        if (w->previous)
          atomic_write(t, w->path, w->previous);
        else
          remove_file(t, w->path);
      }
    }
    if (select_path) {
      if (old_selection)
        atomic_write(t, select_path, old_selection);
      else
        remove_file(t, select_path);
    }
  }
  if (old_selection)
    g_bytes_unref(old_selection);
  g_free(select_path);
  return ok;
}
bool apply(Themes *t, const char *selected) {
  yyjson_val *p = resolve(t, selected);
  if (!p)
    return false;
  yyjson_val *files = field(p, "files");
  if (!yyjson_is_obj(files))
    return fail(t, "Invalid palette files");
  GPtrArray *writes = g_ptr_array_new_with_free_func(write_free),
            *names = keys(files);
  char *active = path_join(t->state, "active");
  for (size_t i = 0; i < names->len && !t->error; i++) {
    const char *name = names->pdata[i], *source = string(t, field(files, name));
    GBytes *data = source ? read_bytes(t, source) : NULL;
    if (data)
      put(writes, path_join(active, name), data);
  }
  g_ptr_array_free(names, true);
  g_free(active);
  yyjson_val *native = field(p, "nativeMode");
  char *mode = truth(native) ? scalar(native, NULL) : current_mode(t);
  bool ok = !t->error && mode && projected(t, writes, mode, p) &&
            transact(t, writes, selected);
  g_free(mode);
  g_ptr_array_free(writes, true);
  return ok;
}
bool set_mode(Themes *t, const char *mode) {
  if (strcmp(mode, "light") && strcmp(mode, "dark"))
    return fail(t, "Unknown appearance mode: %s", mode);
  char *s = selection(t);
  yyjson_val *p = s ? resolve(t, s) : NULL;
  g_free(s);
  if (!p)
    return false;
  yyjson_val *native = field(p, "nativeMode");
  char *n = truth(native) ? scalar(native, NULL) : NULL;
  if (n && strcmp(mode, n)) {
    fail(t, "This theme is %s-only. Select a %s theme from the menu.", n, mode);
    g_free(n);
    return false;
  }
  g_free(n);
  GPtrArray *writes = g_ptr_array_new_with_free_func(write_free);
  bool ok = projected(t, writes, mode, p) && transact(t, writes, NULL);
  g_ptr_array_free(writes, true);
  return ok;
}
