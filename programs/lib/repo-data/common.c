#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
Val *field(Val *object, const char *name) {
  return yyjson_obj_get(object, name);
}
bool text_is(Val *v, const char *s) {
  return yyjson_is_str(v) && yyjson_get_len(v) == strlen(s) &&
         !memcmp(yyjson_get_str(v), s, strlen(s));
}
bool nonempty(Val *v) {
  if (!yyjson_is_str(v) || !yyjson_get_len(v))
    return false;
  const unsigned char *p = (const unsigned char *)yyjson_get_str(v);
  for (size_t i = 0; i < yyjson_get_len(v); i++)
    if (p[i] < 32)
      return false;
  return true;
}
bool integer(Val *v) {
  if (yyjson_is_int(v))
    return true;
  if (!yyjson_is_raw(v))
    return false;
  const char *s = yyjson_get_raw(v);
  size_t n = yyjson_get_len(v), i = *s == '-' ? 1 : 0;
  if (i == n || n - i > 4300)
    return false;
  for (; i < n; i++)
    if (s[i] < '0' || s[i] > '9')
      return false;
  return true;
}
bool same(Val *a, Val *b) {
  if (!a || !b)
    return a == b;
  if (yyjson_get_type(a) != yyjson_get_type(b))
    return false;
  if (yyjson_is_obj(a)) {
    if (yyjson_obj_size(a) != yyjson_obj_size(b))
      return false;
    Val *key, *value;
    size_t i, n;
    yyjson_obj_foreach(
        a, i, n, key,
        value) if (!same(value,
                         yyjson_obj_getn(b, yyjson_get_str(key),
                                         yyjson_get_len(key)))) return false;
    return true;
  }
  if (yyjson_is_arr(a)) {
    if (yyjson_arr_size(a) != yyjson_arr_size(b))
      return false;
    Val *item;
    size_t i, n;
    yyjson_arr_foreach(
        a, i, n, item) if (!same(item, yyjson_arr_get(b, i))) return false;
    return true;
  }
  if (yyjson_is_str(a) || yyjson_is_raw(a))
    return yyjson_get_len(a) == yyjson_get_len(b) &&
           !memcmp(yyjson_get_str(a) ? yyjson_get_str(a) : yyjson_get_raw(a),
                   yyjson_get_str(b) ? yyjson_get_str(b) : yyjson_get_raw(b),
                   yyjson_get_len(a));
  if (yyjson_is_bool(a))
    return yyjson_get_bool(a) == yyjson_get_bool(b);
  if (yyjson_is_null(a))
    return true;
  if (yyjson_is_int(a) && yyjson_is_int(b)) {
    bool an = yyjson_is_sint(a) && yyjson_get_sint(a) < 0,
         bn = yyjson_is_sint(b) && yyjson_get_sint(b) < 0;
    return an == bn && (an ? yyjson_get_sint(a) == yyjson_get_sint(b)
                           : yyjson_get_uint(a) == yyjson_get_uint(b));
  }
  return yyjson_get_subtype(a) == yyjson_get_subtype(b) && yyjson_equals(a, b);
}
bool unique(Val *v, unsigned depth) {
  if (depth > 1000)
    return false;
  Val *key, *item;
  size_t i, n;
  if (yyjson_is_obj(v)) {
    GHashTable *keys = g_hash_table_new_full(
        g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref, NULL);
    bool ok = true;
    yyjson_obj_foreach(v, i, n, key, item) {
      GBytes *name = g_bytes_new(yyjson_get_str(key), yyjson_get_len(key));
      if (g_hash_table_contains(keys, name)) {
        g_bytes_unref(name);
        ok = false;
        break;
      }
      g_hash_table_add(keys, name);
      if (!unique(item, depth + 1)) {
        ok = false;
        break;
      }
    }
    g_hash_table_destroy(keys);
    return ok;
  }
  if (yyjson_is_arr(v)) {
    yyjson_arr_foreach(v, i, n,
                       item) if (!unique(item, depth + 1)) return false;
  }
  return true;
}
GString *read_text(const char *path) {
  char *data = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &data, &length, NULL))
    return NULL;
  for (size_t at = 0; at < length;) {
    const char *end = memchr(data + at, 0, length - at);
    size_t n = end ? (size_t)(end - data - at) : length - at;
    if (!g_utf8_validate(data + at, (gssize)n, NULL)) {
      g_free(data);
      return NULL;
    }
    at += n + 1;
  }
  GString *text = g_string_sized_new(length);
  for (size_t i = 0; i < length; i++) {
    char c = data[i];
    if (c == '\r') {
      c = '\n';
      if (i + 1 < length && data[i + 1] == '\n')
        i++;
    }
    g_string_append_c(text, c);
  }
  g_free(data);
  return text;
}
yyjson_doc *load_json(const char *path) {
  GString *s = read_text(path);
  if (!s)
    return NULL;
  yyjson_doc *d =
      yyjson_read(s->str, s->len,
                  YYJSON_READ_BIGNUM_AS_RAW | YYJSON_READ_ALLOW_INF_AND_NAN);
  g_string_free(s, TRUE);
  return d;
}
bool write_text(const char *path, const char *data, size_t length) {
  FILE *f = fopen(path, "w");
  if (!f)
    return false;
  bool ok = fwrite(data, 1, length, f) == length;
  if (fclose(f) != 0)
    ok = false;
  return ok;
}
bool unicode_space(gunichar c) {
  return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 0x85 ||
         c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
         c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
         c == 0x3000;
}
char *strip(const char *text) {
  const char *a = text, *b = text + strlen(text);
  while (*a && unicode_space(g_utf8_get_char(a)))
    a = g_utf8_next_char(a);
  while (b > a) {
    const char *p = g_utf8_find_prev_char(a, b);
    if (!unicode_space(g_utf8_get_char(p)))
      break;
    b = p;
  }
  return g_strndup(a, (size_t)(b - a));
}
