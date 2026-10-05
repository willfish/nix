#define _DEFAULT_SOURCE
#include "projects.h"
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
const Limits project_limits = {
    .max_depth = 3, .max_entries = 200, .max_dirents = 4000};
char *path_normalize(const char *path, bool collapse_parent) {
  bool absolute = *path == '/',
       double_root = path[0] == '/' && path[1] == '/' && path[2] != '/';
  char **parts = g_strsplit(path, "/", -1);
  GPtrArray *kept = g_ptr_array_new_with_free_func(g_free);
  for (size_t i = 0; parts[i]; i++) {
    const char *part = parts[i];
    if (!*part || !strcmp(part, "."))
      continue;
    if (collapse_parent && !strcmp(part, "..")) {
      if (kept->len && strcmp(kept->pdata[kept->len - 1], ".."))
        g_ptr_array_remove_index(kept, kept->len - 1);
      else if (!absolute)
        g_ptr_array_add(kept, g_strdup(part));
    } else
      g_ptr_array_add(kept, g_strdup(part));
  }
  GString *out = g_string_new(absolute ? (double_root ? "//" : "/") : "");
  for (size_t i = 0; i < kept->len; i++) {
    if (out->len && out->str[out->len - 1] != '/')
      g_string_append_c(out, '/');
    g_string_append(out, kept->pdata[i]);
  }
  if (!out->len)
    g_string_append_c(out, '.');
  g_strfreev(parts);
  g_ptr_array_free(kept, true);
  return g_string_free(out, false);
}
char *home_absolute(const char *path) {
  char *raw;
  if (*path == '/')
    raw = g_strdup(path);
  else {
    char *cwd = getcwd(NULL, 0);
    if (!cwd)
      return NULL;
    raw = g_strconcat(cwd, "/", path, NULL);
    free(cwd);
  }
  char *home = path_normalize(raw, true);
  g_free(raw);
  return home;
}
char *selected_home(const char *explicit_home) {
  if (explicit_home && *explicit_home)
    return home_absolute(explicit_home);
  const char *value = g_getenv("LAUNCHER_HOME");
  if (value && *value)
    return home_absolute(value);
  value = g_getenv("HOME");
  if (value)
    return home_absolute(*value ? value : "/");
  struct passwd *pw = getpwuid(getuid());
  return pw ? home_absolute(pw->pw_dir) : NULL;
}
char *project_id(const char *path) {
  static const unsigned char prefix[] = "launcher-project\0";
  char *normal = path_normalize(path, false);
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(sum, prefix, sizeof(prefix) - 1);
  g_checksum_update(sum, (const guchar *)normal, (gssize)strlen(normal));
  char *id = g_strdup(g_checksum_get_string(sum));
  g_checksum_free(sum);
  g_free(normal);
  return id;
}
char *safe_text(const char *value) {
  GString *out = g_string_new(NULL);
  const char *p = value;
  size_t left = strlen(value);
  while (left) {
    gunichar c = g_utf8_get_char_validated(p, (gssize)left);
    if (c == (gunichar)-1 || c == (gunichar)-2) {
      g_string_append_c(out, '?');
      p++;
      left--;
    } else {
      size_t n = (size_t)(g_utf8_next_char(p) - p);
      g_string_append_len(out, p, (gssize)n);
      p += n;
      left -= n;
    }
  }
  return g_string_free(out, false);
}
bool valid_id(const char *id) {
  if (strlen(id) != 64)
    return false;
  for (const char *s = id; *s; s++)
    if (!(*s >= '0' && *s <= '9') && !(*s >= 'a' && *s <= 'f'))
      return false;
  return true;
}
bool skipped_name(const char *name) {
  if (!*name || name[0] == '.' || strchr(name, '/'))
    return true;
  const char *skip[] = {"node_modules", "bower_components", "vendor",
                        "venv",         "site-packages",    "__pycache__",
                        "third_party",  "third-party",      "deps",
                        "Pods",         "Carthage",         "elm-stuff",
                        "result",       "target",           "dist",
                        "build"};
  for (size_t i = 0; i < G_N_ELEMENTS(skip); i++)
    if (!strcmp(name, skip[i]))
      return true;
  return false;
}
bool real_dir(const char *path) {
  struct stat st;
  return !lstat(path, &st) && S_ISDIR(st.st_mode);
}
bool git_marker(const char *path) {
  char *marker = g_build_filename(path, ".git", NULL);
  struct stat st;
  bool yes =
      !lstat(marker, &st) && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode));
  g_free(marker);
  return yes;
}
bool launchable(const char *home, const char *raw_path) {
  char *path = path_normalize(raw_path, false);
  bool ok = false;
  if (*path != '/')
    goto finish;
  char **parts = g_strsplit(path, "/", -1);
  for (size_t i = 0; parts[i]; i++)
    if (!strcmp(parts[i], "..")) {
      g_strfreev(parts);
      goto finish;
    }
  g_strfreev(parts);
  char *dotfiles = g_build_filename(home, ".dotfiles", NULL);
  if (!strcmp(path, dotfiles)) {
    ok = real_dir(path);
    g_free(dotfiles);
    goto finish;
  }
  g_free(dotfiles);
  char *root = g_build_filename(home, "Repositories", NULL);
  size_t len = strlen(root);
  if (!g_str_has_prefix(path, root) || path[len] != '/' || !path[len + 1] ||
      !real_dir(root)) {
    g_free(root);
    goto finish;
  }
  parts = g_strsplit(path + len + 1, "/", -1);
  if (g_strv_length(parts) > project_limits.max_depth) {
    g_strfreev(parts);
    g_free(root);
    goto finish;
  }
  char *current = g_strdup(root);
  ok = true;
  for (size_t i = 0; parts[i] && ok; i++) {
    if (skipped_name(parts[i])) {
      ok = false;
      break;
    }
    char *next = g_build_filename(current, parts[i], NULL);
    g_free(current);
    current = next;
    ok = real_dir(current);
  }
  ok = ok && !strcmp(current, path) && git_marker(path);
  g_free(current);
  g_strfreev(parts);
  g_free(root);
finish:
  g_free(path);
  return ok;
}
