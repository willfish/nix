#define _DEFAULT_SOURCE
#include "focus.h"
#include <math.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
bool focus_error;
Val *field(Val *v, const char *key) {
  Val *found = NULL, *k;
  yyjson_obj_iter it = yyjson_obj_iter_with(v);
  while ((k = yyjson_obj_iter_next(&it)))
    if (yyjson_get_len(k) == strlen(key) &&
        !memcmp(yyjson_get_str(k), key, strlen(key)))
      found = yyjson_obj_iter_get_val(k);
  return found;
}
bool truth(Val *v) {
  if (!v || yyjson_is_null(v))
    return false;
  if (yyjson_is_bool(v))
    return yyjson_get_bool(v);
  if (yyjson_is_num(v))
    return yyjson_get_num(v) != 0;
  return yyjson_get_len(v) != 0;
}
char *strip(const char *s) {
  const char *a = s, *b = s + strlen(s);
  while (*a && (g_unichar_isspace(g_utf8_get_char(a)) ||
                (g_utf8_get_char(a) >= 0x1c && g_utf8_get_char(a) <= 0x1f) ||
                g_utf8_get_char(a) == 0x85))
    a = g_utf8_next_char(a);
  while (b > a) {
    const char *p = g_utf8_find_prev_char(a, b);
    gunichar c = g_utf8_get_char(p);
    if (!g_unichar_isspace(c) && !(c >= 0x1c && c <= 0x1f) && c != 0x85)
      break;
    b = p;
  }
  return g_strndup(a, (gsize)(b - a));
}
char *string(Val *v) {
  if (!truth(v))
    return g_strdup("");
  if (yyjson_is_str(v)) {
    if (strlen(yyjson_get_str(v)) != yyjson_get_len(v)) {
      focus_error = true;
      return g_strdup("");
    }
    return g_strdup(yyjson_get_str(v));
  }
  if (yyjson_is_bool(v))
    return g_strdup("True");
  if (yyjson_is_sint(v))
    return g_strdup_printf("%" G_GINT64_FORMAT, (gint64)yyjson_get_sint(v));
  if (yyjson_is_uint(v))
    return g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)yyjson_get_uint(v));
  if (yyjson_is_raw(v))
    return g_strdup(yyjson_get_raw(v));
  if (yyjson_is_real(v))
    return g_strdup_printf("%.17g", yyjson_get_real(v));
  char *out = yyjson_val_write(v, YYJSON_WRITE_ALLOW_INF_AND_NAN, NULL);
  if (!out) {
    focus_error = true;
    return g_strdup("");
  }
  char *s = g_strdup(out);
  free(out);
  return s;
}
char *integer_text(const char *s) {
  if (!s || !g_utf8_validate(s, -1, NULL)) {
    focus_error = true;
    return NULL;
  }
  char *trim = strip(s);
  const char *p = trim;
  bool negative = *p == '-';
  if (*p == '+' || *p == '-')
    p++;
  GString *digits = g_string_new(NULL);
  bool previous = false, ok = *p;
  for (; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (c == '_') {
      if (!previous) {
        ok = false;
        break;
      }
      previous = false;
      continue;
    }
    int n = g_unichar_digit_value(c);
    if (n < 0 || n > 9) {
      ok = false;
      break;
    }
    g_string_append_c(digits, (char)('0' + n));
    previous = true;
  }
  ok = ok && previous && digits->len <= 4300;
  g_free(trim);
  if (!ok) {
    g_string_free(digits, true);
    focus_error = true;
    return NULL;
  }
  gsize first = 0;
  while (first + 1 < digits->len && digits->str[first] == '0')
    first++;
  char *out =
      g_strconcat(negative && strcmp(digits->str + first, "0") ? "-" : "",
                  digits->str + first, NULL);
  g_string_free(digits, true);
  return out;
}
char *integer(Val *v) {
  if (!truth(v))
    return g_strdup("0");
  if (yyjson_is_bool(v))
    return g_strdup("1");
  if (yyjson_is_str(v)) {
    char *s = string(v), *n = integer_text(s);
    g_free(s);
    return n;
  }
  if (yyjson_is_int(v) || yyjson_is_uint(v) || yyjson_is_raw(v)) {
    char *s = string(v), *n = integer_text(s);
    g_free(s);
    return n;
  }
  if (yyjson_is_real(v) && isfinite(yyjson_get_real(v))) {
    char *s = g_strdup_printf("%.0f", trunc(yyjson_get_real(v))),
         *n = integer_text(s);
    g_free(s);
    return n;
  }
  focus_error = true;
  return NULL;
}
int integer_compare(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  bool an = *a == '-', bn = *b == '-';
  if (an != bn)
    return an ? -1 : 1;
  if (an)
    a++;
  if (bn)
    b++;
  size_t al = strlen(a), bl = strlen(b);
  int c = al < bl ? -1 : al > bl ? 1 : strcmp(a, b);
  return an ? -c : c;
}
bool equal(Val *a, Val *b) {
  if (!a || yyjson_is_null(a))
    return !b || yyjson_is_null(b);
  if (!b)
    return false;
  if ((yyjson_is_num(a) || yyjson_is_bool(a) || yyjson_is_raw(a)) &&
      (yyjson_is_num(b) || yyjson_is_bool(b) || yyjson_is_raw(b))) {
    if ((yyjson_is_real(a) &&
         trunc(yyjson_get_real(a)) != yyjson_get_real(a)) ||
        (yyjson_is_real(b) && trunc(yyjson_get_real(b)) != yyjson_get_real(b)))
      return yyjson_get_num(a) == yyjson_get_num(b);
    char *x = integer(a), *y = integer(b);
    bool same = x && y && !strcmp(x, y);
    g_free(x);
    g_free(y);
    return same;
  }
  return yyjson_equals(a, b);
}
char *normal_path(const char *s) {
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
char *home_path(void) {
  const char *s = g_getenv("HOME");
  if (s)
    return normal_path(*s ? s : "/");
  struct passwd *pw = getpwuid(getuid());
  if (!pw) {
    focus_error = true;
    return NULL;
  }
  return normal_path(pw->pw_dir);
}
char *json(Doc *d) {
  char *raw = yyjson_mut_write(
      d, YYJSON_WRITE_ESCAPE_UNICODE | YYJSON_WRITE_ALLOW_INF_AND_NAN, NULL);
  if (!raw) {
    focus_error = true;
    return NULL;
  }
  GString *out = g_string_new(NULL);
  bool quote = false, escape = false;
  for (const char *p = raw; *p; p++) {
    g_string_append_c(out, *p);
    if (quote) {
      if (escape)
        escape = false;
      else if (*p == '\\')
        escape = true;
      else if (*p == '"')
        quote = false;
    } else if (*p == '"')
      quote = true;
    else if (*p == ',' || *p == ':')
      g_string_append_c(out, ' ');
  }
  free(raw);
  return g_string_free(out, false);
}
bool utf8_bytes(const char *bytes, size_t length) {
  while (length) {
    const char *zero = memchr(bytes, 0, length);
    size_t part = zero ? (size_t)(zero - bytes) : length;
    if (!g_utf8_validate(bytes, (gssize)part, NULL))
      return false;
    bytes += part;
    length -= part;
    if (zero) {
      bytes++;
      length--;
    }
  }
  return true;
}
yyjson_doc *read_json(const char *path, bool tolerant) {
  char *s = NULL;
  gsize n;
  if (!g_file_get_contents(path, &s, &n, NULL))
    return NULL;
  if (!utf8_bytes(s, n)) {
    focus_error = true;
    g_free(s);
    return NULL;
  }
  yyjson_doc *d = yyjson_read(
      s, n, YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_BIGNUM_AS_RAW);
  g_free(s);
  if (!d && !tolerant)
    focus_error = true;
  return d;
}
Doc *empty_object(void) {
  Doc *d = yyjson_mut_doc_new(NULL);
  yyjson_mut_doc_set_root(d, yyjson_mut_obj(d));
  return d;
}
void member(Doc *d, Mut *o, const char *key, Val *v) {
  yyjson_mut_obj_add_val(d, o, key,
                         v ? yyjson_val_mut_copy(d, v) : yyjson_mut_null(d));
}
