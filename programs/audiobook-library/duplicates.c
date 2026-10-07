#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "audio.h"
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct {
  char *name, *path, *normal, *lower;
  GPtrArray *asins;
  bool size_known;
  guint64 size;
} Entry;
typedef struct {
  Entry *source, *target;
  const char *kind;
  char *detail;
} Match;
static void entry_free(gpointer p) {
  Entry *e = p;
  g_free(e->name);
  g_free(e->path);
  g_free(e->normal);
  g_free(e->lower);
  g_ptr_array_free(e->asins, TRUE);
  g_free(e);
}
static void match_free(gpointer p) {
  Match *m = p;
  g_free(m->detail);
  g_free(m);
}
static gint order(gconstpointer a, gconstpointer b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static void asins(GPtrArray *out, const char *name) {
  if (!name)
    return;
  char *valid = g_utf8_make_valid(name, -1);
  GRegex *re =
      g_regex_new("(*UCP)\\b(B0[A-Z0-9]{8})\\b", G_REGEX_CASELESS, 0, NULL);
  GMatchInfo *match = NULL;
  g_regex_match(re, valid, 0, &match);
  while (g_match_info_matches(match)) {
    char *s = g_match_info_fetch(match, 1), *upper = g_utf8_strup(s, -1);
    g_free(s);
    bool found = false;
    for (size_t i = 0; i < out->len; i++)
      if (!strcmp(out->pdata[i], upper))
        found = true;
    if (!found)
      g_ptr_array_add(out, upper);
    else
      g_free(upper);
    g_match_info_next(match, NULL);
  }
  g_match_info_free(match);
  g_regex_unref(re);
  g_free(valid);
  g_ptr_array_sort(out, order);
}
static char *substitute(const char *pattern, const char *value,
                        GRegexCompileFlags flags) {
  GRegex *re = g_regex_new(pattern, flags, 0, NULL);
  char *out = g_regex_replace_literal(re, value, -1, 0, " ", 0, NULL);
  g_regex_unref(re);
  return out;
}
static char *normalize(const char *name) {
  char *valid = g_utf8_make_valid(name, -1),
       *a = substitute("\\[(B0[A-Z0-9]{8})\\]", valid, G_REGEX_CASELESS);
  g_free(valid);
  char *b =
      substitute("(*UCP)\\(unabridged\\)|\\(abridged\\)|\\bunabridged\\b|"
                 "\\babridged\\b|\\bcomplete\\b|\\bwebrip\\b|\\bx264\\b|"
                 "\\b720p\\b|\\b1080p\\b|\\bnf\\b|\\bgalaxytv\\b|\\btgx\\b",
                 a, G_REGEX_CASELESS);
  g_free(a);
  a = g_utf8_strdown(b, -1);
  g_free(b);
  b = strip(a);
  g_free(a);
  if (a_suffix(b, true, true)) {
    a = a_normal(b);
    g_free(b);
    b = a;
    char *dot = strrchr(b, '.');
    if (dot)
      *dot = 0;
  }
  a = substitute("[^a-z0-9]+", b, 0);
  g_free(b);
  g_strstrip(a);
  return a;
}
static Entry *entry(const char *name, const char *path, bool known,
                    guint64 size) {
  Entry *e = g_new0(Entry, 1);
  e->name = g_strdup(name);
  e->path = g_strdup(path);
  e->size_known = known;
  e->size = size;
  e->normal = normalize(name);
  e->lower = a_lower(name);
  e->asins = g_ptr_array_new_with_free_func(g_free);
  asins(e->asins, name);
  asins(e->asins, path);
  return e;
}
static bool walk(const char *root, guint64 limit, guint64 depth,
                 GPtrArray *entries, GHashTable *seen, char **failure) {
  int error;
  GPtrArray *names = a_names(root, true, &error);
  if (error) {
    *failure = a_error(root, error);
    g_ptr_array_free(names, TRUE);
    return false;
  }
  for (size_t i = 0; i < names->len; i++) {
    char *path = a_join(root, names->pdata[i]);
    struct stat st, link;
    if (stat(path, &st) != 0) {
      *failure = a_error(path, errno);
      g_free(path);
      g_ptr_array_free(names, TRUE);
      return false;
    }
    bool file = S_ISREG(st.st_mode), directory = S_ISDIR(st.st_mode);
    if (directory || (file && a_suffix(path, false, false))) {
      char *key = a_resolve(path, true);
      if (!key) {
        *failure = a_error(path, errno);
        g_free(path);
        g_ptr_array_free(names, TRUE);
        return false;
      }
      if (!g_hash_table_contains(seen, key)) {
        g_hash_table_add(seen, g_strdup(key));
        g_ptr_array_add(entries,
                        entry(names->pdata[i], key, file, (guint64)st.st_size));
        Entry *e = entries->pdata[entries->len - 1];
        g_ptr_array_set_size(e->asins, 0);
        asins(e->asins, e->name);
        asins(e->asins, path);
      }
      g_free(key);
    }
    bool recurse = false;
    if (depth < limit && directory) {
      if (lstat(path, &link) != 0) {
        *failure = a_error(path, errno);
        g_free(path);
        g_ptr_array_free(names, TRUE);
        return false;
      }
      recurse = !S_ISLNK(link.st_mode);
    }
    if (recurse && !walk(path, limit, depth + 1, entries, seen, failure)) {
      g_free(path);
      g_ptr_array_free(names, TRUE);
      return false;
    }
    g_free(path);
  }
  g_ptr_array_free(names, TRUE);
  return true;
}
static GPtrArray *lines(const char *path) {
  GString *text;
  if (strcmp(path, "-"))
    text = read_text(path);
  else {
    text = g_string_new(NULL);
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, stdin)))
      g_string_append_len(text, buffer, (gssize)n);
    if (ferror(stdin) || !g_utf8_validate(text->str, (gssize)text->len, NULL)) {
      g_string_free(text, TRUE);
      return NULL;
    }
  }
  if (!text)
    return NULL;
  if (strlen(text->str) != text->len) {
    g_string_free(text, TRUE);
    return NULL;
  }
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  const char *start = text->str;
  for (const char *p = start; *p;) {
    gunichar c = g_utf8_get_char(p);
    const char *next = g_utf8_next_char(p);
    if (c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
        (c >= 0x1c && c <= 0x1e) || c == 0x85 || c == 0x2028 || c == 0x2029) {
      g_ptr_array_add(out, g_strndup(start, (size_t)(p - start)));
      if (c == '\r' && *next == '\n')
        next++;
      start = next;
    }
    p = next;
  }
  if (*start)
    g_ptr_array_add(out, g_strdup(start));
  g_string_free(text, TRUE);
  return out;
}
static GPtrArray *sources(GPtrArray *names, const char *root) {
  GHashTable *paths =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  struct stat st;
  if (root && stat(root, &st) == 0 && S_ISDIR(st.st_mode))
    for (size_t i = 0; i < names->len; i++) {
      char *name = strip(names->pdata[i]);
      if (*name) {
        char *candidate = a_join(root, name);
        if (stat(candidate, &st) == 0)
          g_hash_table_replace(paths, g_strdup(name), candidate);
        else
          g_free(candidate);
      }
      g_free(name);
    }
  GPtrArray *out = g_ptr_array_new_with_free_func(entry_free);
  for (size_t i = 0; i < names->len; i++) {
    char *name = strip(names->pdata[i]);
    if (!*name || *name == '#') {
      g_free(name);
      continue;
    }
    const char *path = g_hash_table_lookup(paths, name);
    if (!path) {
      char *normal = a_normal(name), *base = g_path_get_basename(normal);
      path = g_hash_table_lookup(paths, base);
      g_free(base);
      g_free(normal);
    }
    guint64 size = 0;
    bool known = false, audio = false;
    if (path && stat(path, &st) == 0) {
      if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))
        known = a_size_tree(path, &size, &audio);
    } else
      path = NULL;
    g_ptr_array_add(out, entry(name, path, known, size));
    g_free(name);
  }
  g_hash_table_destroy(paths);
  return out;
}
static void index_add(GHashTable *index, const char *key, Entry *e) {
  GPtrArray *list = g_hash_table_lookup(index, key);
  if (!list) {
    list = g_ptr_array_new();
    g_hash_table_insert(index, g_strdup(key), list);
  }
  g_ptr_array_add(list, e);
}
static void list_free(gpointer p) { g_ptr_array_free(p, TRUE); }
static void add(GPtrArray *matches, GHashTable *seen, Entry *s, Entry *t,
                const char *kind, char *detail) {
  if (g_hash_table_contains(seen, t->path)) {
    g_free(detail);
    return;
  }
  g_hash_table_add(seen, t->path);
  Match *m = g_new0(Match, 1);
  *m = (Match){s, t, kind, detail};
  g_ptr_array_add(matches, m);
}
static GPtrArray *find_matches(GPtrArray *sources_, GPtrArray *targets) {
  GHashTable *exact = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            list_free),
             *norm = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           list_free),
             *asin = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           list_free);
  for (size_t i = 0; i < targets->len; i++) {
    Entry *t = targets->pdata[i];
    index_add(exact, t->lower, t);
    if (*t->normal)
      index_add(norm, t->normal, t);
    for (size_t j = 0; j < t->asins->len; j++)
      index_add(asin, t->asins->pdata[j], t);
  }
  GPtrArray *out = g_ptr_array_new_with_free_func(match_free);
  for (size_t i = 0; i < sources_->len; i++) {
    Entry *s = sources_->pdata[i];
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *list = g_hash_table_lookup(exact, s->lower);
    for (size_t j = 0; list && j < list->len; j++) {
      Entry *t = list->pdata[j];
      char *quoted = a_repr(t->name);
      add(out, seen, s, t, "exact_name",
          g_strconcat("basename equals ", quoted, NULL));
      g_free(quoted);
    }
    list = *s->normal ? g_hash_table_lookup(norm, s->normal) : NULL;
    for (size_t j = 0; list && j < list->len; j++) {
      Entry *t = list->pdata[j];
      char *a = a_repr(s->normal), *b = a_repr(t->normal);
      add(out, seen, s, t, "normalized",
          g_strdup_printf("normalized %s == %s", a, b));
      g_free(a);
      g_free(b);
    }
    for (size_t k = 0; k < s->asins->len; k++) {
      list = g_hash_table_lookup(asin, s->asins->pdata[k]);
      for (size_t j = 0; list && j < list->len; j++)
        add(out, seen, s, list->pdata[j], "asin",
            g_strconcat("ASIN ", s->asins->pdata[k], NULL));
    }
    if (s->size_known && s->size > 0)
      for (size_t j = 0; j < targets->len; j++) {
        Entry *t = targets->pdata[j];
        if (t->size_known && s->size == t->size)
          add(out, seen, s, t, "size",
              g_strdup_printf("same size_bytes=%" G_GUINT64_FORMAT, s->size));
      }
    g_hash_table_destroy(seen);
  }
  g_hash_table_destroy(exact);
  g_hash_table_destroy(norm);
  g_hash_table_destroy(asin);
  return out;
}
static Json *report(GPtrArray *source, GPtrArray *matches, Json *errors,
                    const char *depth) {
  Json *out = json_node(J_OBJECT), *rows = json_node(J_ARRAY),
       *sources_ = json_node(J_ARRAY);
  a_put(out, "status", a_text(errors->values->len ? "incomplete" : "complete"));
  a_put(out, "scan_errors", json_ref(errors));
  a_put(out, "max_depth", a_int(depth));
  for (size_t i = 0; i < source->len; i++) {
    Entry *s = source->pdata[i];
    Json *row = json_node(J_OBJECT);
    a_put(row, "name", a_text(s->name));
    a_put(row, "path", s->path ? a_text(s->path) : json_node(J_NULL));
    a_put(row, "size_bytes",
          s->size_known ? a_size(s->size) : json_node(J_NULL));
    a_add(sources_, row);
  }
  a_put(out, "sources", sources_);
  a_put(out, "match_count",
        errors->values->len ? json_node(J_NULL) : a_size(matches->len));
  for (size_t i = 0; i < matches->len; i++) {
    Match *m = matches->pdata[i];
    Json *row = json_node(J_OBJECT);
    a_put(row, "source", a_text(m->source->name));
    a_put(row, "target_path", a_text(m->target->path));
    a_put(row, "match_type", a_text(m->kind));
    a_put(row, "detail", a_text(m->detail));
    a_add(rows, row);
  }
  a_put(out, "matches", rows);
  return out;
}
static GPtrArray *hits(GPtrArray *matches, const char *name) {
  GPtrArray *out = g_ptr_array_new();
  for (size_t i = 0; i < matches->len; i++) {
    Match *m = matches->pdata[i];
    if (!strcmp(m->source->name, name))
      g_ptr_array_add(out, m);
  }
  return out;
}
static GString *markdown(GPtrArray *source, GPtrArray *matches, Json *errors) {
  if (errors->values->len) {
    GString *out = g_string_new(
        "# Source to target duplicate preflight incomplete\n\nNo staging "
        "decision is available until every target is scanned.\n\n");
    for (size_t i = 0; i < errors->values->len; i++) {
      Json *e = errors->values->pdata[i];
      g_string_append_printf(out, "- `%s`: %s\n",
                             json_get(e, "root", 4)->string->str,
                             json_get(e, "error", 5)->string->str);
    }
    return out;
  }
  const char *dash = "\342\200\224";
  GHashTable *hit_names = g_hash_table_new(g_str_hash, g_str_equal);
  for (size_t i = 0; i < matches->len; i++) {
    Match *m = matches->pdata[i];
    g_hash_table_add(hit_names, m->source->name);
  }
  GString *out =
      g_string_new("# Source \342\206\222 target duplicate preflight\n\n");
  g_string_append_printf(
      out,
      "Sources checked: **%u**\nSources with at least one hit: **%u**\nTotal "
      "match rows: **%u**\n\n| Source | Hits | Match types | Example target "
      "|\n|---|---|---|---|\n",
      source->len, g_hash_table_size(hit_names), matches->len);
  g_hash_table_destroy(hit_names);
  for (size_t i = 0; i < source->len; i++) {
    Entry *s = source->pdata[i];
    GPtrArray *found = hits(matches, s->name);
    if (!found->len)
      g_string_append_printf(out, "| %s | 0 | %s | %s |\n", s->name, dash,
                             dash);
    else {
      GString *types = g_string_new(NULL);
      const char *kinds[] = {"asin", "exact_name", "normalized", "size"};
      for (size_t k = 0; k < 4; k++) {
        bool any = false;
        for (size_t j = 0; j < found->len; j++)
          if (!strcmp(((Match *)found->pdata[j])->kind, kinds[k]))
            any = true;
        if (any) {
          if (types->len)
            g_string_append_c(types, ',');
          g_string_append(types, kinds[k]);
        }
      }
      g_string_append_printf(out, "| %s | %u | %s | `%s` |\n", s->name,
                             found->len, types->str,
                             ((Match *)found->pdata[0])->target->path);
      g_string_free(types, TRUE);
    }
    g_ptr_array_free(found, TRUE);
  }
  g_string_append(out, "\n## Match detail\n");
  bool clean = false;
  for (size_t i = 0; i < source->len; i++) {
    Entry *s = source->pdata[i];
    GPtrArray *found = hits(matches, s->name);
    if (found->len) {
      g_string_append_printf(out, "\n### %s\n", s->name);
      for (size_t j = 0; j < found->len; j++) {
        Match *m = found->pdata[j];
        g_string_append_printf(out, "- **%s**: `%s` %s %s\n", m->kind,
                               m->target->path, dash, m->detail);
      }
    } else
      clean = true;
    g_ptr_array_free(found, TRUE);
  }
  if (clean) {
    g_string_append(out,
                    "\n## No hit (eligible to stage pending full AC5)\n\n");
    for (size_t i = 0; i < source->len; i++) {
      Entry *s = source->pdata[i];
      GPtrArray *found = hits(matches, s->name);
      if (!found->len)
        g_string_append_printf(out, "- %s\n", s->name);
      g_ptr_array_free(found, TRUE);
    }
  }
  return out;
}
int main(int argc, char **argv) {
  const struct option options[] = {
      {"sources-file", required_argument, NULL, 's'},
      {"targets", required_argument, NULL, 't'},
      {"source-root", required_argument, NULL, 'r'},
      {"max-depth", required_argument, NULL, 'd'},
      {"format", required_argument, NULL, 'f'},
      {"output", required_argument, NULL, 'o'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  const char *file = NULL, *root = NULL, *format = "markdown", *output = NULL;
  char *depth = g_strdup("3");
  GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
  int opt, status = 2;
  opterr = 0;
  while ((opt = getopt_long(argc, argv, "ho:", options, NULL)) != -1) {
    switch (opt) {
    case 's':
      file = optarg;
      break;
    case 't':
      g_ptr_array_set_size(roots, 0);
      g_ptr_array_add(roots, a_normal(optarg));
      while (optind < argc && argv[optind][0] != '-')
        g_ptr_array_add(roots, a_normal(argv[optind++]));
      break;
    case 'r':
      root = optarg;
      break;
    case 'd': {
      char *ascii = decimal_text(optarg);
      g_free(depth);
      depth = a_integer(ascii);
      g_free(ascii);
      if (!depth)
        goto done;
      break;
    }
    case 'f':
      format = optarg;
      break;
    case 'o':
      output = optarg;
      break;
    case 'h':
      puts("usage: source-target-duplicate-check --sources-file FILE --targets "
           "DIR [DIR ...]\n       [--source-root DIR] [--max-depth 3] "
           "[--format markdown|json] [-o FILE]");
      status = 0;
      goto done;
    default:
      goto done;
    }
  }
  if (optind != argc || !file || !roots->len || *depth == '-' ||
      !strcmp(depth, "0") ||
      (strcmp(format, "markdown") && strcmp(format, "json")))
    goto done;
  GPtrArray *names = lines(file);
  if (!names) {
    status = 1;
    goto done;
  }
  GPtrArray *source = sources(names, root),
            *targets = g_ptr_array_new_with_free_func(entry_free);
  g_ptr_array_free(names, TRUE);
  Json *errors = json_node(J_ARRAY);
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  guint64 limit = g_ascii_strtoull(depth, NULL, 10);
  for (size_t i = 0; i < roots->len; i++) {
    char *expanded = a_expand(roots->pdata[i]), *failure = NULL;
    struct stat st;
    if (!expanded) {
      failure = g_strdup("Could not resolve home directory");
      expanded = g_strdup(roots->pdata[i]);
    } else if (stat(expanded, &st) != 0)
      failure = a_error(expanded, errno);
    else if (!S_ISDIR(st.st_mode))
      failure = g_strconcat("Not a library directory: ", expanded, NULL);
    else
      walk(expanded, limit, 1, targets, seen, &failure);
    if (failure) {
      Json *error = json_node(J_OBJECT);
      a_put(error, "root", a_text(expanded));
      a_put(error, "error", a_text(failure));
      a_add(errors, error);
    }
    g_free(expanded);
    g_free(failure);
  }
  g_hash_table_destroy(seen);
  GPtrArray *matches = errors->values->len
                           ? g_ptr_array_new_with_free_func(match_free)
                           : find_matches(source, targets);
  GString *payload;
  if (!strcmp(format, "json")) {
    Json *data = report(source, matches, errors, depth);
    payload = a_json(data);
    json_free(data);
  } else
    payload = markdown(source, matches, errors);
  status = a_output(output, payload, false) ? (errors->values->len ? 1 : 0) : 1;
  if (payload)
    g_string_free(payload, TRUE);
  json_free(errors);
  g_ptr_array_free(matches, TRUE);
  g_ptr_array_free(targets, TRUE);
  g_ptr_array_free(source, TRUE);
done:
  g_free(depth);
  g_ptr_array_free(roots, TRUE);
  if (status == 2)
    fputs("Invalid duplicate-check arguments; max-depth must be at least 1.\n",
          stderr);
  return status;
}
