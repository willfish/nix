#define _DEFAULT_SOURCE
#include "theme.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
yyjson_val *field(yyjson_val *object, const char *key) {
  yyjson_val *found = NULL, *k, *v;
  size_t i, n;
  yyjson_obj_foreach(object, i, n, k, v) if (!strcmp(yyjson_get_str(k), key))
      found = v;
  return found;
}
bool truth(yyjson_val *v) {
  if (!v || yyjson_is_null(v))
    return false;
  if (yyjson_is_bool(v))
    return yyjson_get_bool(v);
  if (yyjson_is_str(v))
    return yyjson_get_len(v) > 0;
  if (yyjson_is_arr(v))
    return yyjson_arr_size(v) > 0;
  if (yyjson_is_obj(v))
    return yyjson_obj_size(v) > 0;
  if (yyjson_is_num(v))
    return yyjson_get_num(v) != 0;
  return false;
}
char *scalar(yyjson_val *v, const char *fallback) {
  if (!v)
    return g_strdup(fallback);
  if (yyjson_is_str(v))
    return g_strdup(yyjson_get_str(v));
  if (yyjson_is_null(v))
    return g_strdup("None");
  if (yyjson_is_bool(v))
    return g_strdup(yyjson_get_bool(v) ? "True" : "False");
  if (yyjson_is_sint(v))
    return g_strdup_printf("%" G_GINT64_FORMAT, (gint64)yyjson_get_sint(v));
  if (yyjson_is_uint(v))
    return g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)yyjson_get_uint(v));
  char *s = yyjson_val_write(v, 0, NULL);
  char *out = g_strdup(s ? s : fallback);
  free(s);
  return out;
}
const char *string(Themes *t, yyjson_val *v) {
  const char *s = yyjson_get_str(v);
  if (!s || strlen(s) != yyjson_get_len(v)) {
    fail(t, "Invalid theme catalogue string");
    return NULL;
  }
  return s;
}
GPtrArray *keys(yyjson_val *object) {
  GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  yyjson_val *k, *v;
  size_t i, n;
  yyjson_obj_foreach(object, i, n, k, v) {
    (void)v;
    const char *s = yyjson_get_str(k);
    if (!g_hash_table_contains(seen, s)) {
      char *copy = g_strdup(s);
      g_hash_table_add(seen, copy);
      g_ptr_array_add(list, copy);
    }
  }
  g_hash_table_destroy(seen);
  return list;
}
static bool space(gunichar c) {
  return g_unichar_isspace(c) || c == '\v' || c == 0x85 ||
         (c >= 0x1c && c <= 0x1f);
}
char *trim(const char *text) {
  const char *begin = text, *end = text + strlen(text);
  while (*begin && space(g_utf8_get_char(begin)))
    begin = g_utf8_next_char(begin);
  while (end > begin) {
    const char *last = g_utf8_find_prev_char(begin, end);
    if (!last || !space(g_utf8_get_char(last)))
      break;
    end = last;
  }
  return g_strndup(begin, (gsize)(end - begin));
}
char **lines(const char *text) {
  GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
  const char *start = text, *p = text;
  while (*p) {
    gunichar c = g_utf8_get_char(p);
    const char *next = g_utf8_next_char(p);
    if (c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
        (c >= 0x1c && c <= 0x1e) || c == 0x85 || c == 0x2028 || c == 0x2029) {
      g_ptr_array_add(parts, g_strndup(start, (gsize)(p - start)));
      if (c == '\r' && *next == '\n')
        next++;
      start = next;
    }
    p = next;
  }
  if (p > start)
    g_ptr_array_add(parts, g_strndup(start, (gsize)(p - start)));
  g_ptr_array_add(parts, NULL);
  return (char **)g_ptr_array_free(parts, false);
}
char *path_join(const char *root, const char *name) {
  char *joined = g_path_is_absolute(name) ? g_strdup(name)
                                          : g_build_filename(root, name, NULL);
  char **parts = g_strsplit(joined, "/", -1);
  GString *out = g_string_new(*joined == '/' ? "/" : "");
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    if (out->len && out->str[out->len - 1] != '/')
      g_string_append_c(out, '/');
    g_string_append(out, parts[i]);
  }
  if (!out->len)
    g_string_append_c(out, '.');
  g_strfreev(parts);
  g_free(joined);
  return g_string_free(out, false);
}
bool fail(Themes *t, const char *format, ...) {
  va_list args;
  va_start(args, format);
  if (!t->error)
    t->error = g_strdup_vprintf(format, args);
  va_end(args);
  return false;
}
void warn(Themes *t, const char *text) {
  g_ptr_array_add(t->warnings, g_strdup(text));
}
bool exists(const char *path) { return g_file_test(path, G_FILE_TEST_EXISTS); }
bool path_is_symlink(const char *path) {
  return g_file_test(path, G_FILE_TEST_IS_SYMLINK);
}
GBytes *read_bytes(Themes *t, const char *path) {
  char *data = NULL;
  gsize len;
  GError *e = NULL;
  if (!g_file_get_contents(path, &data, &len, &e)) {
    fail(t, "%s", e->message);
    g_error_free(e);
    return NULL;
  }
  return g_bytes_new_take(data, len);
}
char *read_text(Themes *t, const char *path) {
  GBytes *data = read_bytes(t, path);
  if (!data)
    return NULL;
  gsize len;
  const char *raw = g_bytes_get_data(data, &len);
  if (!g_utf8_validate(raw, (gssize)len, NULL)) {
    fail(t, "Invalid UTF-8 in %s", path);
    g_bytes_unref(data);
    return NULL;
  }
  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < len; i++) {
    if (raw[i] == '\r') {
      g_string_append_c(s, '\n');
      if (i + 1 < len && raw[i + 1] == '\n')
        i++;
    } else
      g_string_append_c(s, raw[i]);
  }
  g_bytes_unref(data);
  return g_string_free(s, false);
}
bool remove_file(Themes *t, const char *path) {
  return unlink(path) == 0 || errno == ENOENT ||
         fail(t, "Could not remove %s: %s", path, g_strerror(errno));
}
bool atomic_write(Themes *t, const char *path, GBytes *content) {
  char *parent = g_path_get_dirname(path), *base = g_path_get_basename(path);
  bool ok = g_mkdir_with_parents(parent, 0777) == 0;
  if (!ok)
    fail(t, "Could not create %s: %s", parent, g_strerror(errno));
  char *name = g_strdup_printf("%s/.%s.XXXXXX", parent, base);
  int fd = ok ? g_mkstemp(name) : -1;
  if (ok && fd < 0)
    ok = fail(t, "Could not create temporary file for %s: %s", path,
              g_strerror(errno));
  if (ok && t->fail_at >= 0 && t->atomic_count++ == t->fail_at)
    ok = fail(t, "Injected atomic write failure");
  gsize len;
  const char *data = g_bytes_get_data(content, &len);
  size_t done = 0;
  while (ok && done < len) {
    ssize_t n = write(fd, data + done, len - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      ok = fail(t, "Could not write %s: %s", path, g_strerror(errno));
    else
      done += (size_t)n;
  }
  if (fd >= 0 && close(fd))
    ok = fail(t, "Could not close %s: %s", path, g_strerror(errno));
  if (ok && rename(name, path))
    ok = fail(t, "Could not replace %s: %s", path, g_strerror(errno));
  if (fd >= 0 && unlink(name) && errno != ENOENT)
    ok = fail(t, "Could not remove temporary file for %s", path);
  g_free(name);
  g_free(parent);
  g_free(base);
  return ok;
}
