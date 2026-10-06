#include "managed.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>
#include <yyjson.h>

bool failed;
static GPtrArray *arena;
static void release(void *p) {
  Value *v = p;
  g_free(v->text);
  g_free(v->tag);
  if (v->keys)
    g_ptr_array_free(v->keys, TRUE);
  if (v->values)
    g_ptr_array_free(v->values, TRUE);
  g_free(v);
}
void arena_start(void) {
  failed = false;
  arena = g_ptr_array_new_with_free_func(release);
}
void arena_end(void) {
  g_ptr_array_free(arena, TRUE);
  arena = NULL;
}
Value *value(Kind k) {
  Value *v = g_new0(Value, 1);
  v->kind = k;
  if (k == SEQUENCE || k == MAPPING)
    v->values = g_ptr_array_new();
  if (k == MAPPING)
    v->keys = g_ptr_array_new();
  g_ptr_array_add(arena, v);
  return v;
}
Value *string_n(const char *s, size_t n) {
  Value *v = value(TEXT);
  v->text = g_strndup(s, n);
  v->length = n;
  return v;
}
Value *string(const char *s) { return string_n(s, strlen(s)); }
Value *number(const char *s) {
  Value *v = string(s);
  v->kind = NUMBER;
  return v;
}
Value *boolean(bool b) {
  Value *v = value(BOOL);
  v->boolean = b;
  return v;
}
bool map(Value *v) { return v && v->kind == MAPPING && !v->tag; }
bool array(Value *v) { return v && v->kind == SEQUENCE && !v->tag; }
const char *cstring(Value *v) {
  if (!v || v->kind != TEXT || memchr(v->text, 0, v->length)) {
    failed = true;
    return "";
  }
  return v->text;
}
Value *get(Value *o, const char *key) {
  if (!map(o))
    return NULL;
  size_t n = strlen(key);
  for (size_t i = 0; i < o->keys->len; i++) {
    Value *k = o->keys->pdata[i];
    if (k->kind == TEXT && k->length == n && !memcmp(k->text, key, n))
      return o->values->pdata[i];
  }
  return NULL;
}
void put(Value *o, const char *key, Value *v) {
  if (!map(o) || !v) {
    failed = true;
    return;
  }
  size_t n = strlen(key);
  for (size_t i = 0; i < o->keys->len; i++) {
    Value *k = o->keys->pdata[i];
    if (k->kind == TEXT && k->length == n && !memcmp(k->text, key, n)) {
      o->values->pdata[i] = v;
      return;
    }
  }
  g_ptr_array_add(o->keys, string(key));
  g_ptr_array_add(o->values, v);
}
void append(Value *a, Value *v) {
  if (!array(a) || !v) {
    failed = true;
    return;
  }
  g_ptr_array_add(a->values, v);
}
static double numeric_value(Value *v) {
  if (!strcmp(v->text, ".nan"))
    return NAN;
  if (!strcmp(v->text, ".inf"))
    return INFINITY;
  if (!strcmp(v->text, "-.inf"))
    return -INFINITY;
  const char *p = v->text;
  bool negative = *p == '-';
  if (*p == '+' || *p == '-')
    p++;
  if (g_str_has_prefix(p, "0b"))
    return (negative ? -1.0 : 1.0) * (double)g_ascii_strtoull(p + 2, NULL, 2);
  return g_ascii_strtod(v->text, NULL);
}
bool truth(Value *v) {
  if (!v || v->kind == NIL)
    return false;
  if (v->kind == BOOL)
    return v->boolean;
  if (v->kind == NUMBER)
    return numeric_value(v) != 0;
  if (v->kind == TEXT || v->kind == SPECIAL)
    return v->length != 0;
  return v->values->len != 0;
}
static bool eq(Value *a, Value *b, unsigned depth) {
  if (a == b)
    return true;
  if (!a || !b)
    return (!a || a->kind == NIL) && (!b || b->kind == NIL);
  if (depth > 1000) {
    failed = true;
    return false;
  }
  if ((a->kind == NUMBER || a->kind == BOOL) &&
      (b->kind == NUMBER || b->kind == BOOL)) {
    double x = a->kind == BOOL ? a->boolean : numeric_value(a),
           y = b->kind == BOOL ? b->boolean : numeric_value(b);
    if (a->kind == NUMBER && b->kind == NUMBER && !strpbrk(a->text, ".eE") &&
        !strpbrk(b->text, ".eE"))
      return !strcmp(a->text, b->text);
    return x == y;
  }
  if (a->kind != b->kind || g_strcmp0(a->tag, b->tag))
    return false;
  if (a->kind == NIL)
    return true;
  if (a->kind == TEXT || a->kind == SPECIAL)
    return a->length == b->length && !memcmp(a->text, b->text, a->length);
  if (a->values->len != b->values->len)
    return false;
  for (size_t i = 0; i < a->values->len; i++) {
    size_t j = i;
    if (a->kind == MAPPING) {
      for (j = 0; j < b->keys->len; j++)
        if (eq(a->keys->pdata[i], b->keys->pdata[j], depth + 1))
          break;
      if (j == b->keys->len)
        return false;
    }
    if (!eq(a->values->pdata[i], b->values->pdata[j], depth + 1))
      return false;
  }
  return true;
}
bool equal(Value *a, Value *b) { return eq(a, b, 0); }
static void pair(Value *o, Value *k, Value *v) {
  for (size_t i = 0; i < o->keys->len; i++)
    if (equal(k, o->keys->pdata[i])) {
      o->values->pdata[i] = v;
      return;
    }
  g_ptr_array_add(o->keys, k);
  g_ptr_array_add(o->values, v);
}
static Value *clone(Value *v, GHashTable *seen) {
  if (!v)
    return value(NIL);
  if (v->kind != SEQUENCE && v->kind != MAPPING)
    return v;
  Value *out = g_hash_table_lookup(seen, v);
  if (out)
    return out;
  out = value(v->kind);
  g_hash_table_insert(seen, v, out);
  out->tag = g_strdup(v->tag);
  out->boolean = v->boolean;
  out->length = v->length;
  if (v->text)
    out->text = g_strndup(v->text, v->length);
  if (v->values)
    for (size_t i = 0; i < v->values->len; i++) {
      g_ptr_array_add(out->values, clone(v->values->pdata[i], seen));
      if (v->keys)
        g_ptr_array_add(out->keys, clone(v->keys->pdata[i], seen));
    }
  return out;
}
Value *copy(Value *v) {
  GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
  Value *out = clone(v, seen);
  g_hash_table_destroy(seen);
  return out;
}
static Value *from_json(yyjson_val *v, unsigned depth) {
  if (!v || depth > 1000) {
    failed = true;
    return value(NIL);
  }
  if (yyjson_is_null(v))
    return value(NIL);
  if (yyjson_is_bool(v))
    return boolean(yyjson_get_bool(v));
  if (yyjson_is_str(v))
    return string_n(yyjson_get_str(v), yyjson_get_len(v));
  if (yyjson_is_num(v) || yyjson_is_raw(v)) {
    if (yyjson_is_real(v) && !isfinite(yyjson_get_real(v)))
      return number(isnan(yyjson_get_real(v)) ? ".nan"
                    : yyjson_get_real(v) < 0  ? "-.inf"
                                              : ".inf");
    char *s = yyjson_val_write(v, YYJSON_WRITE_ALLOW_INF_AND_NAN, NULL);
    Value *n = number(s ? s : "0");
    free(s);
    return n;
  }
  Value *out = value(yyjson_is_arr(v) ? SEQUENCE : MAPPING);
  size_t i, n;
  yyjson_val *key, *item;
  if (out->kind == SEQUENCE) {
    yyjson_arr_foreach(v, i, n, item) append(out, from_json(item, depth + 1));
  } else
    yyjson_obj_foreach(v, i, n, key, item)
        pair(out, from_json(key, depth + 1), from_json(item, depth + 1));
  return out;
}
Value *parse_json(const char *s, size_t n) {
  yyjson_doc *d = yyjson_read(
      s, n, YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_BIGNUM_AS_RAW);
  if (!d) {
    failed = true;
    return NULL;
  }
  Value *v = from_json(yyjson_doc_get_root(d), 0);
  yyjson_doc_free(d);
  return v;
}
static yyjson_mut_val *to_json(yyjson_mut_doc *d, Value *v, unsigned depth) {
  if (!v || depth > 1000) {
    failed = true;
    return yyjson_mut_null(d);
  }
  switch (v->kind) {
  case NIL:
    return yyjson_mut_null(d);
  case BOOL:
    return yyjson_mut_bool(d, v->boolean);
  case NUMBER:
    return yyjson_mut_rawcpy(d, !strcmp(v->text, ".nan")    ? "NaN"
                                : !strcmp(v->text, ".inf")  ? "Infinity"
                                : !strcmp(v->text, "-.inf") ? "-Infinity"
                                                            : v->text);
  case TEXT:
    return yyjson_mut_strncpy(d, v->text, v->length);
  case SPECIAL:
    failed = true;
    return yyjson_mut_null(d);
  case SEQUENCE: {
    yyjson_mut_val *a = yyjson_mut_arr(d);
    for (size_t i = 0; i < v->values->len; i++)
      yyjson_mut_arr_add_val(a, to_json(d, v->values->pdata[i], depth + 1));
    return a;
  }
  case MAPPING: {
    yyjson_mut_val *o = yyjson_mut_obj(d);
    for (size_t i = 0; i < v->values->len; i++) {
      Value *k = v->keys->pdata[i];
      if (k->kind != TEXT) {
        failed = true;
        continue;
      }
      yyjson_mut_obj_add(o, yyjson_mut_strncpy(d, k->text, k->length),
                         to_json(d, v->values->pdata[i], depth + 1));
    }
    return o;
  }
  }
  return NULL;
}
GBytes *json_bytes(Value *v, bool pretty) {
  yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
  yyjson_mut_doc_set_root(d, to_json(d, v, 0));
  size_t n = 0;
  char *s =
      failed ? NULL
             : yyjson_mut_write(d,
                                (pretty ? YYJSON_WRITE_PRETTY_TWO_SPACES : 0) |
                                    YYJSON_WRITE_ESCAPE_UNICODE |
                                    YYJSON_WRITE_ALLOW_INF_AND_NAN,
                                &n);
  yyjson_mut_doc_free(d);
  if (!s) {
    failed = true;
    return NULL;
  }
  GString *out = g_string_new_len(s, (gssize)n);
  free(s);
  g_string_append_c(out, '\n');
  n = out->len;
  return g_bytes_new_take(g_string_free(out, FALSE), n);
}
char *as_text(Value *v) {
  if (!v || v->kind == NIL)
    return g_strdup("None");
  if (v->kind == BOOL)
    return g_strdup(v->boolean ? "True" : "False");
  if (v->kind == TEXT)
    return g_strdup(cstring(v));
  if (v->kind == NUMBER)
    return g_strdup(v->text);
  failed = true;
  return g_strdup("");
}
static bool word(const char *s, const char *lower) {
  char *upper = g_ascii_strup(lower, -1), *title = g_strdup(lower);
  title[0] = g_ascii_toupper(title[0]);
  bool ok = !strcmp(s, lower) || !strcmp(s, upper) || !strcmp(s, title);
  g_free(upper);
  g_free(title);
  return ok;
}
static Value *scalar(const yaml_event_t *event) {
  const char *s = (const char *)event->data.scalar.value,
             *tag = (const char *)event->data.scalar.tag;
  size_t n = event->data.scalar.length;
  Value *v = string_n(s, n);
  bool implicit = event->data.scalar.plain_implicit &&
                  event->data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
  if (tag && !strcmp(tag, YAML_STR_TAG))
    return v;
  if ((tag && !strcmp(tag, YAML_NULL_TAG)) ||
      (implicit && (!n || !strcmp(s, "~") || word(s, "null")))) {
    v->kind = NIL;
    return v;
  }
  if ((tag && !strcmp(tag, YAML_BOOL_TAG)) ||
      (implicit && (word(s, "true") || word(s, "false") || word(s, "yes") ||
                    word(s, "no") || word(s, "on") || word(s, "off")))) {
    if (tag && !word(s, "true") && !word(s, "false") && !word(s, "yes") &&
        !word(s, "no") && !word(s, "on") && !word(s, "off"))
      failed = true;
    v->kind = BOOL;
    v->boolean = word(s, "true") || word(s, "yes") || word(s, "on");
    return v;
  }
  if (tag && strcmp(tag, YAML_INT_TAG) && strcmp(tag, YAML_FLOAT_TAG)) {
    if (strcmp(tag, "tag:yaml.org,2002:timestamp") &&
        strcmp(tag, "tag:yaml.org,2002:binary") &&
        strcmp(tag, "tag:yaml.org,2002:merge")) {
      failed = true;
      return v;
    }
    v->kind = SPECIAL;
    v->tag = g_strdup(tag);
    return v;
  }
  if (!implicit && !tag)
    return v;
  char *digits = g_strdup(s);
  size_t at = 0;
  for (size_t i = 0; i < n; i++)
    if (s[i] != '_')
      digits[at++] = s[i];
  digits[at] = 0;
  bool integer =
      (tag && !strcmp(tag, YAML_INT_TAG)) ||
      (!tag && g_regex_match_simple(
                   "^([-+]?0b[01_]+|[-+]?0[0-7_]+|[-+]?(0|[1-9][0-9_]*)|[-+]?"
                   "0x[0-9a-fA-F_]+|[-+]?[1-9][0-9_]*(:[0-5]?[0-9])+)$",
                   s, 0, 0));
  bool floating =
      (tag && !strcmp(tag, YAML_FLOAT_TAG)) ||
      (!tag && g_regex_match_simple(
                   "^([-+]?[0-9][0-9_]*\\.[0-9_]*([eE][-+][0-9]+)?|\\.[0-9][0-"
                   "9_]*([eE][-+][0-9]+)?|[-+]?[0-9][0-9_]*(:[0-5]?[0-9])+\\.["
                   "0-9_]*|[-+]?\\.(inf|Inf|INF)|\\.(nan|NaN|NAN))$",
                   s, 0, 0));
  bool numeric = false;
  const char *p = digits;
  if (*p == '+' || *p == '-')
    p++;
  if ((integer || floating) && strchr(p, ':')) {
    char **pieces = g_strsplit(p, ":", -1);
    double sum = 0;
    bool bounded = true;
    for (size_t i = 0; pieces[i]; i++) {
      sum = sum * 60 + g_ascii_strtod(pieces[i], NULL);
      if (sum > 9007199254740991.0)
        bounded = false;
    }
    if (*digits == '-')
      sum = -sum;
    if (bounded) {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_dtostr(buf, sizeof buf, sum);
      g_free(v->text);
      v->text = floating && !strpbrk(buf, ".eE") ? g_strconcat(buf, ".0", NULL)
                                                 : g_strdup(buf);
      v->length = strlen(v->text);
    }
    g_strfreev(pieces);
    numeric = true;
  }
  if (!numeric && integer && *p && g_ascii_isdigit(*p)) {
    char *end;
    int base = g_str_has_prefix(p, "0x") ? 16
               : g_str_has_prefix(p, "0b")
                   ? 2
                   : (*p == '0' && p[1] && p[1] != '.' ? 8 : 10);
    if (base == 16 || base == 2) {
      const char *start = p + 2;
      errno = 0;
      guint64 x = g_ascii_strtoull(start, &end, base);
      if (*start && !*end) {
        if (errno != ERANGE) {
          g_free(v->text);
          v->text = g_strdup_printf("%s%" G_GUINT64_FORMAT,
                                    *digits == '-' ? "-" : "", x);
          v->length = strlen(v->text);
        }
        numeric = true;
      }
    } else {
      errno = 0;
      gint64 x = g_ascii_strtoll(digits, &end, base);
      if (end != digits && !*end) {
        if (errno != ERANGE) {
          g_free(v->text);
          v->text = g_strdup_printf("%" G_GINT64_FORMAT, x);
          v->length = strlen(v->text);
        }
        numeric = true;
      }
    }
  }
  if (!numeric && floating) {
    if (word(p, ".inf") || word(p, ".nan")) {
      g_free(v->text);
      v->text = g_strdup(word(p, ".nan")  ? ".nan"
                         : *digits == '-' ? "-.inf"
                                          : ".inf");
      v->length = strlen(v->text);
      numeric = true;
    }
  }
  if (!numeric && floating) {
    char *end;
    double d = g_ascii_strtod(digits, &end);
    if (end != digits && !*end && strchr(digits, '.')) {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      g_ascii_dtostr(buf, sizeof buf, d);
      g_free(v->text);
      v->text =
          strpbrk(buf, ".eE") ? g_strdup(buf) : g_strconcat(buf, ".0", NULL);
      v->length = strlen(v->text);
      numeric = true;
    }
  }
  if (numeric && (implicit || tag))
    v->kind = NUMBER;
  else if (tag) {
    failed = true;
  } else if (implicit &&
             g_regex_match_simple(
                 "^([0-9]{4}-[0-9]{2}-[0-9]{2}|[0-9]{4}-[0-9]{1,2}-[0-9]{1,2}(["
                 "Tt]|[ \\t]+)[0-9]{1,2}:[0-9]{2}:[0-9]{2}(\\.[0-9]*)?([ "
                 "\\t]*(Z|[-+][0-9]{1,2}(:[0-9]{2})?))?)$",
                 s, 0, 0)) {
    v->kind = SPECIAL;
    v->tag = g_strdup("tag:yaml.org,2002:timestamp");
  }
  g_free(digits);
  return v;
}
typedef struct {
  yaml_parser_t parser;
  GHashTable *anchors;
} Reader;
static Value *yaml_node(Reader *r, yaml_event_t *e, unsigned depth) {
  if (depth > 1000) {
    failed = true;
    return NULL;
  }
  Value *v = NULL;
  const char *anchor = NULL;
  if (e->type == YAML_ALIAS_EVENT) {
    v = g_hash_table_lookup(r->anchors, e->data.alias.anchor);
    if (!v)
      failed = true;
    return v;
  }
  if (e->type == YAML_SCALAR_EVENT) {
    v = scalar(e);
    anchor = (const char *)e->data.scalar.anchor;
  } else if (e->type == YAML_SEQUENCE_START_EVENT ||
             e->type == YAML_MAPPING_START_EVENT) {
    bool mapping = e->type == YAML_MAPPING_START_EVENT;
    v = value(mapping ? MAPPING : SEQUENCE);
    anchor = (const char *)(mapping ? e->data.mapping_start.anchor
                                    : e->data.sequence_start.anchor);
    const char *tag = (const char *)(mapping ? e->data.mapping_start.tag
                                             : e->data.sequence_start.tag);
    if (tag && strcmp(tag, mapping ? YAML_MAP_TAG : YAML_SEQ_TAG)) {
      if (strcmp(tag, "tag:yaml.org,2002:set") &&
          strcmp(tag, "tag:yaml.org,2002:omap") &&
          strcmp(tag, "tag:yaml.org,2002:pairs"))
        failed = true;
      v->tag = g_strdup(tag);
    }
    if (anchor)
      g_hash_table_insert(r->anchors, g_strdup(anchor), v);
    GPtrArray *keys = g_ptr_array_new(), *vals = g_ptr_array_new();
    while (!failed) {
      yaml_event_t item;
      if (!yaml_parser_parse(&r->parser, &item)) {
        failed = true;
        break;
      }
      if (item.type ==
          (mapping ? YAML_MAPPING_END_EVENT : YAML_SEQUENCE_END_EVENT)) {
        yaml_event_delete(&item);
        break;
      }
      Value *a = yaml_node(r, &item, depth + 1);
      bool merge = mapping && item.type == YAML_SCALAR_EVENT &&
                   item.data.scalar.plain_implicit &&
                   !strcmp((const char *)item.data.scalar.value, "<<");
      yaml_event_delete(&item);
      if (!mapping) {
        g_ptr_array_add(v->values, a);
        continue;
      }
      if (!yaml_parser_parse(&r->parser, &item)) {
        failed = true;
        break;
      }
      Value *b = yaml_node(r, &item, depth + 1);
      yaml_event_delete(&item);
      if (merge) {
        if (map(b)) {
          for (size_t i = 0; i < b->keys->len; i++)
            pair(v, b->keys->pdata[i], b->values->pdata[i]);
        } else if (array(b)) {
          for (size_t n = b->values->len; n > 0; n--) {
            Value *o = b->values->pdata[n - 1];
            if (!map(o)) {
              failed = true;
              break;
            }
            for (size_t i = 0; i < o->keys->len; i++)
              pair(v, o->keys->pdata[i], o->values->pdata[i]);
          }
        } else
          failed = true;
      } else {
        g_ptr_array_add(keys, a);
        g_ptr_array_add(vals, b);
      }
    }
    for (size_t i = 0; i < keys->len; i++)
      pair(v, keys->pdata[i], vals->pdata[i]);
    g_ptr_array_free(keys, TRUE);
    g_ptr_array_free(vals, TRUE);
    return v;
  } else {
    failed = true;
    return NULL;
  }
  if (anchor)
    g_hash_table_insert(r->anchors, g_strdup(anchor), v);
  return v;
}
Value *parse_yaml(const char *data, size_t length) {
  Reader r = {0};
  yaml_parser_initialize(&r.parser);
  yaml_parser_set_input_string(&r.parser, (const unsigned char *)data, length);
  r.anchors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  Value *v = NULL;
  yaml_event_t e;
  unsigned documents = 0;
  while (!failed && yaml_parser_parse(&r.parser, &e)) {
    yaml_event_type_t type = e.type;
    if (type == YAML_DOCUMENT_START_EVENT) {
      if (++documents > 1)
        failed = true;
    } else if (type == YAML_SCALAR_EVENT || type == YAML_MAPPING_START_EVENT ||
               type == YAML_SEQUENCE_START_EVENT)
      v = yaml_node(&r, &e, 0);
    yaml_event_delete(&e);
    if (type == YAML_STREAM_END_EVENT)
      break;
  }
  if (r.parser.error != YAML_NO_ERROR)
    failed = true;
  g_hash_table_destroy(r.anchors);
  yaml_parser_delete(&r.parser);
  return v ? v : value(NIL);
}
static int emit_node(yaml_document_t *doc, Value *v, GHashTable *seen) {
  gpointer old = v->values ? g_hash_table_lookup(seen, v) : NULL;
  if (old)
    return GPOINTER_TO_INT(old);
  int id = 0;
  switch (v->kind) {
  case NIL:
    id = yaml_document_add_scalar(doc, (const yaml_char_t *)YAML_NULL_TAG,
                                  (const yaml_char_t *)"null", 4,
                                  YAML_PLAIN_SCALAR_STYLE);
    break;
  case BOOL:
    id = yaml_document_add_scalar(
        doc, (const yaml_char_t *)YAML_BOOL_TAG,
        (const yaml_char_t *)(v->boolean ? "true" : "false"),
        v->boolean ? 4 : 5, YAML_PLAIN_SCALAR_STYLE);
    break;
  case NUMBER:
    id = yaml_document_add_scalar(
        doc,
        (const yaml_char_t *)(strpbrk(v->text, ".eE") ? YAML_FLOAT_TAG
                                                      : YAML_INT_TAG),
        (const yaml_char_t *)v->text, (int)v->length, YAML_PLAIN_SCALAR_STYLE);
    break;
  case TEXT:
    id = yaml_document_add_scalar(doc, (const yaml_char_t *)YAML_STR_TAG,
                                  (const yaml_char_t *)v->text, (int)v->length,
                                  YAML_DOUBLE_QUOTED_SCALAR_STYLE);
    break;
  case SPECIAL:
    id = yaml_document_add_scalar(doc, (const yaml_char_t *)v->tag,
                                  (const yaml_char_t *)v->text, (int)v->length,
                                  YAML_PLAIN_SCALAR_STYLE);
    break;
  case SEQUENCE:
    id = yaml_document_add_sequence(
        doc, (const yaml_char_t *)(v->tag ? v->tag : YAML_SEQ_TAG),
        YAML_BLOCK_SEQUENCE_STYLE);
    break;
  case MAPPING:
    id = yaml_document_add_mapping(
        doc, (const yaml_char_t *)(v->tag ? v->tag : YAML_MAP_TAG),
        YAML_BLOCK_MAPPING_STYLE);
    break;
  }
  if (!id) {
    failed = true;
    return 0;
  }
  if (v->values)
    g_hash_table_insert(seen, v, GINT_TO_POINTER(id));
  if (v->values)
    for (size_t i = 0; i < v->values->len; i++) {
      int child = emit_node(doc, v->values->pdata[i], seen);
      if (v->kind == SEQUENCE)
        yaml_document_append_sequence_item(doc, id, child);
      else
        yaml_document_append_mapping_pair(
            doc, id, emit_node(doc, v->keys->pdata[i], seen), child);
    }
  return id;
}
static int yaml_output(void *data, unsigned char *buffer, size_t size) {
  g_byte_array_append(data, buffer, (guint)size);
  return 1;
}
GBytes *yaml_bytes(Value *v) {
  yaml_document_t doc;
  yaml_document_initialize(&doc, NULL, NULL, NULL, 1, 1);
  GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
  emit_node(&doc, v, seen);
  g_hash_table_destroy(seen);
  if (failed) {
    yaml_document_delete(&doc);
    return NULL;
  }
  yaml_emitter_t emitter;
  yaml_emitter_initialize(&emitter);
  GByteArray *out = g_byte_array_new();
  yaml_emitter_set_output(&emitter, yaml_output, out);
  yaml_emitter_set_unicode(&emitter, 1);
  yaml_emitter_set_indent(&emitter, 2);
  yaml_emitter_set_width(&emitter, 80);
  bool ok = yaml_emitter_dump(&emitter, &doc);
  if (ok)
    ok = yaml_emitter_close(&emitter);
  yaml_emitter_delete(&emitter);
  if (!ok) {
    failed = true;
    g_byte_array_free(out, TRUE);
    return NULL;
  }
  return g_byte_array_free_to_bytes(out);
}
