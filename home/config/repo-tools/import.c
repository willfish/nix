#include "common.h"
#include <libxml/HTMLparser.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *start_marker =
                      "    # BEGIN generated Omarchy community inputs",
                  *end_marker = "    # END generated Omarchy community inputs";
typedef struct {
  htmlParserCtxt *parser;
  GString *source, *label;
  char *slug, *url;
  GHashTable *entries;
  size_t scanned;
  bool current, caption, failed;
} Catalogue;
typedef struct {
  char *slug, *label, *url;
} Theme;
static void theme_free(void *p) {
  Theme *t = p;
  g_free(t->slug);
  g_free(t->label);
  g_free(t->url);
  g_free(t);
}
static xmlParserErrors no_resource(void *context, const char *url,
                                   const char *public_id, xmlResourceType type,
                                   xmlParserInputFlags flags,
                                   xmlParserInput **out) {
  (void)context;
  (void)url;
  (void)public_id;
  (void)type;
  (void)flags;
  *out = NULL;
  return XML_IO_ENOENT;
}
static void decoded_text(void *context, const xmlChar *text, int length) {
  g_string_append_len(context, (const char *)text, length);
}
static char *decode(const char *raw) {
  if (!strchr(raw, '&'))
    return g_strdup(raw);
  GString *input = g_string_new(NULL), *out = g_string_new(NULL);
  for (const char *p = raw; *p; p++) {
    if (*p == '<')
      g_string_append(input, "&#60;");
    else
      g_string_append_c(input, *p);
  }
  htmlSAXHandler sax = {0};
  sax.characters = decoded_text;
  sax.cdataBlock = decoded_text;
  htmlParserCtxt *p = htmlCreatePushParserCtxt(&sax, out, NULL, 0, NULL,
                                               XML_CHAR_ENCODING_UTF8);
  xmlCtxtSetResourceLoader(p, no_resource, NULL);
  htmlCtxtSetOptions(p, HTML_PARSE_HTML5 | HTML_PARSE_NONET |
                            HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING |
                            HTML_PARSE_IGNORE_ENC);
  htmlParseChunk(p, input->str, (int)input->len, 1);
  htmlFreeParserCtxt(p);
  g_string_free(input, TRUE);
  return g_string_free(out, FALSE);
}
static size_t token_start(Catalogue *c, size_t end) {
  const char *s = c->source->str;
  for (size_t i = c->scanned; i < end; i++) {
    if (s[i] != '<')
      continue;
    if (!strncmp(s + i, "<!--", 4)) {
      const char *close = strstr(s + i + 4, "-->");
      if (!close)
        return SIZE_MAX;
      i = (size_t)(close - s) + 2;
      continue;
    }
    char quote = 0;
    for (size_t j = i + 1; j < c->source->len; j++) {
      if (quote) {
        if (s[j] == quote)
          quote = 0;
      } else if (s[j] == '\'' || s[j] == '"')
        quote = s[j];
      else if (s[j] == '>') {
        if (j == end || (s[end] == '/' && j == end + 1))
          return i;
        i = j;
        break;
      }
    }
  }
  return SIZE_MAX;
}
static char *attribute(Catalogue *c, size_t begin, size_t end,
                       const char *wanted) {
  char *tag = g_strndup(c->source->str + begin, end - begin);
  GRegex *regex =
      g_regex_new("(?<=['\"\\t\\n\\r\\f /])([^\\t\\n\\r\\f />][^\\t\\n\\r\\f "
                  "/=>]*)(?:[\\t\\n\\r\\f ]*=[\\t\\n\\r\\f "
                  "]*('[^']*'|\"[^\"]*\"|(?!['\"])[^>\\t\\n\\r\\f "
                  "]*))?(?:[\\t\\n\\r\\f ]|/(?!>))*",
                  0, 0, NULL);
  GMatchInfo *matches = NULL;
  g_regex_match(regex, tag, 0, &matches);
  char *result = g_strdup("");
  while (g_match_info_matches(matches)) {
    char *name = g_match_info_fetch(matches, 1);
    if (!g_ascii_strcasecmp(name, wanted)) {
      int a, b;
      g_match_info_fetch_pos(matches, 2, &a, &b);
      g_free(result);
      if (a < 0) {
        c->failed = true;
        result = g_strdup("");
      } else {
        char *raw = g_strndup(tag + a, (size_t)(b - a));
        size_t n = strlen(raw);
        if (n >= 2 && ((raw[0] == '\'' && raw[n - 1] == '\'') ||
                       (raw[0] == '"' && raw[n - 1] == '"'))) {
          memmove(raw, raw + 1, n - 2);
          raw[n - 2] = 0;
        }
        result = decode(raw);
        g_free(raw);
      }
    }
    g_free(name);
    g_match_info_next(matches, NULL);
  }
  g_match_info_free(matches);
  g_regex_unref(regex);
  g_free(tag);
  return result;
}
static char *stem(char *src) {
  size_t n = strlen(src);
  while (n && src[n - 1] == '/')
    src[--n] = 0;
  const char *base = strrchr(src, '/');
  base = base ? base + 1 : src;
  char *out = g_strdup(base), *dot = strrchr(out, '.');
  if (dot && dot != out && dot[1])
    *dot = 0;
  g_free(src);
  return out;
}
static void end_tag(void *context, const xmlChar *name) {
  Catalogue *c = context;
  unsigned long position = 0;
  xmlCtxtGetInputPosition(c->parser, 0, NULL, NULL, NULL, &position);
  if (position > c->scanned)
    c->scanned = position;
  if (!strcmp((const char *)name, "figcaption"))
    c->caption = false;
  else if (!strcmp((const char *)name, "figure") && c->current) {
    char *label = strip(c->label ? c->label->str : "");
    bool valid =
        c->slug && c->url &&
        g_regex_match_simple("\\A[a-z0-9]+(?:-[a-z0-9]+)*\\z", c->slug, 0, 0) &&
        strlen(c->slug) <= 53 && !g_hash_table_contains(c->entries, c->slug) &&
        g_regex_match_simple(
            "\\Ahttps://github\\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\\z",
            c->url, 0, 0) &&
        *label;
    for (const unsigned char *p = (const unsigned char *)label; *p; p++)
      if (*p < 32)
        valid = false;
    if (!valid) {
      c->failed = true;
      g_free(label);
    } else {
      Theme *t = g_new0(Theme, 1);
      t->slug = g_strdup(c->slug);
      t->url = g_strdup(c->url);
      t->label = label;
      g_hash_table_insert(c->entries, t->slug, t);
    }
    c->current = false;
  }
}
static void start_tag(void *context, const xmlChar *name,
                      const xmlChar **attributes) {
  (void)attributes;
  Catalogue *c = context;
  unsigned long position = 0;
  xmlCtxtGetInputPosition(c->parser, 0, NULL, NULL, NULL, &position);
  if (position >= c->source->len) {
    c->failed = true;
    return;
  }
  size_t start = token_start(c, (size_t)position);
  if (start == SIZE_MAX) {
    c->failed = true;
    return;
  }
  bool closing = c->source->str[position] == '/' &&
                 position + 1 < c->source->len &&
                 c->source->str[position + 1] == '>';
  size_t end = position + (closing ? 1 : 0);
  c->scanned = end + 1;
  if (!strcmp((const char *)name, "figure")) {
    c->current = true;
    g_clear_pointer(&c->slug, g_free);
    g_clear_pointer(&c->url, g_free);
    if (c->label)
      g_string_free(c->label, TRUE);
    c->label = g_string_new(NULL);
  } else if (c->current) {
    if (!strcmp((const char *)name, "img")) {
      g_free(c->slug);
      c->slug = stem(attribute(c, start, end, "src"));
    } else if (!strcmp((const char *)name, "figcaption"))
      c->caption = true;
    else if (!strcmp((const char *)name, "a") && c->caption) {
      g_free(c->url);
      c->url = attribute(c, start, end, "href");
      size_t n = strlen(c->url);
      while (n && c->url[n - 1] == '/')
        c->url[--n] = 0;
    }
  }
  if (closing)
    end_tag(c, name);
}
static void content(void *context, const xmlChar *text, int length) {
  Catalogue *c = context;
  if (c->current && c->caption)
    g_string_append_len(c->label, (const char *)text, length);
}
static gint theme_compare(gconstpointer a, gconstpointer b) {
  return strcmp((*(Theme *const *)a)->slug, (*(Theme *const *)b)->slug);
}
static const char *once(const char *text, const char *marker) {
  const char *p = strstr(text, marker);
  return p && !strstr(p + strlen(marker), marker) ? p : NULL;
}
static int import(const char *root, const char *page, const char *source) {
  int status = 1;
  Catalogue c = {0};
  c.source = read_text(page);
  c.entries = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, theme_free);
  if (!c.source || c.source->len > G_MAXINT ||
      memchr(c.source->str, 0, c.source->len))
    goto done;
  htmlSAXHandler sax = {0};
  sax.startElement = start_tag;
  sax.endElement = end_tag;
  sax.characters = content;
  sax.cdataBlock = content;
  c.parser =
      htmlCreatePushParserCtxt(&sax, &c, NULL, 0, NULL, XML_CHAR_ENCODING_UTF8);
  if (!c.parser)
    goto done;
  xmlCtxtSetResourceLoader(c.parser, no_resource, NULL);
  htmlCtxtSetOptions(c.parser, HTML_PARSE_HTML5 | HTML_PARSE_NONET |
                                   HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING |
                                   HTML_PARSE_IGNORE_ENC);
  int parsed = htmlParseChunk(c.parser, c.source->str, (int)c.source->len, 1);
  if (parsed || c.failed || !g_hash_table_size(c.entries))
    goto done;
  char *flake = g_build_filename(root, "flake.nix", NULL),
       *unavailable_path = g_build_filename(
           root, "home/user/themes/community-unavailable.json", NULL),
       *target =
           g_build_filename(root, "home/user/themes/community.json", NULL);
  GString *text = read_text(flake);
  yyjson_doc *unavailable = load_json(unavailable_path);
  g_free(unavailable_path);
  if (!text || !unavailable ||
      !yyjson_is_obj(yyjson_doc_get_root(unavailable)) ||
      memchr(text->str, 0, text->len))
    goto files_done;
  const char *begin = once(text->str, start_marker),
             *end = once(text->str, end_marker);
  if (!begin || !end || end < begin)
    goto files_done;
  GPtrArray *themes = g_ptr_array_new();
  GHashTableIter iter;
  void *v;
  g_hash_table_iter_init(&iter, c.entries);
  while (g_hash_table_iter_next(&iter, NULL, &v))
    g_ptr_array_add(themes, v);
  g_ptr_array_sort(themes, theme_compare);
  GString *replacement =
      g_string_new_len(text->str, (gssize)(begin - text->str));
  g_string_append(replacement, start_marker);
  g_string_append_c(replacement, '\n');
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *out = yyjson_mut_obj(doc), *entries = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, out);
  yyjson_mut_obj_add_strcpy(doc, out, "source", source);
  yyjson_mut_obj_add_val(doc, out, "themes", entries);
  for (size_t i = 0; i < themes->len; i++) {
    Theme *t = themes->pdata[i];
    yyjson_mut_val *entry = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, entry, "label", t->label);
    yyjson_mut_obj_add_strcpy(doc, entry, "url", t->url);
    yyjson_mut_obj_add_val(doc, entries, t->slug, entry);
    if (field(yyjson_doc_get_root(unavailable), t->slug))
      continue;
    g_string_append_printf(
        replacement,
        "    omarchy-theme-%s = {\n      url = \"git+%s?shallow=1\";\n      "
        "flake = false;\n    };\n",
        t->slug, t->url);
  }
  g_string_append(replacement, end_marker);
  g_string_append(replacement, end + strlen(end_marker));
  size_t length;
  char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &length);
  if (json) {
    GString *serialized = g_string_new_len(json, (gssize)length);
    g_string_append_c(serialized, '\n');
    if (write_text(flake, replacement->str, replacement->len) &&
        write_text(target, serialized->str, serialized->len)) {
      printf("Read %u community themes; run nix flake lock\n", themes->len);
      Val *key, *reason;
      size_t i, n;
      yyjson_obj_foreach(yyjson_doc_get_root(unavailable), i, n, key, reason) {
        char *value = yyjson_is_str(reason) ? g_strdup(yyjson_get_str(reason))
                                            : yyjson_val_write(reason, 0, NULL);
        printf("Unavailable: %s: %s\n", yyjson_get_str(key),
               value ? value : "");
        g_free(value);
      }
      status = 0;
    }
    g_string_free(serialized, TRUE);
    free(json);
  }
  yyjson_mut_doc_free(doc);
  g_string_free(replacement, TRUE);
  g_ptr_array_free(themes, TRUE);
