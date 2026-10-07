#include "theme.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
static int luma(const char *colour) {
  while (*colour == '#')
    colour++;
  char part[3] = {0};
  int rgb[3];
  for (int i = 0; i < 3; i++) {
    if (strlen(colour) < (size_t)(2 * i + 2))
      return -1;
    memcpy(part, colour + 2 * i, 2);
    char *end;
    rgb[i] = (int)strtol(part, &end, 16);
    if (*end)
      return -1;
  }
  return (2126 * rgb[0] + 7152 * rgb[1] + 722 * rgb[2]) / 10000;
}
const char *selection_text(const char *background, const char *paper,
                           const char *ink, const char *bright) {
  int b = luma(background), i = luma(ink);
  if (abs(i - b) >= 120)
    return ink;
  const char *best = paper;
  int difference = abs(luma(paper) - b);
  if (abs(i - b) > difference) {
    best = ink;
    difference = abs(i - b);
  }
  if (abs(luma(bright) - b) > difference)
    best = bright;
  return best;
}
static const char *colour(yyjson_val *colours, const char *key,
                          const char *fallback) {
  const char *s = yyjson_get_str(field(colours, key));
  if (!s)
    s = fallback;
  while (*s == '#')
    s++;
  return s;
}
static char *setting(yyjson_val *o, const char *key, const char *fallback) {
  return scalar(field(o, key), fallback);
}
static int palette_compare(const void *a, const void *b, void *raw) {
  yyjson_val *palettes = raw;
  const char *x = *(char *const *)a, *y = *(char *const *)b;
  const char *sx = yyjson_get_str(field(field(palettes, x), "label")),
             *sy = yyjson_get_str(field(field(palettes, y), "label"));
  int difference = strcmp(sx ? sx : "", sy ? sy : "");
  if (difference)
    return difference;
  // Python's label sort retains catalogue order when labels are equal.
  yyjson_val *key, *value;
  size_t i, count;
  yyjson_obj_foreach(palettes, i, count, key, value) {
    (void)value;
    const char *id = yyjson_get_str(key);
    if (!strcmp(id, x))
      return strcmp(x, y) ? -1 : 0;
    if (!strcmp(id, y))
      return 1;
  }
  return 0;
}
static GHashTable *ini_values(const char *text) {
  GHashTable *values =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  char **rows = lines(text), *section = g_strdup("");
  for (size_t i = 0; rows[i]; i++) {
    char *s = trim(rows[i]);
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '[' && s[n - 1] == ']') {
      g_free(section);
      section = g_strndup(s + 1, n - 2);
    } else {
      char *eq = strchr(s, '=');
      if (eq && s[0] != '#') {
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1),
             *name = g_strdup_printf("%s/%s", section, key);
        g_hash_table_replace(values, name, value);
        g_free(key);
      }
    }
    g_free(s);
  }
  g_free(section);
  g_strfreev(rows);
  return values;
}
static bool launcher_style(Themes *t, char **font, char **border,
                           char **radius) {
  yyjson_val *a = field(t->catalogue, "appearance");
  char *family = setting(a, "monoFont", "JetBrainsMono Nerd Font"),
       *size = setting(a, "fontSize", "12");
  *font = g_strdup_printf("%s:size=%s", family, size);
  g_free(family);
  g_free(size);
  *border = setting(a, "borderSize", "2");
  *radius = setting(a, "rounding", "12");
  char *saved = t->error;
  t->error = NULL;
  char *selected = selection(t), *mode = current_mode(t);
  yyjson_val *p = selected ? resolve(t, selected) : NULL,
             *v = p && mode ? field(session(p, mode), "fuzzel.ini") : NULL;
  const char *source = truth(v) ? string(t, v) : NULL;
  g_free(selected);
  g_free(mode);
  if (t->error) {
    g_free(t->error);
    t->error = saved;
    return true;
  }
  t->error = saved;
  if (!source)
    return true;
  char *text = read_text(t, source);
  if (!text)
    return false;
  GHashTable *values = ini_values(text);
  g_free(text);
  const char *keys[] = {"main/font", "border/width", "border/radius"};
  char **dest[] = {font, border, radius};
  for (size_t i = 0; i < 3; i++) {
    const char *value = g_hash_table_lookup(values, keys[i]);
    if (value) {
      g_free(*dest[i]);
      *dest[i] = g_strdup(value);
    }
  }
  g_hash_table_destroy(values);
  return true;
}
char *choose(Themes *t) {
  char *current = selection(t), *mode = current_mode(t), *chosen = NULL,
       *font = NULL, *border = NULL, *radius = NULL;
  if (!current || !mode) {
    g_free(current);
    g_free(mode);
    return NULL;
  }
  bool light = !strcmp(mode, "light");
  const char *target = light ? "dark" : "light";
  yyjson_val *palettes = field(t->catalogue, "palettes"),
             *p = resolve(t, current), *default_palette = resolve(t, "default");
  if (!p || !default_palette) {
    g_free(current);
    g_free(mode);
    return NULL;
  }
  const char *default_label = string(t, field(default_palette, "label"));
  GPtrArray *sorted = keys(palettes),
            *ids = g_ptr_array_new_with_free_func(g_free),
            *labels = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_sort_with_data(sorted, palette_compare, palettes);
  if (!truth(field(p, "nativeMode"))) {
    g_ptr_array_add(ids, g_strdup(target));
    g_ptr_array_add(labels, g_strdup_printf("Switch to %s mode", target));
  }
  g_ptr_array_add(ids, g_strdup("default"));
  g_ptr_array_add(labels, g_strdup_printf("Host default (%s)",
                                          default_label ? default_label : ""));
  for (size_t i = 0; i < sorted->len; i++) {
    const char *id = sorted->pdata[i],
               *label = string(t, field(field(palettes, id), "label"));
    g_ptr_array_add(ids, g_strdup(id));
    g_ptr_array_add(labels, g_strdup(label ? label : ""));
  }
  g_ptr_array_free(sorted, true);
  GString *rows = g_string_new(NULL);
  size_t selected = 0;
  bool found = false;
  for (size_t i = 0; i < ids->len; i++) {
    bool active = !strcmp(ids->pdata[i], current);
    if (active) {
      selected = i;
      found = true;
    }
    g_string_append_printf(rows, "%c %s\n", active ? '*' : ' ',
                           (char *)labels->pdata[i]);
  }
  if (!found)
    fail(t, "Invalid current theme selection");
  yyjson_val *colours = field(field(p, "nvim"), light ? "light" : "dark"),
             *launcher = field(t->catalogue, "launcher");
  const char *bg = colour(colours, "base02", "494d64"),
             *fg = selection_text(bg, colour(colours, "base00", "24273a"),
                                  colour(colours, "base05", "cad3f5"),
                                  colour(colours, "base07", "f4dbd6"));
  if (luma(bg) < 0 || luma(fg) < 0 ||
      luma(colour(colours, "base00", "24273a")) < 0 ||
      luma(colour(colours, "base05", "cad3f5")) < 0 ||
      luma(colour(colours, "base07", "f4dbd6")) < 0)
    fail(t, "Invalid theme colour");
  if (!t->error)
    launcher_style(t, &font, &border, &radius);
  char *anchor = setting(launcher, "anchor", "center"),
       *layer = setting(launcher, "layer", "overlay"),
       *width = setting(launcher, "width", "40"),
       *match = setting(launcher, "matchMode", "fzf");
  yyjson_val *limit = field(launcher, "lines");
  size_t row_count = ids->len;
  char *count;
  if (limit && yyjson_is_num(limit) &&
      yyjson_get_num(limit) < (double)row_count)
    count = scalar(limit, NULL);
  else if (limit && !yyjson_is_num(limit)) {
    fail(t, "Invalid launcher line count");
    count = g_strdup("0");
  } else
    count = g_strdup_printf("%zu", row_count);
  char *settings = NULL, *directory = NULL, *file = NULL;
  Command *c = NULL;
  if (!t->error) {
    settings = g_strdup_printf(
        "[main]\nfont=%s\nanchor=%s\nlayer=%s\nwidth=%s\nlines=%s\nminimal-"
        "lines=yes\nmatch-mode=%s\nicons-enabled=no\nhorizontal-pad="
        "20\nvertical-pad=12\ninner-pad=8\n[colors]\nbackground=%sff\ntext=%"
        "sff\ninput=%sff\nprompt=%sff\nselection=%sff\nselection-text=%"
        "sff\nselection-match=%sff\nmatch=%sff\nborder=%sff\n[border]\nwidth=%"
        "s\nradius=%s\n",
        font, anchor, layer, width, count, match,
        colour(colours, "base00", "24273a"),
        colour(colours, "base05", "cad3f5"),
        colour(colours, "base05", "cad3f5"),
        colour(colours, "base0D", "b7bdf8"), bg, fg,
        colour(colours, "base0D", "b7bdf8"),
        colour(colours, "base0D", "b7bdf8"),
        colour(colours, "base0D", "b7bdf8"), border, radius);
    GError *error = NULL;
    directory = g_dir_make_tmp("theme-menu-XXXXXX", &error);
    if (!directory) {
      fail(t, "%s", error->message);
      g_error_free(error);
    } else {
      file = path_join(directory, "fuzzel.ini");
      if (!g_file_set_contents(file, settings, -1, &error)) {
        fail(t, "%s", error->message);
        g_error_free(error);
      } else {
        char *index = g_strdup_printf("%zu", selected);
        const char *argv[] = {"fuzzel",   "--config", file,
                              "--dmenu",  "--index",  "--only-match",
                              "--prompt", "Theme > ", "--select-index",
                              index,      NULL};
        c = command(argv, rows->str, 0, false);
        g_free(index);
      }
    }
  }
  if (c) {
    char *value = trim(c->out), *detail = trim(c->err);
    bool cancelled = (c->status == 1 || c->status == 2) && !*value &&
                     !*detail && !c->failure;
    if (!cancelled) {
      if (!c->ok)
        fail(t, "Fuzzel failed: %s", c->failure ? c->failure : detail);
      else {
        size_t index = 0;
        bool valid = *value;
        for (const char *s = value; *s && valid; s = g_utf8_next_char(s)) {
          gunichar ch = g_utf8_get_char(s);
          int digit = g_unichar_digit_value(ch);
          if (g_unichar_type(ch) != G_UNICODE_DECIMAL_NUMBER || digit < 0 ||
              index >= ids->len)
            valid = false;
          else
            index = index * 10 + (unsigned)digit;
        }
        if (!valid || index >= ids->len)
          fail(t, "Invalid theme menu selection");
        else
          chosen = g_strdup(ids->pdata[index]);
      }
    }
    g_free(value);
    g_free(detail);
    command_free(c);
  }
  if (file)
    g_remove(file);
  if (directory)
    g_rmdir(directory);
  g_free(file);
  g_free(directory);
  g_free(settings);
  g_free(font);
  g_free(border);
  g_free(radius);
  g_free(anchor);
  g_free(layer);
  g_free(width);
  g_free(match);
  g_free(count);
  g_string_free(rows, true);
  g_ptr_array_free(ids, true);
  g_ptr_array_free(labels, true);
  g_free(current);
  g_free(mode);
  return chosen;
}
