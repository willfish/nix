#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <errno.h>
#include <getopt.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

static yyjson_val *field(yyjson_val *root, const char *name) {
  yyjson_val *key, *value, *found = NULL;
  size_t i, n, len = strlen(name);
  yyjson_obj_foreach(root, i, n, key, value) {
    if (yyjson_get_len(key) == len && !memcmp(yyjson_get_str(key), name, len))
      found = value;
  }
  return found;
}
static bool space(gunichar c) {
  return g_unichar_isspace(c) || c == '\v' || c == 0x85 ||
         (c >= 0x1c && c <= 0x1f);
}
static char *read_text(const char *path) {
  char *text = NULL;
  gsize len;
  if (!g_file_get_contents(path, &text, &len, NULL))
    return NULL;
  if (!g_utf8_validate(text, (gssize)len, NULL)) {
    g_free(text);
    return NULL;
  }
  char *begin = text, *end = text + len;
  while (*begin && space(g_utf8_get_char(begin)))
    begin = g_utf8_next_char(begin);
  while (end > begin) {
    char *prev = g_utf8_find_prev_char(begin, end);
    if (!prev || !space(g_utf8_get_char(prev)))
      break;
    end = prev;
  }
  char *trimmed = g_strndup(begin, (gsize)(end - begin));
  g_free(text);
  return trimmed;
}
static yyjson_val *mapping(yyjson_val *value, bool *invalid) {
  if (yyjson_is_obj(value))
    return value;
  bool empty = !value || yyjson_is_null(value) || yyjson_is_false(value) ||
               (yyjson_is_num(value) && yyjson_get_num(value) == 0) ||
               (yyjson_is_str(value) && yyjson_get_len(value) == 0) ||
               (yyjson_is_arr(value) && yyjson_arr_size(value) == 0);
  if (!empty)
    *invalid = true;
  return NULL;
}
static char *reference(yyjson_val *catalogue, const char *selection,
                       const char *mode, bool *invalid) {
  const char *key = *selection && strcmp(selection, "default")
                        ? selection
                        : yyjson_get_str(field(catalogue, "default"));
  if (!key)
    return NULL;
  yyjson_val *palettes = mapping(field(catalogue, "palettes"), invalid);
  yyjson_val *palette = mapping(field(palettes, key), invalid);
  yyjson_val *session = mapping(field(palette, "session"), invalid);
  yyjson_val *selected = mapping(field(session, mode), invalid);
  yyjson_val *source = field(selected, "wallpaper.png");
  const char *value = yyjson_get_str(source);
  if (!value || yyjson_get_len(source) != strlen(value) ||
      !g_str_has_prefix(value, "nix-theme:"))
    return NULL;
  const char *name = value + strlen("nix-theme:");
  return *name && !strchr(name, '/') && !strstr(name, "..") &&
                 !strchr(name, ' ')
             ? g_strdup(name)
             : NULL;
}
static char *next_name(yyjson_val *names, const char *current,
                       const char *preferred, int step) {
  GPtrArray *ordered = g_ptr_array_new();
  yyjson_val *name;
  size_t i, n;
  yyjson_arr_foreach(names, i, n, name) {
    const char *text = yyjson_get_str(name);
    if (text && yyjson_get_len(name) == strlen(text) && !strchr(text, '/') &&
        strcmp(text, ".") && strcmp(text, ".."))
      g_ptr_array_add(ordered, (gpointer)text);
  }
  char *chosen = NULL;
  if (ordered->len < 2 || (step != 1 && step != -1))
    goto done;
  size_t at = 0, preferred_at = 0;
  bool found = false, preferred_found = false;
  for (size_t index = 0; index < ordered->len; index++) {
    if (!preferred_found && preferred &&
        !strcmp(preferred, ordered->pdata[index])) {
      preferred_at = index;
      preferred_found = true;
    }
    if (!found && !strcmp(current, ordered->pdata[index])) {
      at = index;
      found = true;
    }
  }
  if (!found)
    at = preferred_at;
  at = step == 1 ? (at + 1) % ordered->len
                 : (at + ordered->len - 1) % ordered->len;
  chosen = g_strdup(ordered->pdata[at]);
done:
  g_ptr_array_free(ordered, true);
  return chosen;
}
static bool exists(const char *path) {
  return g_file_test(path, G_FILE_TEST_EXISTS);
}
static bool symlink_at(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}
static char *anchor(const char *path) {
  return symlink_at(path) ? g_file_read_link(path, NULL) : NULL;
}
static bool remove_if_present(const char *path) {
  return unlink(path) == 0 || errno == ENOENT;
}
static bool run(const char *const *argv) {
  GError *error = NULL;
  gint status;
  bool spawned = g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, NULL, &status, &error);
  bool ok = spawned && g_spawn_check_wait_status(status, NULL);
  g_clear_error(&error);
  return ok;
}
static int advance(const char *state, const char *catalogue_path,
                   const char *flake, const char *magick, int step) {
  int result = 3;
  char *link = g_build_filename(state, "wallpaper-source", NULL);
  char *current_path = g_build_filename(state, "wallpaper-current", NULL);
  char *live = g_build_filename(state, "active/wallpaper-live.png", NULL);
  char *selection_path = g_build_filename(state, "selection", NULL),
       *mode_path = g_build_filename(state, "mode", NULL);
  char *selection = read_text(selection_path), *mode = read_text(mode_path);
  char *theme = NULL, *anchored = NULL, *package = NULL, *meta_path = NULL,
       *current = NULL, *chosen = NULL;
  char *source_path = NULL, *source = NULL, *background_path = NULL,
       *background = NULL, *parent = NULL, *temporary = NULL;
  yyjson_doc *catalogue = yyjson_read_file(
                 catalogue_path, YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL),
             *meta = NULL;
  if (!catalogue || yyjson_is_null(yyjson_doc_get_root(catalogue)))
    goto done;
  if (!yyjson_is_obj(yyjson_doc_get_root(catalogue))) {
    result = 1;
    goto done;
  }
  bool invalid_reference = false;
  theme = reference(
      yyjson_doc_get_root(catalogue), selection ? selection : "default",
      mode && (!strcmp(mode, "light") || !strcmp(mode, "dark")) ? mode : "dark",
      &invalid_reference);
  if (invalid_reference) {
    result = 1;
    goto done;
  }
  if (!theme) {
    if (!remove_if_present(live) || !remove_if_present(current_path) ||
        ((symlink_at(link) || exists(link)) && !remove_if_present(link)))
      result = 1;
    goto done;
  }
  if (!exists(link)) {
    char *target = g_strdup_printf("%s#theme-%s", flake, theme);
    const char *argv[] = {"nix", "build", "--out-link", link, target, NULL};
    bool built = run(argv);
    g_free(target);
    if (!built) {
      result = 1;
      goto done;
    }
  }
  anchored = anchor(link);
  package = realpath(link, NULL);
  if (!package)
    goto done;
  meta_path = g_build_filename(package, "theme.json", NULL);
  current = exists(current_path) ? read_text(current_path) : g_strdup("");
  if (!current) {
    result = 1;
    goto done;
  }
  // A rendered package still carries theme.json. A locked theme fetch does
  // not; the catalogue lists the same names without rooting the images.
  yyjson_val *names = NULL;
  const char *preferred = NULL;
  if (g_file_test(meta_path, G_FILE_TEST_IS_REGULAR)) {
    meta = yyjson_read_file(meta_path, YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL);
    if (meta && yyjson_is_obj(yyjson_doc_get_root(meta))) {
      yyjson_val *preferred_value = field(yyjson_doc_get_root(meta), "preferred");
      preferred = yyjson_get_str(preferred_value);
      if (preferred && yyjson_get_len(preferred_value) != strlen(preferred))
        preferred = NULL;
      names = field(yyjson_doc_get_root(meta), "backgrounds");
    }
  } else {
    yyjson_val *palette =
        field(field(yyjson_doc_get_root(catalogue), "palettes"), theme);
    yyjson_val *listed = palette ? field(palette, "backgrounds") : NULL;
    if (listed && yyjson_is_arr(listed)) {
      yyjson_val *preferred_value = field(palette, "preferred");
      if (preferred_value && !yyjson_is_null(preferred_value)) {
        preferred = yyjson_get_str(preferred_value);
        if (!preferred || yyjson_get_len(preferred_value) != strlen(preferred)) {
          result = 1;
          goto done;
        }
      }
      names = listed;
    }
  }
  chosen = next_name(names, current, preferred, step);
  if (!chosen)
    goto done;
  background_path = g_build_filename(package, "backgrounds", NULL);
  source_path = g_build_filename(background_path, chosen, NULL);
  source = realpath(source_path, NULL);
  background = realpath(background_path, NULL);
  if (!source || !background)
    goto done;
  parent = g_path_get_dirname(source);
  if (strcmp(parent, background) ||
      !g_file_test(source, G_FILE_TEST_IS_REGULAR))
    goto done;
  g_free(parent);
  parent = g_path_get_dirname(live);
  if (g_mkdir_with_parents(parent, 0777) != 0) {
    result = 1;
    goto done;
  }
  temporary = g_build_filename(parent, ".wallpaper-live.png.tmp", NULL);
  char *output = g_strconcat("PNG:", temporary, NULL);
  const char *argv[] = {magick, source, output, NULL};
  bool converted = run(argv);
  g_free(output);
  if (!converted) {
    result = 1;
    goto done;
  }
  char *after = anchor(link);
  bool link_changed = symlink_at(link) && g_strcmp0(after, anchored) != 0;
  g_free(after);
  char *after_current = exists(current_path) ? read_text(current_path) : NULL;
  bool current_changed =
      exists(current_path) && g_strcmp0(after_current, current) != 0;
  g_free(after_current);
  if (link_changed || current_changed) {
    if (!remove_if_present(temporary))
      result = 1;
    goto done;
  }
  if (g_rename(temporary, live) != 0) {
    result = 1;
    goto done;
  }
  char *payload = g_strconcat(chosen, "\n", NULL);
  // Keep current-file symlinks intact when recording the selected image.
  FILE *file = fopen(current_path, "w");
  bool wrote =
      file && fwrite(payload, 1, strlen(payload), file) == strlen(payload);
  bool closed = file ? fclose(file) == 0 : false;
  g_free(payload);
  result = wrote && closed && chmod(current_path, 0600) == 0 ? 0 : 1;
done:
  yyjson_doc_free(catalogue);
  yyjson_doc_free(meta);
  g_free(link);
  g_free(current_path);
  g_free(live);
  g_free(selection_path);
  g_free(mode_path);
  g_free(selection);
  g_free(mode);
  g_free(theme);
  g_free(anchored);
  free(package);
  g_free(meta_path);
  g_free(current);
  g_free(chosen);
  g_free(source_path);
  free(source);
  g_free(background_path);
  free(background);
  g_free(parent);
  g_free(temporary);
  return result;
}
int main(int argc, char **argv) {
  const char *state = NULL, *catalogue = NULL, *flake = NULL,
             *magick = "magick", *direction = "next";
  const struct option options[] = {{"state", required_argument, NULL, 's'},
                                   {"catalogue", required_argument, NULL, 'c'},
                                   {"flake", required_argument, NULL, 'f'},
                                   {"magick", required_argument, NULL, 'm'},
                                   {"direction", required_argument, NULL, 'd'},
                                   {"help", no_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
  int flag;
  while ((flag = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    switch (flag) {
    case 's':
      state = optarg;
      break;
    case 'c':
      catalogue = optarg;
      break;
    case 'f':
      flake = optarg;
      break;
    case 'm':
      magick = optarg;
      break;
    case 'd':
      direction = optarg;
      break;
    case 'h':
      puts("usage: wallpaper-cycle --state DIR --catalogue FILE --flake PATH "
           "[--magick BIN] [--direction next|previous]");
      return 0;
    default:
      return 2;
    }
  }
  if (!state || !catalogue || !flake || optind != argc ||
      (strcmp(direction, "next") && strcmp(direction, "previous"))) {
    fputs("usage: wallpaper-cycle --state DIR --catalogue FILE --flake PATH "
          "[--magick BIN] [--direction next|previous]\n",
          stderr);
    return 2;
  }
  return advance(state, catalogue, flake, magick,
                 !strcmp(direction, "previous") ? -1 : 1);
}
