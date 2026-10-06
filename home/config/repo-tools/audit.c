#define _XOPEN_SOURCE 700
#include "common.h"
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  char *name, *kind, *path, *description;
  GPtrArray *lines;
} Skill;
static bool io_failed;
static const char *types[] = {"user-invoked router/orchestrator",
                              "model-invoked discipline", "process guardrail",
                              "reference-backed domain skill"};
static const char *stopwords =
    " a about agent agents all and any as before by code creating for from in "
    "including into only or repo skill skills that the this to use when will "
    "with workflow work working writing ";
static void add(GPtrArray *list, const char *format, ...) {
  va_list args;
  va_start(args, format);
  g_ptr_array_add(list, g_strdup_vprintf(format, args));
  va_end(args);
}
static gint compare(gconstpointer a, gconstpointer b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static GPtrArray *array(void) { return g_ptr_array_new_with_free_func(g_free); }
static GHashTable *set(void) {
  return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}
static void put(GHashTable *s, const char *key) {
  g_hash_table_add(s, g_strdup(key));
}
static GPtrArray *keys(GHashTable *s) {
  GPtrArray *out = array();
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, s);
  while (g_hash_table_iter_next(&it, &key, NULL))
    g_ptr_array_add(out, g_strdup(key));
  g_ptr_array_sort(out, compare);
  return out;
}
static const char *string(Val *v) {
  return yyjson_is_str(v) && strlen(yyjson_get_str(v)) == yyjson_get_len(v)
             ? yyjson_get_str(v)
             : NULL;
}
static bool regular(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
static bool exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}
static GPtrArray *lines(const char *path) {
  GPtrArray *out = array();
  GString *s = read_text(path);
  if (!s) {
    io_failed = true;
    return out;
  }
  const char *start = s->str;
  for (const char *p = start; *p;) {
    gunichar c = g_utf8_get_char(p);
    const char *next = g_utf8_next_char(p);
    if (c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
        (c >= 0x1c && c <= 0x1e) || c == 0x85 || c == 0x2028 || c == 0x2029) {
      g_ptr_array_add(out, g_strndup(start, (gsize)(p - start)));
      if (c == '\r' && *next == '\n')
        next++;
      start = next;
    }
    p = next;
  }
  if (*start)
    g_ptr_array_add(out, g_strdup(start));
  g_string_free(s, TRUE);
  return out;
}
static char *frontmatter(GPtrArray *text, const char *key) {
  if (!text->len || strcmp(text->pdata[0], "---"))
    return NULL;
  char *prefix = g_strconcat(key, ":", NULL), *value = NULL;
  for (size_t i = 1; i < text->len; i++) {
    const char *line = text->pdata[i];
    if (!strcmp(line, "---"))
      break;
    if (g_str_has_prefix(line, prefix)) {
      value = strip(line + strlen(prefix));
      break;
    }
  }
  g_free(prefix);
  return value;
}
static bool manual(GPtrArray *text) {
  char *s = frontmatter(text, "disable-model-invocation");
  bool value = s && (!g_ascii_strcasecmp(s, "true") ||
                     !g_ascii_strcasecmp(s, "yes") || !strcmp(s, "1"));
  g_free(s);
  return value;
}
static void part(GString *out, char *s) {
  if (*s) {
    if (out->len)
      g_string_append_c(out, ' ');
    g_string_append(out, s);
  }
  g_free(s);
}
static char *description(GPtrArray *text) {
  GString *out = g_string_new(NULL);
  bool collecting = false;
  if (text->len && !strcmp(text->pdata[0], "---"))
    for (size_t i = 1; i < text->len; i++) {
      const char *line = text->pdata[i];
      if (!strcmp(line, "---"))
        break;
      if (collecting) {
        if (*line == ' ' || *line == '\t') {
          part(out, strip(line));
          continue;
        }
        collecting = false;
      }
      if (g_str_has_prefix(line, "description:")) {
        char *value = strip(line + 12);
        if (!strcmp(value, ">") || !strcmp(value, "|") ||
            !strcmp(value, ">-") || !strcmp(value, "|-"))
          collecting = true;
        else if (*value) {
          const char *a = value, *b = value + strlen(value);
          while (a < b && *a == '"')
            a++;
          while (b > a && b[-1] == '"')
            b--;
          part(out, g_strndup(a, (gsize)(b - a)));
        }
        g_free(value);
      }
    }
  char *s = strip(out->str);
  g_string_free(out, TRUE);
  return s;
}
static GPtrArray *children(const char *path) {
  GPtrArray *out = array();
  GDir *dir = g_dir_open(path, 0, NULL);
  if (!dir)
    return out;
  const char *name;
  while ((name = g_dir_read_name(dir)))
    g_ptr_array_add(out, g_strdup(name));
  g_dir_close(dir);
  g_ptr_array_sort(out, compare);
  return out;
}
static void skill_free(gpointer p) {
  Skill *s = p;
  g_free(s->name);
  g_free(s->kind);
  g_free(s->path);
  g_free(s->description);
  g_ptr_array_free(s->lines, TRUE);
  g_free(s);
}
static GPtrArray *skills(const char *llm) {
  GPtrArray *out = g_ptr_array_new_with_free_func(skill_free);
  const char *bases[] = {"skills", "process-skills"};
  for (size_t k = 0; k < 2; k++) {
    char *base = g_build_filename(llm, bases[k], NULL);
    GPtrArray *names = children(base);
    for (size_t i = 0; i < names->len; i++) {
      char *path = g_build_filename(base, names->pdata[i], "SKILL.md", NULL);
      if (!exists(path)) {
        g_free(path);
        continue;
      }
      Skill *s = g_new0(Skill, 1);
      s->name = g_strdup(names->pdata[i]);
      s->kind = g_strdup(k ? "process" : "shared");
      s->path = path;
      s->lines = lines(path);
      s->description = description(s->lines);
      g_ptr_array_add(out, s);
    }
    g_ptr_array_free(names, TRUE);
    g_free(base);
  }
  return out;
}
static char *safe_path(const char *root, const char *s, char **error) {
  bool safe = s && *s && *s != '/' && !strchr(s, '\\');
  char **parts = s ? g_strsplit(s, "/", -1) : NULL;
  for (size_t i = 0; parts && parts[i]; i++)
    if (!*parts[i] || !strcmp(parts[i], ".") || !strcmp(parts[i], ".."))
      safe = false;
  g_strfreev(parts);
  if (!safe) {
    *error = g_strdup("unsafe catalogue path");
    return NULL;
  }
  char *path = g_build_filename(root, s, NULL),
       *resolved = realpath(path, NULL);
  if (!resolved || !regular(path)) {
    *error = g_strdup_printf("missing catalogue source: %s", s);
    g_free(path);
    free(resolved);
    return NULL;
  }
  char *base = realpath(root, NULL);
  size_t n = base ? strlen(base) : 0;
  if (!base || strncmp(base, resolved, n) ||
      (resolved[n] && resolved[n] != '/')) {
    *error = g_strdup_printf("catalogue path escapes source root: %s", s);
    g_free(path);
    path = NULL;
  }
  free(base);
  free(resolved);
  return path;
}
static bool validate(Val *catalog, const char *llm, char **error) {
  if (!yyjson_is_arr(catalog) || !yyjson_arr_size(catalog)) {
    *error = g_strdup("catalogue must be a nonempty array");
    return false;
  }
  GHashTable *names = set();
  Val *entry;
  size_t i, n;
  bool ok = false;
  yyjson_arr_foreach(catalog, i, n, entry) {
    const char *fields[] = {"name", "kind", "invocation", "hermes",
                            "references"};
    if (!yyjson_is_obj(entry) || yyjson_obj_size(entry) != 5) {
      *error = g_strdup("catalogue entry has missing or unknown fields");
      goto done;
    }
    for (size_t j = 0; j < 5; j++)
      if (!field(entry, fields[j])) {
        *error = g_strdup("catalogue entry has missing or unknown fields");
        goto done;
      }
    const char *name = string(field(entry, "name")),
               *kind = string(field(entry, "kind")),
               *invocation = string(field(entry, "invocation"));
    if (!name || strlen(name) < 2 || strlen(name) > 64 ||
        !g_regex_match_simple("\\A[a-z0-9]+(?:-[a-z0-9]+)*\\z", name, 0, 0)) {
      *error = g_strdup("invalid skill name");
      goto done;
    }
    if (g_hash_table_contains(names, name)) {
      *error = g_strdup_printf("duplicate skill: %s", name);
      goto done;
    }
    put(names, name);
    bool valid_type = false;
    for (size_t j = 0; j < 4; j++)
      if (invocation && !strcmp(invocation, types[j]))
        valid_type = true;
    bool shared = kind && !strcmp(kind, "shared"),
         process = kind && !strcmp(kind, "process");
    if ((!shared && !process) || !valid_type) {
      *error = g_strdup_printf("%s: invalid kind or invocation", name);
      goto done;
    }
    Val *hermes = field(entry, "hermes"), *refs = field(entry, "references");
    if (!yyjson_is_bool(hermes) || (yyjson_get_bool(hermes) && !shared)) {
      *error = g_strdup_printf("%s: invalid Hermes deployment", name);
      goto done;
    }
    char *relative = g_strdup_printf(
        "%s/%s/SKILL.md", shared ? "skills" : "process-skills", name);
    char *skill = safe_path(llm, relative, error);
    g_free(relative);
    if (!skill)
      goto done;
    char *skill_dir = g_path_get_dirname(skill);
    g_free(skill);
    if (!yyjson_is_obj(refs) || (process && yyjson_obj_size(refs))) {
      *error = g_strdup_printf("%s: invalid reference map", name);
      g_free(skill_dir);
      goto done;
    }
    Val *key, *source;
    size_t j, m;
    yyjson_obj_foreach(refs, j, m, key, source) {
      const char *target = string(key);
      if (!target ||
          !g_regex_match_simple("\\A[A-Za-z0-9_-][A-Za-z0-9_.-]*\\.md\\z",
                                target, 0, 0)) {
        *error = g_strdup_printf("%s: unsafe reference target", name);
        g_free(skill_dir);
        goto done;
      }
      char *path = safe_path(llm, string(source), error);
      if (!path) {
        g_free(skill_dir);
        goto done;
      }
      g_free(path);
      char *local = g_build_filename(skill_dir, "references", target, NULL);
      bool collision = exists(local);
      g_free(local);
      if (collision) {
        *error = g_strdup_printf(
            "%s: mapped reference duplicates local reference: %s", name,
            target);
        g_free(skill_dir);
        goto done;
      }
    }
    g_free(skill_dir);
  }
  ok = true;
done:
  g_hash_table_destroy(names);
  return ok;
}
static Val *catalog_entry(Val *catalog, const char *name) {
  Val *entry;
  size_t i, n;
  yyjson_arr_foreach(catalog, i, n, entry) if (text_is(field(entry, "name"),
                                                       name)) return entry;
  return NULL;
}
static GHashTable *matches(const char *pattern, const char *text) {
  GHashTable *out = set();
  GRegex *re = g_regex_new(pattern, 0, 0, NULL);
  GMatchInfo *info = NULL;
  g_regex_match(re, text, 0, &info);
  while (g_match_info_matches(info)) {
    char *s = g_match_info_fetch(info, 1);
    g_hash_table_add(out, s);
    g_match_info_next(info, NULL);
  }
  g_match_info_free(info);
  g_regex_unref(re);
  return out;
}
static char *joined(GPtrArray *text, size_t limit) {
  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < MIN(limit, text->len); i++) {
    if (i)
      g_string_append_c(s, '\n');
    g_string_append(s, text->pdata[i]);
  }
  return g_string_free(s, FALSE);
}
static void missing(GHashTable *from, GHashTable *to, GPtrArray *errors,
                    const char *prefix) {
  GPtrArray *list = keys(from);
  for (size_t i = 0; i < list->len; i++)
    if (!g_hash_table_contains(to, list->pdata[i]))
      add(errors, "%s%s", prefix, (char *)list->pdata[i]);
  g_ptr_array_free(list, TRUE);
}
static GPtrArray *markdown_files(const char *root, bool recursive) {
  GPtrArray *out = array(), *names = children(root);
  for (size_t i = 0; i < names->len; i++) {
    char *path = g_build_filename(root, names->pdata[i], NULL);
    if (g_str_has_suffix(names->pdata[i], ".md"))
      g_ptr_array_add(out, g_strdup(path));
    struct stat st;
    if (recursive && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      GPtrArray *sub = markdown_files(path, true);
      for (size_t j = 0; j < sub->len; j++)
        g_ptr_array_add(out, g_strdup(sub->pdata[j]));
      g_ptr_array_free(sub, TRUE);
    }
    g_free(path);
  }
  g_ptr_array_free(names, TRUE);
  g_ptr_array_sort(out, compare);
  return out;
}
static void section(const char *title) { printf("\n== %s ==\n", title); }
static bool date_value(const char *s, GDate *out) {
  unsigned year, month, day;
  int used = 0;
  if (sscanf(s, "%4u-%2u-%2u%n", &year, &month, &day, &used) == 3 &&
      used == 10 && strlen(s) == 10 &&
      g_regex_match_simple("\\A[0-9]{4}-[0-9]{2}-[0-9]{2}\\z", s, 0, 0)) {
    if (!g_date_valid_dmy(day, month, year))
      return false;
    g_date_set_dmy(out, day, month, year);
    return true;
  }
  if (strlen(s) == 8 && g_regex_match_simple("\\A[0-9]{8}\\z", s, 0, 0)) {
    sscanf(s, "%4u%2u%2u", &year, &month, &day);
    if (!g_date_valid_dmy(day, month, year))
      return false;
    g_date_set_dmy(out, day, month, year);
    return true;
  }
  GRegex *re = g_regex_new(
      "\\A([0-9]{4})(?:-W([0-9]{2})(?:-([1-7]))?|W([0-9]{2})([1-7])?)\\z", 0, 0,
      NULL);
  GMatchInfo *info = NULL;
  g_regex_match(re, s, 0, &info);
  bool ok = false;
  if (g_match_info_matches(info)) {
    char *a = g_match_info_fetch(info, 1), *b = g_match_info_fetch(info, 2),
         *c = g_match_info_fetch(info, 3), *d = g_match_info_fetch(info, 4),
         *e = g_match_info_fetch(info, 5);
    year = (unsigned)atoi(a);
    unsigned week = (unsigned)atoi(*b ? b : d);
    day = (unsigned)(*c ? atoi(c) : *e ? atoi(e) : 1);
    if (year >= 1 && year <= 9999 && week >= 1 && week <= 53) {
      GDate first;
      g_date_clear(&first, 1);
      g_date_set_dmy(&first, 4, 1, year);
      guint32 julian = g_date_get_julian(&first) - g_date_get_weekday(&first) +
                       1 + (week - 1) * 7 + day - 1;
      if (julian) {
        g_date_set_julian(out, julian);
        ok = g_date_get_year(out) <= 9999 &&
             g_date_get_iso8601_week_of_year(out) == week;
      }
    }
    g_free(a);
    g_free(b);
    g_free(c);
    g_free(d);
    g_free(e);
  }
  g_match_info_free(info);
  g_regex_unref(re);
  return ok;
}
typedef struct {
  const char *term;
  GPtrArray *names;
} Overlap;
static gint overlap_order(gconstpointer a, gconstpointer b) {
  const Overlap *x = *(Overlap *const *)a, *y = *(Overlap *const *)b;
  if (x->names->len != y->names->len)
    return x->names->len > y->names->len ? -1 : 1;
  return strcmp(x->term, y->term);
}
static void list_free(gpointer p) { g_ptr_array_free(p, TRUE); }
static int audit(const char *root, const char *llm, Val *catalog) {
  GPtrArray *all = skills(llm), *errors = array(), *warnings = array();
  GHashTable *registered[2] = {set(), set()}, *disk[2] = {set(), set()};
  Val *entry;
  size_t i, n;
  yyjson_arr_foreach(catalog, i, n, entry)
      put(registered[text_is(field(entry, "kind"), "process")],
          string(field(entry, "name")));
  section("Skills");
  for (i = 0; i < all->len; i++) {
    Skill *s = all->pdata[i];
    printf("%-7s %-36s lines=%3u desc_chars=%3ld %s\n", s->kind, s->name,
           s->lines->len, (long)g_utf8_strlen(s->description, -1),
           s->description);
    put(disk[!strcmp(s->kind, "process")], s->name);
    if (!*s->description)
      add(errors, "%s: missing frontmatter description", s->name);
    if (!catalog_entry(catalog, s->name))
      add(errors,
          "%s: missing invocation classification in "
          "home/config/llm/skill-catalog.json",
          s->name);
  }
  section("Deployment Lists");
  const char *kinds[] = {"shared", "process"},
             *lists[] = {"sharedSkillNames", "processSkillNames"};
  for (size_t k = 0; k < 2; k++) {
    char *prefix =
        g_strdup_printf("%s lists missing skill directory: ", lists[k]);
    missing(registered[k], disk[k], errors, prefix);
    g_free(prefix);
    prefix = g_strdup_printf(
        "%s skill exists on disk but is absent from %s: ", kinds[k], lists[k]);
    missing(disk[k], registered[k], errors, prefix);
    g_free(prefix);
  }
  for (size_t k = 0; k < 2; k++)
    printf("%s listed=%u disk=%u\n", kinds[k], g_hash_table_size(registered[k]),
           g_hash_table_size(disk[k]));
  section("References");
  for (i = 0; i < all->len; i++) {
    Skill *s = all->pdata[i];
    if (strcmp(s->kind, "shared"))
      continue;
    char *base = g_path_get_dirname(s->path),
         *refs_dir = g_build_filename(base, "references", NULL),
         *text = joined(s->lines, G_MAXSIZE);
    GPtrArray *files = markdown_files(refs_dir, false);
    GHashTable *local = set(), *mapped = set(), *deployed = set(),
               *mentioned =
                   matches("(?:\\./)?references/([A-Za-z0-9_.-]+\\.md)", text);
    for (size_t j = 0; j < files->len; j++) {
      char *name = g_path_get_basename(files->pdata[j]);
      put(local, name);
      put(deployed, name);
      g_free(name);
    }
    Val *refs = field(catalog_entry(catalog, s->name), "references"), *key,
        *value;
    size_t j, m;
    yyjson_obj_foreach(refs, j, m, key, value) {
      put(mapped, string(key));
      put(deployed, string(key));
    }
    GPtrArray *names = keys(mentioned);
    for (j = 0; j < names->len; j++)
      if (!g_hash_table_contains(deployed, names->pdata[j]))
        add(errors,
            "%s: mentions references/%s, but it is not local or mapped in "
            "sharedSkillReferences",
            s->name, (char *)names->pdata[j]);
    g_ptr_array_free(names, TRUE);
    names = keys(local);
    for (j = 0; j < names->len; j++)
      if (!g_hash_table_contains(mentioned, names->pdata[j]))
        add(warnings, "%s: local references/%s is not mentioned by SKILL.md",
            s->name, (char *)names->pdata[j]);
    g_ptr_array_free(names, TRUE);
    printf("%-36s mentioned=%2u local=%2u mapped=%2u\n", s->name,
           g_hash_table_size(mentioned), g_hash_table_size(local),
           g_hash_table_size(mapped));
    g_hash_table_destroy(local);
    g_hash_table_destroy(mapped);
    g_hash_table_destroy(deployed);
    g_hash_table_destroy(mentioned);
    g_ptr_array_free(files, TRUE);
    g_free(base);
    g_free(refs_dir);
    g_free(text);
  }
  section("Long References Without Contents");
  const char *bases[] = {"guides", "skills"};
  for (size_t k = 0; k < 2; k++) {
    char *base = g_build_filename(llm, bases[k], NULL);
    GPtrArray *files = markdown_files(base, true);
    for (i = 0; i < files->len; i++) {
      char *path = files->pdata[i], *name = g_path_get_basename(path);
      if (strcmp(name, "SKILL.md")) {
        GPtrArray *text = lines(path);
        char *first = joined(text, 40), *lower = g_utf8_strdown(first, -1);
        if (text->len > 100 && !strstr(lower, "table of contents") &&
            !g_regex_match_simple("(*UCP)^#+\\s+contents\\b", lower,
                                  G_REGEX_MULTILINE, 0)) {
          const char *relative = path + strlen(root) + 1;
          add(warnings, "%s: %u lines and no contents section", relative,
              text->len);
          printf("%3u %s\n", text->len, relative);
        }
        g_free(first);
        g_free(lower);
        g_ptr_array_free(text, TRUE);
      }
      g_free(name);
    }
    g_ptr_array_free(files, TRUE);
    g_free(base);
  }
  section("Trigger Overlap");
  GHashTable *terms =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, list_free);
  for (i = 0; i < all->len; i++) {
    Skill *s = all->pdata[i];
    char *lower = g_utf8_strdown(s->description, -1);
    GHashTable *words = matches("([a-z][a-z0-9-]{2,})", lower);
    GPtrArray *names = keys(words);
    for (size_t j = 0; j < names->len; j++) {
      const char *word = names->pdata[j];
      char *needle = g_strconcat(" ", word, " ", NULL);
      bool skip = strstr(stopwords, needle) != NULL;
      g_free(needle);
      if (skip)
        continue;
      GPtrArray *names_ = g_hash_table_lookup(terms, word);
      if (!names_) {
        names_ = array();
        g_hash_table_insert(terms, g_strdup(word), names_);
      }
      g_ptr_array_add(names_, g_strdup(s->name));
    }
    g_ptr_array_free(names, TRUE);
    g_hash_table_destroy(words);
    g_free(lower);
  }
  GPtrArray *overlaps = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init(&it, terms);
  while (g_hash_table_iter_next(&it, &key, &value)) {
    GPtrArray *names = value;
    if (names->len >= 4) {
      Overlap *o = g_new(Overlap, 1);
      *o = (Overlap){key, names};
      g_ptr_array_add(overlaps, o);
    }
  }
  g_ptr_array_sort(overlaps, overlap_order);
  for (i = 0; i < MIN(overlaps->len, 20u); i++) {
    Overlap *o = overlaps->pdata[i];
    g_ptr_array_sort(o->names, compare);
    printf("%-24s ", o->term);
    for (size_t j = 0; j < o->names->len; j++)
      printf("%s%s", j ? ", " : "", (char *)o->names->pdata[j]);
    putchar('\n');
  }
  g_ptr_array_free(overlaps, TRUE);
  g_hash_table_destroy(terms);
  section("Explicit Invocation Metadata");
  GHashTable *classifications = set();
  yyjson_arr_foreach(catalog, i, n, entry)
      put(classifications, string(field(entry, "name")));
  GPtrArray *names = keys(classifications);
  for (i = 0; i < names->len; i++) {
    const char *name = names->pdata[i];
    Skill *skill = NULL;
    for (size_t j = 0; j < all->len; j++) {
      Skill *s = all->pdata[j];
      if (!strcmp(s->name, name))
        skill = s;
    }
    if (!skill) {
      add(errors, "home/config/llm/skill-catalog.json lists unknown skill: %s",
          name);
      continue;
    }
    entry = catalog_entry(catalog, name);
    if (text_is(field(entry, "invocation"), types[0])) {
      char *parent = g_path_get_dirname(skill->path),
           *metadata = g_build_filename(parent, "agents/openai.yaml", NULL);
      if (!exists(metadata))
        add(errors,
            "%s: user-invoked skill has no agents/openai.yaml "
            "explicit-invocation metadata",
            name);
      if (!manual(skill->lines))
        add(errors,
            "%s: user-invoked skill missing Pi disable-model-invocation: true",
            name);
      g_free(parent);
      g_free(metadata);
    }
  }
  for (size_t k = 0; k < 2; k++) {
    const char *name = k ? "using-superpowers" : "superpowers";
    Skill *skill = NULL;
    for (i = 0; i < all->len; i++) {
      Skill *s = all->pdata[i];
      if (!strcmp(s->name, name))
        skill = s;
    }
    if (skill && manual(skill->lines))
      add(warnings, "%s: process guardrail should stay model-invoked", name);
  }
  g_ptr_array_free(names, TRUE);
  g_hash_table_destroy(classifications);
  section("Source Freshness Markers");
  GPtrArray *fresh = array();
  char *base = g_build_filename(llm, "guides", NULL);
  GPtrArray *files = markdown_files(base, false);
  for (i = 0; i < files->len; i++)
    g_ptr_array_add(fresh, g_strdup(files->pdata[i]));
  g_ptr_array_free(files, TRUE);
  g_free(base);
  base = g_build_filename(llm, "skills", NULL);
  names = children(base);
  for (i = 0; i < names->len; i++) {
    char *refs = g_build_filename(base, names->pdata[i], "references", NULL);
    files = markdown_files(refs, false);
    for (size_t j = 0; j < files->len; j++)
      g_ptr_array_add(fresh, g_strdup(files->pdata[j]));
    g_ptr_array_free(files, TRUE);
    g_free(refs);
  }
  g_ptr_array_free(names, TRUE);
  g_free(base);
  base = g_build_filename(llm, "references/plugin-eval", NULL);
  files = markdown_files(base, true);
  for (i = 0; i < files->len; i++)
    g_ptr_array_add(fresh, g_strdup(files->pdata[i]));
  g_ptr_array_free(files, TRUE);
  g_free(base);
  unsigned markers[3] = {0};
  const char *headers[] = {"Source", "Checked", "Update trigger"};
  GDateTime *now = g_date_time_new_now_local();
  GDate today;
  g_date_clear(&today, 1);
  g_date_set_dmy(&today, g_date_time_get_day_of_month(now),
                 g_date_time_get_month(now), g_date_time_get_year(now));
  g_date_time_unref(now);
  for (i = 0; i < fresh->len; i++) {
    const char *path = fresh->pdata[i], *relative = path + strlen(root) + 1;
    GPtrArray *text = lines(path);
    char *values[3] = {NULL};
    for (size_t j = 0; j < MIN(text->len, 12u); j++)
      for (size_t k = 0; k < 3; k++) {
        char *prefix = g_strconcat(headers[k], ":", NULL);
        if (g_str_has_prefix(text->pdata[j], prefix)) {
          g_free(values[k]);
          values[k] = strip((char *)text->pdata[j] + strlen(prefix));
        }
        g_free(prefix);
      }
    for (size_t k = 0; k < 3; k++)
      if (values[k])
        markers[k]++;
    if (values[0])
      for (size_t k = 1; k < 3; k++)
        if (!values[k])
          add(warnings, "%s: Source header missing %s:", relative, headers[k]);
    if (values[1]) {
      GDate checked;
      g_date_clear(&checked, 1);
      if (!date_value(values[1], &checked))
        add(warnings, "%s: invalid Checked date '%s'", relative, values[1]);
      else {
        gint64 age = (gint64)g_date_get_julian(&today) -
                     (gint64)g_date_get_julian(&checked);
        if (age > 120)
          add(warnings, "%s: freshness check is %" G_GINT64_FORMAT " days old",
              relative, age);
      }
    }
    for (size_t k = 0; k < 3; k++)
      g_free(values[k]);
    g_ptr_array_free(text, TRUE);
  }
  printf("Source=%u Checked=%u Update trigger=%u\n", markers[0], markers[1],
         markers[2]);
  g_ptr_array_free(fresh, TRUE);
  section("Quarterly Review Prompts");
  const char *prompts[] = {
      "OpenAI/Codex docs and skills behavior",
      "Agent skill authoring conventions",
      "Jira API/auth behavior",
      "GitHub review API behavior",
      "Private domain workflow conventions",
      "RSpec/Rails conventions after major framework changes"};
  for (i = 0; i < G_N_ELEMENTS(prompts); i++)
    printf("- %s\n", prompts[i]);
  if (io_failed)
    add(errors, "Cannot read skill/reference text.");
  section("Findings");
  if (errors->len) {
    puts("Errors:");
    for (i = 0; i < errors->len; i++)
      printf("- %s\n", (char *)errors->pdata[i]);
  }
  if (warnings->len) {
    puts("Warnings:");
    for (i = 0; i < warnings->len; i++)
      printf("- %s\n", (char *)warnings->pdata[i]);
  }
  if (!errors->len && !warnings->len)
    puts("No findings.");
  int status = errors->len || warnings->len ? 1 : 0;
  for (size_t k = 0; k < 2; k++) {
    g_hash_table_destroy(registered[k]);
    g_hash_table_destroy(disk[k]);
  }
  g_ptr_array_free(all, TRUE);
  g_ptr_array_free(errors, TRUE);
  g_ptr_array_free(warnings, TRUE);
  return status;
}
int main(int argc, char **argv) {
  const struct option options[] = {{"root", required_argument, NULL, 'r'},
                                   {"help", no_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
  const char *path = ".";
  int opt;
  opterr = 0;
  while ((opt = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    if (opt == 'h') {
      puts("usage: audit-skills [--root DOTFILES_CHECKOUT]");
      return 0;
    }
    if (opt != 'r')
      return 2;
    path = optarg;
  }
  if (optind != argc)
    return 2;
  char *root = realpath(path, NULL);
  if (!root) {
    fputs("Invalid skill catalogue: checkout is unavailable\n", stderr);
    return 1;
  }
  char *llm = g_build_filename(root, "home/config/llm", NULL);
  char *catalog_path = g_build_filename(llm, "skill-catalog.json", NULL);
  yyjson_doc *doc = load_json(catalog_path);
  g_free(catalog_path);
  char *error = NULL;
  int status = 1;
  if (!doc)
    error = g_strdup("cannot read catalogue JSON");
  else if (!unique(yyjson_doc_get_root(doc), 0))
    error = g_strdup("duplicate JSON key or excessive nesting");
  else if (validate(yyjson_doc_get_root(doc), llm, &error))
    status = audit(root, llm, yyjson_doc_get_root(doc));
  if (error)
    fprintf(stderr, "Invalid skill catalogue: %s\n", error);
  g_free(error);
  if (doc)
    yyjson_doc_free(doc);
  g_free(llm);
  free(root);
  return status;
}
