#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "managed.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *path_normal(const char *p) {
  GString *s = g_string_new(NULL);
  size_t slash = 0;
  while (p[slash] == '/')
    slash++;
  if (slash)
    g_string_append(s, slash == 2 ? "//" : "/");
  char **parts = g_strsplit(p + slash, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    if (s->len && s->str[s->len - 1] != '/')
      g_string_append_c(s, '/');
    g_string_append(s, parts[i]);
  }
  g_strfreev(parts);
  if (!s->len)
    g_string_append_c(s, '.');
  return g_string_free(s, FALSE);
}
char *path_join(const char *p, const char *c) {
  return g_strconcat(p, "/", c, NULL);
}
char *path_resolve(const char *path) {
  char *cur = *path == '/' ? g_strdup("/") : g_get_current_dir();
  char **parts = g_strsplit(path, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    char *next = !strcmp(parts[i], "..") ? g_path_get_dirname(cur)
                                         : path_join(cur, parts[i]);
    g_free(cur);
    char *real = realpath(next, NULL);
    cur = real ? g_strdup(real) : g_strdup(next);
    free(real);
    g_free(next);
  }
  g_strfreev(parts);
  return cur;
}
bool symlinked(const char *p) {
  struct stat st;
  return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}
bool exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}
bool regular(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
char *safe_path(const char *root, const char *relative) {
  char *normal = path_normal(relative);
  bool ok = *normal != '/' && strcmp(normal, ".");
  char **parts = g_strsplit(normal, "/", -1);
  for (size_t i = 0; parts[i]; i++)
    if (!strcmp(parts[i], ".."))
      ok = false;
  g_strfreev(parts);
  char *path = ok ? path_join(root, normal) : NULL;
  g_free(normal);
  if (!path) {
    failed = true;
    return NULL;
  }
  char *parent = g_path_get_dirname(root), *probe = g_strdup(path);
  for (;;) {
    if (strcmp(probe, parent) && symlinked(probe)) {
      ok = false;
      break;
    }
    char *next = g_path_get_dirname(probe);
    if (!strcmp(next, probe)) {
      g_free(next);
      break;
    }
    g_free(probe);
    probe = next;
  }
  g_free(probe);
  g_free(parent);
  if (!ok) {
    g_free(path);
    failed = true;
    return NULL;
  }
  return path;
}
static bool mkdir_private(const char *path, mode_t mode) {
  if (mkdir(path, mode) == 0)
    return true;
  struct stat st;
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
    return true;
  if (errno != ENOENT)
    return false;
  char *p = g_path_get_dirname(path);
  bool ok = strcmp(path, p) && mkdir_private(p, 0777);
  g_free(p);
  return ok && (mkdir(path, mode) == 0 ||
                (stat(path, &st) == 0 && S_ISDIR(st.st_mode)));
}
bool parents(const char *path, mode_t mode) {
  char *p = g_path_get_dirname(path);
  bool ok = mkdir_private(p, mode);
  g_free(p);
  if (!ok)
    failed = true;
  return ok;
}
GBytes *read_bytes(const char *p) {
  char *data = NULL;
  gsize n = 0;
  if (!g_file_get_contents(p, &data, &n, NULL)) {
    failed = true;
    return NULL;
  }
  return g_bytes_new_take(data, n);
}
char *read_text(const char *p) {
  GBytes *bytes = read_bytes(p);
  if (!bytes)
    return NULL;
  gsize n;
  const char *data = g_bytes_get_data(bytes, &n);
  if (!g_utf8_validate(data, (gssize)n, NULL)) {
    g_bytes_unref(bytes);
    failed = true;
    return NULL;
  }
  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < n; i++) {
    if (data[i] == '\r') {
      g_string_append_c(s, '\n');
      if (i + 1 < n && data[i + 1] == '\n')
        i++;
    } else
      g_string_append_c(s, data[i]);
  }
  g_bytes_unref(bytes);
  return g_string_free(s, FALSE);
}
static bool whitespace(gunichar c) {
  return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 0x85 ||
         c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
         c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
         c == 0x3000;
}
char *strip(char *s) {
  if (!s)
    return NULL;
  const char *start = s, *end = s + strlen(s);
  while (*start && whitespace(g_utf8_get_char(start)))
    start = g_utf8_next_char(start);
  while (end > start) {
    const char *p = g_utf8_find_prev_char(start, end);
    if (!whitespace(g_utf8_get_char(p)))
      break;
    end = p;
  }
  char *out = g_strndup(start, (size_t)(end - start));
  g_free(s);
  return out;
}
Value *read_json(const char *path) {
  GBytes *b = read_bytes(path);
  if (!b)
    return NULL;
  gsize n;
  const char *s = g_bytes_get_data(b, &n);
  Value *v = parse_json(s, n);
  g_bytes_unref(b);
  return v;
}
bool atomic_write(const char *path, GBytes *data, mode_t mode,
                  const char *prefix, bool unchanged) {
  if (!data || failed || !parents(path, 0700) || symlinked(path)) {
    failed = true;
    return false;
  }
  if (unchanged && exists(path)) {
    GBytes *old = read_bytes(path);
    bool same = old && g_bytes_equal(old, data);
    if (old)
      g_bytes_unref(old);
    if (failed)
      return false;
    if (same) {
      if (chmod(path, mode) != 0)
        failed = true;
      return !failed;
    }
  }
  char *parent = g_path_get_dirname(path),
       *temp = g_strconcat(parent, "/", prefix, "XXXXXX", NULL);
  g_free(parent);
  int fd = g_mkstemp_full(temp, O_WRONLY | O_CLOEXEC, 0600);
  bool made = fd >= 0, ok = made;
  gsize length;
  const char *bytes = g_bytes_get_data(data, &length);
  for (size_t at = 0; ok && at < length;) {
    ssize_t n = write(fd, bytes + at, length - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      ok = false;
    else
      at += (size_t)n;
  }
  if (fd >= 0 && close(fd) < 0)
    ok = false;
  if (ok && chmod(temp, mode) != 0)
    ok = false;
  if (ok && rename(temp, path) != 0)
    ok = false;
  if (made)
    unlink(temp);
  g_free(temp);
  if (!ok)
    failed = true;
  return ok;
}
