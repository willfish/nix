#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *config_path(const char *path) {
  GString *out = g_string_new(NULL);
  size_t leading = 0;
  while (path[leading] == '/')
    leading++;
  if (leading)
    g_string_append(out, leading == 2 ? "//" : "/");
  char **parts = g_strsplit(path + leading, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    if (out->len && out->str[out->len - 1] != '/')
      g_string_append_c(out, '/');
    g_string_append(out, parts[i]);
  }
  g_strfreev(parts);
  if (!out->len)
    g_string_append_c(out, '.');
  return g_string_free(out, false);
}
char *config_sibling(const char *path, const char *name) {
  if (!*path || !strcmp(path, ".") || !strcmp(path, "/") || !strcmp(path, "//"))
    return NULL;
  char *parent = g_path_get_dirname(path),
       *result = g_strconcat(parent, "/", name, NULL);
  g_free(parent);
  return result;
}
int config_exists(const char *path) {
  if (!path)
    return 0;
  struct stat st;
  if (stat(path, &st) == 0)
    return 1;
  return errno == ENOENT || errno == ENOTDIR || errno == EBADF || errno == ELOOP
             ? 0
             : -1;
}
static bool valid(const char *data, size_t length) {
  for (size_t at = 0; at < length;) {
    const char *nul = memchr(data + at, 0, length - at);
    size_t n = nul ? (size_t)(nul - (data + at)) : length - at;
    if (!g_utf8_validate(data + at, (gssize)n, NULL))
      return false;
    at += n + 1;
  }
  return true;
}
GString *config_read(const char *path) {
  char *data = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &data, &length, NULL))
    return NULL;
  if (!valid(data, length)) {
    g_free(data);
    return NULL;
  }
  GString *out = g_string_sized_new(length);
  for (size_t i = 0; i < length; i++) {
    char c = data[i];
    if (c == '\r') {
      c = '\n';
      if (i + 1 < length && data[i + 1] == '\n')
        i++;
    }
    g_string_append_c(out, c);
  }
  g_free(data);
  return out;
}
static bool space(gunichar cp) {
  return g_unichar_isspace(cp) || (cp >= 0x1c && cp <= 0x1f);
}
GString *config_strip(GString *s) {
  size_t at = 0, end = s->len;
  while (at < end && space(g_utf8_get_char(s->str + at)))
    at = (size_t)(g_utf8_next_char(s->str + at) - s->str);
  while (end > at) {
    char *previous = g_utf8_find_prev_char(s->str, s->str + end);
    if (!space(g_utf8_get_char(previous)))
      break;
    end = (size_t)(previous - s->str);
  }
  GString *out = g_string_new_len(s->str + at, (gssize)(end - at));
  g_string_free(s, true);
  return out;
}
Json *config_load(const char *path, bool optional) {
  if (optional) {
    int exists = config_exists(path);
    if (exists < 0)
      return NULL;
    if (!exists)
      return json_node(J_OBJECT);
  }
  GString *data = config_read(path);
  if (!data)
    return NULL;
  Json *value = NULL;
  if (!memchr(data->str, 0, data->len))
    value = json_parse(optional && !data->len ? "{}" : data->str);
  g_string_free(data, true);
  if (value && value->kind != J_OBJECT) {
    json_free(value);
    return NULL;
  }
  return value;
}
static bool parents(const char *path) {
  char *parent = g_path_get_dirname(path);
  bool ok = g_mkdir_with_parents(parent, 0777) == 0;
  g_free(parent);
  return ok;
}
static bool private_write(const char *path, const char *data) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  // Existing temporary files may have a permissive mode. Tighten it before any
  // credentials reach the descriptor, not just before final publication.
  bool ok = fchmod(fd, 0600) == 0;
  size_t length = strlen(data);
  for (size_t at = 0; ok && at < length;) {
    ssize_t n = write(fd, data + at, length - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      ok = false;
    else
      at += (size_t)n;
  }
  if (close(fd) < 0)
    ok = false;
  return ok;
}
bool config_save(const char *path, Json *value) {
  if (!parents(path))
    return false;
  char *name = g_path_get_basename(path),
       *temp_name = g_strconcat(name, ".tmp", NULL),
       *temporary = config_sibling(path, temp_name);
  g_free(name);
  g_free(temp_name);
  if (!temporary)
    return false;
  char *encoded = json_encode(value);
  bool ok = encoded && private_write(temporary, encoded) &&
            rename(temporary, path) == 0;
  g_free(encoded);
  g_free(temporary);
  return ok;
}
bool config_marker(const char *path) {
  char *marker = config_sibling(path, ".om-default-off-migrated");
  if (!marker)
    return false;
  bool ok = parents(marker) &&
            private_write(marker, "observational memory home default is off\n");
  g_free(marker);
  return ok;
}
