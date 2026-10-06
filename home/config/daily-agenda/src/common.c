#define _DEFAULT_SOURCE
#include "agenda.h"
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
const char *agenda_error;
bool bad(const char *message) {
  agenda_error = message;
  return false;
}
yyjson_val *field(yyjson_val *o, const char *key) {
  yyjson_val *v = NULL, *k, *item;
  yyjson_obj_iter iter = yyjson_obj_iter_with(o);
  while ((k = yyjson_obj_iter_next(&iter))) {
    item = yyjson_obj_iter_get_val(k);
    if (yyjson_get_len(k) == strlen(key) &&
        !memcmp(yyjson_get_str(k), key, strlen(key)))
      v = item;
  }
  return v;
}
const char *text(yyjson_val *v) {
  return yyjson_is_str(v) && strlen(yyjson_get_str(v)) == yyjson_get_len(v)
             ? yyjson_get_str(v)
             : NULL;
}
bool truth(yyjson_val *v) {
  if (!v || yyjson_is_null(v))
    return false;
  if (yyjson_is_bool(v))
    return yyjson_get_bool(v);
  if (yyjson_is_str(v))
    return yyjson_get_len(v) > 0;
  if (yyjson_is_num(v))
    return yyjson_get_num(v) != 0;
  return yyjson_get_len(v) > 0;
}
GPtrArray *keys(yyjson_val *o) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  yyjson_val *k;
  yyjson_obj_iter iter = yyjson_obj_iter_with(o);
  while ((k = yyjson_obj_iter_next(&iter))) {
    const char *s = text(k);
    if (!s) {
      g_ptr_array_free(out, true);
      out = NULL;
      break;
    }
    if (!g_hash_table_contains(seen, s)) {
      g_ptr_array_add(out, g_strdup(s));
      g_hash_table_add(seen, (void *)s);
    }
  }
  g_hash_table_destroy(seen);
  return out;
}
static bool whitespace(gunichar c) {
  return g_unichar_isspace(c) || c == 0x85 || (c >= 0x1c && c <= 0x1f);
}
char *clean(const char *value, size_t limit) {
  if (!value)
    value = "";
  char *valid = g_utf8_make_valid(value, -1);
  GString *out = g_string_new(NULL);
  size_t count = 0;
  for (const char *p = valid; *p && count < limit; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    GUnicodeType t = g_unichar_type(c);
    if (t == G_UNICODE_CONTROL || t == G_UNICODE_FORMAT ||
        t == G_UNICODE_UNASSIGNED || t == G_UNICODE_PRIVATE_USE ||
        t == G_UNICODE_SURROGATE)
      continue;
    g_string_append_unichar(out, c);
    count++;
  }
  g_free(valid);
  return g_string_free(out, false);
}
size_t string_length(yyjson_val *v) {
  if (!yyjson_is_str(v))
    return 0;
  const char *s = yyjson_get_str(v), *end = s + yyjson_get_len(v);
  size_t n = 0;
  while (s < end) {
    s = g_utf8_next_char(s);
    n++;
  }
  return n;
}
char *clean_value(yyjson_val *v, size_t limit) {
  if (!yyjson_is_str(v))
    return clean("", limit);
  const char *s = yyjson_get_str(v);
  size_t n = yyjson_get_len(v);
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < n; i++)
    if (s[i])
      g_string_append_c(out, s[i]);
  char *value = clean(out->str, limit);
  g_string_free(out, true);
  return value;
}
char *strip(const char *value) {
  const char *first = value, *end = value + strlen(value);
  while (*first && whitespace(g_utf8_get_char(first)))
    first = g_utf8_next_char(first);
  while (end > first) {
    const char *p = g_utf8_find_prev_char(first, end);
    if (!p || !whitespace(g_utf8_get_char(p)))
      break;
    end = p;
  }
  return g_strndup(first, (size_t)(end - first));
}
char *redact(const char *value, GPtrArray *secrets, size_t limit) {
  char *out = clean(value, 2000);
  if (secrets)
    for (size_t i = 0; i < secrets->len; i++) {
      const char *secret = secrets->pdata[i];
      if (!*secret)
        continue;
      char **parts = g_strsplit(out, secret, -1);
      char *next = g_strjoinv("[redacted]", parts);
      g_strfreev(parts);
      g_free(out);
      out = next;
    }
  if ((size_t)g_utf8_strlen(out, -1) > limit) {
    char *end = g_utf8_offset_to_pointer(out, (glong)limit);
    *end = 0;
  }
  return out;
}
static char *normal_path(const char *s) {
  bool root = *s == '/', two = root && s[1] == '/' && s[2] != '/';
  GString *out = g_string_new(root ? (two ? "//" : "/") : "");
  char **parts = g_strsplit(s, "/", -1);
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
  return g_string_free(out, false);
}
char *path_home(void) {
  const char *home = g_getenv("HOME");
  if (home)
    return normal_path(*home ? home : "/");
  struct passwd *pw = getpwuid(getuid());
  return pw ? normal_path(pw->pw_dir) : NULL;
}
char *env_path(const char *variable, const char *suffix) {
  const char *value = g_getenv(variable);
  if (value)
    return normal_path(value);
  char *home = path_home();
  if (!home)
    return NULL;
  char *joined = g_build_filename(home, suffix, NULL),
       *out = normal_path(joined);
  g_free(home);
  g_free(joined);
  return out;
}
bool read_bytes(const char *path, size_t limit, GBytes **bytes) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return bad("could not read file");
  GByteArray *out = g_byte_array_new();
  unsigned char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, MIN(sizeof buf, limit + 1 - out->len), f))) {
    g_byte_array_append(out, buf, (guint)n);
    if (out->len > limit)
      break;
  }
  bool ok = !ferror(f) && out->len <= limit;
  fclose(f);
  if (!ok) {
    g_byte_array_unref(out);
    return bad("file too large or unreadable");
  }
  *bytes = g_byte_array_free_to_bytes(out);
  return true;
}
yyjson_doc *load_json(const char *path) {
  GBytes *b;
  if (!read_bytes(path, MAX_BYTES, &b))
    return NULL;
  gsize n;
  const char *s = g_bytes_get_data(b, &n);
  yyjson_doc *doc = yyjson_read(s, n, YYJSON_READ_ALLOW_INF_AND_NAN);
  g_bytes_unref(b);
  if (!doc)
    bad("invalid json");
  return doc;
}
char *json_string(yyjson_mut_doc *doc, bool ascii) {
  char *raw =
      yyjson_mut_write(doc, ascii ? YYJSON_WRITE_ESCAPE_UNICODE : 0, NULL);
  if (!raw)
    return NULL;
  GString *out = g_string_new(NULL);
  bool quoted = false, escaped = false;
  for (const char *p = raw; *p; p++) {
    g_string_append_c(out, *p);
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (*p == '\\')
        escaped = true;
      else if (*p == '"')
        quoted = false;
    } else if (*p == '"')
      quoted = true;
    else if (*p == ',' || *p == ':')
      g_string_append_c(out, ' ');
  }
  free(raw);
  return g_string_free(out, false);
}
bool save_cache(const char *path, yyjson_mut_doc *doc) {
  char *parent = g_path_get_dirname(path);
  bool ok = false;
  char *tmp = NULL, *json = NULL;
  int fd = -1;
  struct stat st;
  if (g_mkdir_with_parents(parent, 0700) < 0)
    goto done;
  if (!lstat(parent, &st) && S_ISLNK(st.st_mode))
    goto done;
  if (!lstat(path, &st) && S_ISLNK(st.st_mode))
    goto done;
  if (chmod(parent, 0700) < 0)
    goto done;
  tmp = g_build_filename(parent, ".calendar-XXXXXX", NULL);
  fd = g_mkstemp(tmp);
  if (fd < 0)
    goto done;
  json = json_string(doc, true);
  if (!json)
    goto done;
  size_t left = strlen(json);
  const char *p = json;
  while (left) {
    ssize_t n = write(fd, p, left);
    if (n <= 0)
      goto done;
    p += n;
    left -= (size_t)n;
  }
  if (fsync(fd) < 0)
    goto done;
  if (close(fd) < 0) {
    fd = -1;
    goto done;
  }
  fd = -1;
  if (rename(tmp, path) < 0)
    goto done;
  ok = true;
done:
  if (fd >= 0)
    close(fd);
  if (tmp)
    unlink(tmp);
  g_free(tmp);
  g_free(json);
  g_free(parent);
  return ok ? true : bad("unsafe or unwritable cache");
}
