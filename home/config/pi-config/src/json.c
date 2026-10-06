#include "json.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *p, *end;
  bool bad;
  size_t depth;
} Parser;
static uint32_t utf8(const unsigned char *s, size_t left, size_t *width,
                     bool surrogate) {
  if (!left) {
    *width = 0;
    return 0;
  }
  unsigned first = s[0];
  size_t n = first < 0x80                     ? 1
             : first >= 0xc2 && first < 0xe0  ? 2
             : first >= 0xe0 && first < 0xf0  ? 3
             : first >= 0xf0 && first <= 0xf4 ? 4
                                              : 0;
  if (!n || n > left)
    goto invalid;
  uint32_t cp = first & (n == 1 ? 0x7f : n == 2 ? 0x1f : n == 3 ? 0xf : 7);
  for (size_t i = 1; i < n; i++) {
    if ((s[i] & 0xc0) != 0x80)
      goto invalid;
    cp = (cp << 6) | (s[i] & 0x3f);
  }
  if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
      (n == 4 && cp < 0x10000) || cp > 0x10ffff ||
      (!surrogate && cp >= 0xd800 && cp <= 0xdfff))
    goto invalid;
  *width = n;
  return cp;
invalid:
  *width = 1;
  return 0xdc00 + first;
}
static void character(GString *s, uint32_t c) {
  char b[4];
  size_t n;
  if (c < 0x80) {
    b[0] = (char)c;
    n = 1;
  } else if (c < 0x800) {
    b[0] = (char)(0xc0 | (c >> 6));
    b[1] = (char)(0x80 | (c & 63));
    n = 2;
  } else if (c < 0x10000) {
    b[0] = (char)(0xe0 | (c >> 12));
    b[1] = (char)(0x80 | ((c >> 6) & 63));
    b[2] = (char)(0x80 | (c & 63));
    n = 3;
  } else {
    b[0] = (char)(0xf0 | (c >> 18));
    b[1] = (char)(0x80 | ((c >> 12) & 63));
    b[2] = (char)(0x80 | ((c >> 6) & 63));
    b[3] = (char)(0x80 | (c & 63));
    n = 4;
  }
  g_string_append_len(s, b, (gssize)n);
}
GString *json_argument(const char *text) {
  GString *out = g_string_new(NULL);
  size_t length = strlen(text);
  for (size_t i = 0; i < length;) {
    size_t width;
    uint32_t cp =
        utf8((const unsigned char *)text + i, length - i, &width, false);
    character(out, cp);
    i += width;
  }
  return out;
}
static void free_string(void *value) { g_string_free(value, true); }
Json *json_ref(Json *value) {
  if (value)
    value->refs++;
  return value;
}
void json_free(Json *value) {
  if (!value || --value->refs)
    return;
  if (value->string)
    g_string_free(value->string, true);
  if (value->values)
    g_ptr_array_free(value->values, true);
  if (value->keys)
    g_ptr_array_free(value->keys, true);
  g_free(value);
}
Json *json_node(enum Kind kind) {
  Json *out = g_new0(Json, 1);
  out->kind = kind;
  out->refs = 1;
  if (kind == J_ARRAY || kind == J_OBJECT)
    out->values = g_ptr_array_new_with_free_func((GDestroyNotify)json_free);
  if (kind == J_OBJECT)
    out->keys = g_ptr_array_new_with_free_func(free_string);
  return out;
}
static void spaces(Parser *p) {
  while (p->p < p->end && strchr(" \t\r\n", *p->p))
    p->p++;
}
static bool literal(Parser *p, const char *s) {
  size_t n = strlen(s);
  if ((size_t)(p->end - p->p) < n || memcmp(p->p, s, n))
    return false;
  p->p += n;
  return true;
}
static int hex(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}
static bool codepoint(Parser *p, uint32_t *out) {
  if (p->end - p->p < 4)
    return false;
  *out = 0;
  for (size_t i = 0; i < 4; i++) {
    int n = hex(p->p[i]);
    if (n < 0)
      return false;
    *out = (*out << 4) | (unsigned)n;
  }
  p->p += 4;
  return true;
}
static GString *string(Parser *p) {
  if (p->p == p->end || *p->p++ != '"')
    return NULL;
  GString *out = g_string_new(NULL);
  while (p->p < p->end) {
    unsigned char c = (unsigned char)*p->p++;
    if (c == '"')
      return out;
    if (c < 32)
      break;
    if (c != '\\') {
      g_string_append_c(out, (char)c);
      continue;
    }
    if (p->p == p->end)
      break;
    c = (unsigned char)*p->p++;
    const char *escapes = "\"\\/bfnrt", *at = strchr(escapes, c);
    if (at) {
      const char values[] = {'"', '\\', '/', '\b', '\f', '\n', '\r', '\t'};
      g_string_append_c(out, values[at - escapes]);
      continue;
    }
    uint32_t cp;
    if (c != 'u' || !codepoint(p, &cp))
      break;
    if (cp >= 0xd800 && cp <= 0xdbff && p->end - p->p >= 6 && p->p[0] == '\\' &&
        p->p[1] == 'u') {
      Parser copy = *p;
      copy.p += 2;
      uint32_t low;
      if (codepoint(&copy, &low) && low >= 0xdc00 && low <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        *p = copy;
      }
    }
    character(out, cp);
  }
  g_string_free(out, true);
  p->bad = true;
  return NULL;
}
static bool digit(char c) { return c >= '0' && c <= '9'; }
static Json *value(Parser *p);
static Json *container(Parser *p, bool object) {
  if (++p->depth > 8192) {
    p->bad = true;
    p->depth--;
    return NULL;
  }
  char end = object ? '}' : ']';
  p->p++;
  Json *out = json_node(object ? J_OBJECT : J_ARRAY);
  spaces(p);
  if (p->p < p->end && *p->p == end) {
    p->p++;
    p->depth--;
    return out;
  }
  while (p->p < p->end) {
    GString *key = NULL;
    if (object) {
      key = string(p);
      if (!key)
        goto error;
      spaces(p);
      if (p->p == p->end || *p->p++ != ':') {
        g_string_free(key, true);
        goto error;
      }
    }
    Json *item = value(p);
    if (!item) {
      if (key)
        g_string_free(key, true);
      goto error;
    }
    size_t slot = out->values->len;
    if (object)
      for (size_t i = 0; i < out->keys->len; i++) {
        GString *existing = out->keys->pdata[i];
        if (existing->len == key->len &&
            !memcmp(existing->str, key->str, key->len)) {
          slot = i;
          break;
        }
      }
    if (slot < out->values->len) {
      json_free(out->values->pdata[slot]);
      out->values->pdata[slot] = item;
      g_string_free(key, true);
    } else {
      g_ptr_array_add(out->values, item);
      if (object)
        g_ptr_array_add(out->keys, key);
    }
    spaces(p);
    if (p->p == p->end)
      goto error;
    if (*p->p == end) {
      p->p++;
      p->depth--;
      return out;
    }
    if (*p->p++ != ',')
      goto error;
    spaces(p);
  }
error:
  p->bad = true;
  p->depth--;
  json_free(out);
  return NULL;
}
static Json *number(Parser *p) {
  const char *start = p->p;
  if (*p->p == '-')
    p->p++;
  if (p->p == p->end || !digit(*p->p))
    return NULL;
  if (*p->p == '0')
    p->p++;
  else
    while (p->p < p->end && digit(*p->p))
      p->p++;
  bool real = false;
  if (p->p < p->end && *p->p == '.') {
    real = true;
    p->p++;
    if (p->p == p->end || !digit(*p->p))
      return NULL;
    while (p->p < p->end && digit(*p->p))
      p->p++;
  }
  if (p->p < p->end && (*p->p == 'e' || *p->p == 'E')) {
    real = true;
    p->p++;
    if (p->p < p->end && (*p->p == '+' || *p->p == '-'))
      p->p++;
    if (p->p == p->end || !digit(*p->p))
      return NULL;
    while (p->p < p->end && digit(*p->p))
      p->p++;
  }
  size_t n = (size_t)(p->p - start);
  if (!real && n - (*start == '-') > 4300)
    return NULL;
  Json *out = json_node(real ? J_REAL : J_INT);
  if (real) {
    char *s = g_strndup(start, n);
    out->real = g_ascii_strtod(s, NULL);
    g_free(s);
  } else
    out->string =
        g_string_new_len(n == 2 && !memcmp(start, "-0", 2) ? "0" : start,
                         (gssize)(n == 2 && !memcmp(start, "-0", 2) ? 1 : n));
  return out;
}
static Json *value(Parser *p) {
  spaces(p);
  if (p->p == p->end)
    return NULL;
  char c = *p->p;
  if (c == '{' || c == '[')
    return container(p, c == '{');
  if (c == '"') {
    GString *s = string(p);
    if (!s)
      return NULL;
    Json *out = json_node(J_STRING);
    out->string = s;
    return out;
  }
  if (literal(p, "null"))
    return json_node(J_NULL);
  if (literal(p, "true")) {
    Json *out = json_node(J_BOOL);
    out->boolean = true;
    return out;
  }
  if (literal(p, "false"))
    return json_node(J_BOOL);
  double special;
  if (literal(p, "NaN"))
    special = NAN;
  else if (literal(p, "Infinity"))
    special = INFINITY;
  else if (literal(p, "-Infinity"))
    special = -INFINITY;
  else
    return c == '-' || digit(c) ? number(p) : NULL;
  Json *out = json_node(J_REAL);
  out->real = special;
  return out;
}
Json *json_parse(const char *input) {
  Parser p = {input, input + strlen(input), false, 0};
  Json *out = value(&p);
  spaces(&p);
  if (p.bad || p.p != p.end) {
    json_free(out);
    return NULL;
  }
  return out;
}
static size_t slot(Json *object, const char *key, size_t length) {
  if (!object || object->kind != J_OBJECT)
    return SIZE_MAX;
  for (size_t i = 0; i < object->keys->len; i++) {
    GString *k = object->keys->pdata[i];
    if (k->len == length && !memcmp(k->str, key, length))
      return i;
  }
  return SIZE_MAX;
}
Json *json_get(Json *object, const char *key, size_t length) {
  size_t i = slot(object, key, length);
  return i == SIZE_MAX ? NULL : object->values->pdata[i];
}
void json_set(Json *object, const char *key, size_t length, Json *value) {
  size_t i = slot(object, key, length);
  if (i == SIZE_MAX) {
    g_ptr_array_add(object->keys, g_string_new_len(key, (gssize)length));
    g_ptr_array_add(object->values, value);
  } else {
    json_free(object->values->pdata[i]);
    object->values->pdata[i] = value;
  }
}
bool json_remove(Json *object, const char *key, size_t length) {
  size_t i = slot(object, key, length);
  if (i == SIZE_MAX)
    return false;
  g_ptr_array_remove_index(object->keys, i);
  g_ptr_array_remove_index(object->values, i);
  return true;
}
Json *json_text(const char *text, size_t length) {
  Json *value = json_node(J_STRING);
  value->string = g_string_new_len(text, (gssize)length);
  return value;
}
static void quoted(GString *out, GString *s) {
  g_string_append_c(out, '"');
  for (size_t i = 0; i < s->len;) {
    size_t width;
    uint32_t c =
        utf8((const unsigned char *)s->str + i, s->len - i, &width, true);
    i += width;
    switch (c) {
    case '"':
      g_string_append(out, "\\\"");
      break;
    case '\\':
      g_string_append(out, "\\\\");
      break;
    case '\b':
      g_string_append(out, "\\b");
      break;
    case '\f':
      g_string_append(out, "\\f");
      break;
    case '\n':
      g_string_append(out, "\\n");
      break;
    case '\r':
      g_string_append(out, "\\r");
      break;
    case '\t':
      g_string_append(out, "\\t");
      break;
    default:
      if (c >= 32 && c <= 126)
        g_string_append_c(out, (char)c);
      else if (c <= 0xffff)
        g_string_append_printf(out, "\\u%04x", c);
      else {
        c -= 0x10000;
        g_string_append_printf(out, "\\u%04x\\u%04x", 0xd800 + (c >> 10),
                               0xdc00 + (c & 1023));
      }
    }
  }
  g_string_append_c(out, '"');
}
static void real_text(GString *out, double n) {
  if (isnan(n)) {
    g_string_append(out, "NaN");
    return;
  }
  if (isinf(n)) {
    g_string_append(out, signbit(n) ? "-Infinity" : "Infinity");
    return;
  }
  char candidate[64], format[16];
  uint64_t wanted, actual;
  memcpy(&wanted, &n, sizeof wanted);
  // Select the closest shortest decimal that round-trips to the same double,
  // then apply Python's fixed/scientific thresholds rather than printf's.
  for (int precision = 1; precision <= 17; precision++) {
    snprintf(format, sizeof format, "%%.%dg", precision);
    g_ascii_formatd(candidate, sizeof candidate, format, n);
    double parsed = g_ascii_strtod(candidate, NULL);
    memcpy(&actual, &parsed, sizeof actual);
    if (actual == wanted)
      break;
  }
  const char *s = candidate;
  if (*s == '-') {
    g_string_append_c(out, '-');
    s++;
  }
  const char *e = strchr(s, 'e');
  int exponent = e ? (int)strtol(e + 1, NULL, 10) : 0;
  GString *digits = g_string_new(NULL);
  int before = 0;
  bool point = false;
  for (const char *p = s; *p && p != e; p++) {
    if (*p == '.') {
      point = true;
      continue;
    }
    g_string_append_c(digits, *p);
    if (!point)
      before++;
  }
  exponent += before - 1;
  while (digits->len > 1 && digits->str[0] == '0') {
    g_string_erase(digits, 0, 1);
    exponent--;
  }
  if (exponent < -4 || exponent >= 16) {
    g_string_append_c(out, digits->str[0]);
    if (digits->len > 1) {
      g_string_append_c(out, '.');
      g_string_append_len(out, digits->str + 1, (gssize)digits->len - 1);
    }
    g_string_append_printf(out, "e%c%02d", exponent < 0 ? '-' : '+',
                           exponent < 0 ? -exponent : exponent);
  } else {
    int at = exponent + 1;
    if (at <= 0) {
      g_string_append(out, "0.");
      for (int i = 0; i < -at; i++)
        g_string_append_c(out, '0');
      g_string_append_len(out, digits->str, (gssize)digits->len);
    } else {
      for (int i = 0; i < at; i++)
        g_string_append_c(out, (size_t)i < digits->len ? digits->str[i] : '0');
      g_string_append_c(out, '.');
      if ((size_t)at < digits->len)
        g_string_append_len(out, digits->str + at, (gssize)digits->len - at);
      else
        g_string_append_c(out, '0');
    }
  }
  g_string_free(digits, true);
}
static bool encode(GString *out, Json *value, size_t depth) {
  if (value->kind == J_ARRAY || value->kind == J_OBJECT)
    depth++;
  switch (value->kind) {
  case J_NULL:
    g_string_append(out, "null");
    break;
  case J_BOOL:
    g_string_append(out, value->boolean ? "true" : "false");
    break;
  case J_INT:
    g_string_append_len(out, value->string->str, (gssize)value->string->len);
    break;
  case J_REAL:
    real_text(out, value->real);
    break;
  case J_STRING:
    quoted(out, value->string);
    break;
  case J_ARRAY:
  case J_OBJECT: {
    bool obj = value->kind == J_OBJECT;
    g_string_append_c(out, obj ? '{' : '[');
    if (value->values->len)
      g_string_append_c(out, '\n');
    for (size_t i = 0; i < value->values->len; i++) {
      if (i)
        g_string_append(out, ",\n");
      for (size_t n = 0; n < depth * 2; n++)
        g_string_append_c(out, ' ');
      if (obj) {
        quoted(out, value->keys->pdata[i]);
        g_string_append(out, ": ");
      }
      if (!encode(out, value->values->pdata[i], depth))
        return false;
    }
    if (value->values->len) {
      g_string_append_c(out, '\n');
      for (size_t n = 0; n < (depth - 1) * 2; n++)
        g_string_append_c(out, ' ');
    }
    g_string_append_c(out, obj ? '}' : ']');
    break;
  }
  }
  return true;
}
char *json_encode(Json *value) {
  GString *out = g_string_new(NULL);
  if (!encode(out, value, 0)) {
    g_string_free(out, true);
    return NULL;
  }
  g_string_append_c(out, '\n');
  return g_string_free(out, false);
}