files_done:
  if (text)
    g_string_free(text, TRUE);
  if (unavailable)
    yyjson_doc_free(unavailable);
  g_free(flake);
  g_free(target);
done:
  if (c.parser)
    htmlFreeParserCtxt(c.parser);
  if (c.source)
    g_string_free(c.source, TRUE);
  if (c.label)
    g_string_free(c.label, TRUE);
  g_free(c.slug);
  g_free(c.url);
  g_hash_table_destroy(c.entries);
  if (status)
    fputs("Community import failed; check the page, catalogue and generated "
          "block.\n",
          stderr);
  return status;
}
int main(int argc, char **argv) {
  const char *root = ".", *page = NULL, *source = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc)
      root = argv[++i];
    else if (!strcmp(argv[i], "--help")) {
      puts("usage: import-omarchy-community [--root CHECKOUT] PAGE.html "
           "SOURCE_URL");
      return 0;
    } else if (!page)
      page = argv[i];
    else if (!source)
      source = argv[i];
    else {
      fputs("usage: import-omarchy-community [--root CHECKOUT] PAGE.html "
            "SOURCE_URL\n",
            stderr);
      return 2;
    }
  }
  if (!page || !source) {
    fputs("usage: import-omarchy-community [--root CHECKOUT] PAGE.html "
          "SOURCE_URL\n",
          stderr);
    return 2;
  }
  return import(root, page, source);
}
