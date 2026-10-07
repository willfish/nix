#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  char *path, *name, *pname;
  GHashTable *sources, *labels, *paths;
  GPtrArray *closure;
  guint64 direct, total, unique;
  size_t unique_paths, roots, order;
} Row;
typedef struct {
  guint64 bytes;
  size_t refs;
  bool known;
} Size;
static bool failed;
static gboolean color;
static GHashTable *set(void) {
  return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}
static void add(GHashTable *s, const char *key) {
  g_hash_table_add(s, g_strdup(key));
}
static gint strings(gconstpointer a, gconstpointer b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static GPtrArray *keys(GHashTable *s) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, s);
  while (g_hash_table_iter_next(&it, &key, NULL))
    g_ptr_array_add(out, g_strdup(key));
  g_ptr_array_sort(out, strings);
  return out;
}
static char *joined(GHashTable *s) {
  GPtrArray *k = keys(s);
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < k->len; i++) {
    if (i)
      g_string_append_c(out, ',');
    g_string_append(out, k->pdata[i]);
  }
  g_ptr_array_free(k, TRUE);
  return g_string_free(out, FALSE);
}
static void union_(GHashTable *dest, GHashTable *src) {
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, src);
  while (g_hash_table_iter_next(&it, &key, NULL))
    add(dest, key);
}
static void free_row(gpointer p) {
  Row *r = p;
  g_free(r->path);
  g_free(r->name);
  g_free(r->pname);
  g_hash_table_destroy(r->sources);
  g_hash_table_destroy(r->labels);
  g_hash_table_destroy(r->paths);
  g_ptr_array_free(r->closure, TRUE);
  g_free(r);
}
static Row *row(const char *path, const char *name, const char *pname,
                size_t order) {
  Row *r = g_new0(Row, 1);
  r->path = g_strdup(path);
  r->name = g_strdup(name);
  r->pname = g_strdup(pname);
  r->sources = set();
  r->labels = set();
  r->paths = set();
  r->closure = g_ptr_array_new_with_free_func(g_free);
  r->order = order;
  return r;
}
static gint path_order(gconstpointer a, gconstpointer b) {
  return strcmp((*(Row *const *)a)->path, (*(Row *const *)b)->path);
}
static gint rank(gconstpointer a, gconstpointer b) {
  const Row *x = *(Row *const *)a, *y = *(Row *const *)b;
  if (x->unique != y->unique)
    return x->unique > y->unique ? -1 : 1;
  if (x->total != y->total)
    return x->total > y->total ? -1 : 1;
  return x->order < y->order ? -1 : x->order > y->order;
}
static char *run(char **args, bool quiet, bool *ok) {
  char *out = NULL, *err = NULL;
  int status = 0;
  bool started = g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                              &out, quiet ? &err : NULL, &status, NULL);
  g_free(err);
  *ok = started && g_spawn_check_wait_status(status, NULL);
  if (!started) {
    g_free(out);
    return NULL;
  }
  return out;
}
static yyjson_doc *query_json(char **args) {
  bool ok;
  char *out = run(args, false, &ok);
  yyjson_doc *doc = ok && out ? yyjson_read(out, strlen(out), 0) : NULL;
  g_free(out);
  if (!doc)
    failed = true;
  return doc;
}
static Size *size_for(GHashTable *sizes, const char *path) {
  Size *s = g_hash_table_lookup(sizes, path);
  if (!s) {
    s = g_new0(Size, 1);
    g_hash_table_insert(sizes, g_strdup(path), s);
  }
  return s;
}
static char *human(guint64 n) {
  const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = (double)n;
  size_t i = 0;
  while (v >= 1024 && i < 4) {
    v /= 1024;
    i++;
  }
  return i ? g_strdup_printf("%.1f %s", v, units[i])
           : g_strdup_printf("%" G_GUINT64_FORMAT " B", n);
}
static char *paint(const char *text, const char *code) {
  return color ? g_strdup_printf("\033[%sm%s\033[0m", code, text)
               : g_strdup(text);
}
static char *shorten(const char *text, size_t width) {
  glong n = g_utf8_strlen(text, -1);
  if ((size_t)n <= width)
    return g_strdup(text);
  size_t chars = width <= 3 ? width : width - 3;
  char *part = g_strndup(
      text, (gsize)(g_utf8_offset_to_pointer(text, (glong)chars) - text));
  char *out = width <= 3 ? g_strdup(part) : g_strconcat(part, "...", NULL);
  g_free(part);
  return out;
}
static char *display_sources(const char *s) {
  char **parts = g_strsplit(s, "home-manager", -1);
  char *out = g_strjoinv("home", parts);
  g_strfreev(parts);
  return out;
}
static char *table(char **columns, size_t *widths) {
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < 6; i++) {
    if (i)
      g_string_append(out, "  ");
    g_string_append(out, columns[i]);
    size_t n = (size_t)g_utf8_strlen(columns[i], -1);
    while (n++ < widths[i])
      g_string_append_c(out, ' ');
  }
  return g_string_free(out, FALSE);
}
static void csv_cell(GString *out, const char *s) {
  bool quote = strpbrk(s, ",\r\n\"") != NULL;
  if (quote)
    g_string_append_c(out, '"');
  for (; *s; s++) {
    if (*s == '"')
      g_string_append_c(out, '"');
    g_string_append_c(out, *s);
  }
  if (quote)
    g_string_append_c(out, '"');
}
static guint64 closure_size(const char *path) {
  if (!*path)
    return 0;
  char *args[] = {"nix", "path-info", "--json",     "--json-format",
                  "1",   "-S",        (char *)path, NULL};
  yyjson_doc *doc = query_json(args);
  if (!doc)
    return 0;
  Val *meta = field(yyjson_doc_get_root(doc), path),
      *size = field(meta, "closureSize");
  if (!yyjson_is_uint(size))
    failed = true;
  guint64 n = yyjson_get_uint(size);
  yyjson_doc_free(doc);
  return n;
}
static char *suffix(const char *prefix, const char *ext) {
  char *s = g_strdup(prefix), *base = strrchr(s, '/'),
       *dot = strrchr(base ? base + 1 : s, '.');
  if (dot && dot != (base ? base + 1 : s))
    *dot = 0;
  char *out = g_strconcat(s, ext, NULL);
  g_free(s);
  return out;
}
static gint source_order(gconstpointer a, gconstpointer b, gpointer sizes) {
  const char *x = *(char *const *)a, *y = *(char *const *)b;
  guint64 vx = *(guint64 *)g_hash_table_lookup(sizes, x),
          vy = *(guint64 *)g_hash_table_lookup(sizes, y);
  return vx > vy ? -1 : vx < vy;
}
int main(int argc, char **argv) {
  if (argc != 10) {
    fputs("usage: nix-storage-report INVENTORY SYSTEM HOME HOST HOME_CONFIG "
          "OUT SUMMARY LIMIT COLOR\n",
          stderr);
    return 2;
  }
  const char *system = argv[2], *home = argv[3], *host = argv[4],
             *home_config = argv[5];
  bool summary = !strcmp(argv[7], "1");
  size_t limit = (size_t)g_ascii_strtoull(argv[8], NULL, 10);
  color = !strcmp(argv[9], "always") || (!strcmp(argv[9], "auto") && isatty(1));
  yyjson_doc *inventory = load_json(argv[1]);
  if (!inventory || !yyjson_is_arr(yyjson_doc_get_root(inventory))) {
    if (inventory)
      yyjson_doc_free(inventory);
    return 1;
  }
  GPtrArray *all = g_ptr_array_new_with_free_func(free_row),
            *valid = g_ptr_array_new(), *skipped = g_ptr_array_new(),
            *groups = g_ptr_array_new_with_free_func(free_row);
  GHashTable *index = g_hash_table_new(g_str_hash, g_str_equal),
             *sizes =
                 g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free),
             *all_paths = set(),
             *group_index = g_hash_table_new(g_str_hash, g_str_equal);
  Val *entry;
  size_t i, n;
  yyjson_arr_foreach(yyjson_doc_get_root(inventory), i, n, entry) {
    const char *path = yyjson_get_str(field(entry, "storePath")),
               *name = yyjson_get_str(field(entry, "name")),
               *pname = yyjson_get_str(field(entry, "pname")),
               *source = yyjson_get_str(field(entry, "source")),
               *label = yyjson_get_str(field(entry, "label"));
    if (!path || !name || !pname || !source || !label) {
      failed = true;
      break;
    }
    Row *r = g_hash_table_lookup(index, path);
    if (!r) {
      r = row(path, name, pname, all->len);
      g_ptr_array_add(all, r);
      g_hash_table_insert(index, r->path, r);
    }
    add(r->sources, source);
    add(r->labels, label);
  }
  yyjson_doc_free(inventory);
  g_ptr_array_sort(all, path_order);
  for (i = 0; i < all->len && !failed; i++) {
    Row *r = all->pdata[i];
    r->order = i;
    char *args[] = {"nix-store", "--query", "--hash", r->path, NULL};
    bool ok;
    char *out = run(args, true, &ok);
    if (!out) {
      failed = true;
      break;
    }
    g_free(out);
    g_ptr_array_add(ok ? valid : skipped, r);
  }
  if (skipped->len) {
    fprintf(stderr,
            "warning: skipping %u unrealized inventory root(s) (evaluated "
            "package path not present in local store):\n",
            skipped->len);
    for (i = 0; i < skipped->len; i++) {
      Row *r = skipped->pdata[i];
      char *labels = joined(r->labels);
      fprintf(stderr, "  - %s (%s) -> %s\n", r->name, labels, r->path);
      g_free(labels);
    }
  }
  for (i = 0; i < valid->len && !failed; i++) {
    Row *r = valid->pdata[i];
    char *args[] = {"nix-store", "-qR", r->path, NULL};
    bool ok;
    char *out = run(args, false, &ok);
    if (!ok || !out) {
      g_free(out);
      failed = true;
      break;
    }
    char **lines = g_strsplit(out, "\n", -1);
    for (size_t j = 0; lines[j]; j++) {
      g_strchomp(lines[j]);
      if (!*lines[j])
        continue;
      g_ptr_array_add(r->closure, g_strdup(lines[j]));
      add(r->paths, lines[j]);
      add(all_paths, lines[j]);
      size_for(sizes, lines[j])->refs++;
    }
    g_strfreev(lines);
    g_free(out);
  }
  GHashTable *queried = set();
  union_(queried, all_paths);
  if (*system)
    add(queried, system);
  if (*home)
    add(queried, home);
  GPtrArray *paths = keys(queried);
  for (i = 0; i < paths->len && !failed; i += 400) {
    GPtrArray *args = g_ptr_array_new();
    const char *prefix[] = {"nix", "path-info", "--json", "--json-format", "1"};
    for (size_t j = 0; j < 5; j++)
      g_ptr_array_add(args, (gpointer)prefix[j]);
    for (size_t j = i; j < MIN(i + 400, paths->len); j++)
      g_ptr_array_add(args, paths->pdata[j]);
    g_ptr_array_add(args, NULL);
    yyjson_doc *doc = query_json((char **)args->pdata);
    g_ptr_array_free(args, TRUE);
    if (!doc)
      break;
    Val *key, *meta;
    size_t j, m;
    yyjson_obj_foreach(yyjson_doc_get_root(doc), j, m, key, meta) {
      Size *s = size_for(sizes, yyjson_get_str(key));
      s->known = true;
      Val *value = field(meta, "narSize");
      s->bytes = yyjson_get_uint(value);
    }
    yyjson_doc_free(doc);
  }
  g_ptr_array_free(paths, TRUE);
  g_hash_table_destroy(queried);
  guint64 inventory_total = 0;
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, all_paths);
  while (g_hash_table_iter_next(&it, &key, NULL)) {
    Size *s = size_for(sizes, key);
    if (!s->known)
      failed = true;
    inventory_total += s->bytes;
  }
  for (i = 0; i < valid->len; i++) {
    Row *r = valid->pdata[i];
    r->direct = size_for(sizes, r->path)->bytes;
    for (size_t j = 0; j < r->closure->len; j++) {
      Size *s = size_for(sizes, r->closure->pdata[j]);
      r->total += s->bytes;
      if (s->refs == 1) {
        r->unique += s->bytes;
        r->unique_paths++;
      }
    }
  }
  g_ptr_array_sort(valid, rank);
  guint64 system_total = failed ? 0 : closure_size(system),
          home_total = failed ? 0 : closure_size(home);
  for (i = 0; i < valid->len; i++) {
    Row *r = valid->pdata[i],
        *group = g_hash_table_lookup(group_index, r->name);
    if (!group) {
      group = row("", r->name, "", groups->len);
      g_ptr_array_add(groups, group);
      g_hash_table_insert(group_index, group->name, group);
    }
    union_(group->sources, r->sources);
    union_(group->labels, r->labels);
    union_(group->paths, r->paths);
    group->direct += r->direct;
    group->roots++;
  }
  for (i = 0; i < groups->len; i++) {
    Row *r = groups->pdata[i];
    g_hash_table_iter_init(&it, r->paths);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
      Size *s = size_for(sizes, key);
      r->total += s->bytes;
      if (s->refs == 1)
        r->unique += s->bytes;
    }
  }
  g_ptr_array_sort(groups, rank);
  char *safe_home = g_strdup(*home_config ? home_config : "system");
  for (char *p = safe_home; *p; p++)
    if (*p == '@' || *p == '/')
      *p = '_';
  char *base = g_strdup_printf("%s-%s-storage-costs", host, safe_home),
       *prefix = g_build_filename(argv[6], base, NULL),
       *csv_path = suffix(prefix, ".csv"), *md_path = suffix(prefix, ".md");
  g_free(safe_home);
  g_free(base);
  g_free(prefix);
  GString
      *csv = g_string_new(
          "name,pname,sources,labels,store_path,direct_bytes,closure_bytes,"
          "unique_inventory_bytes,closure_paths,unique_inventory_paths\r\n"),
      *md = g_string_new(NULL);
  GHashTable *totals =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GPtrArray *source_names = g_ptr_array_new_with_free_func(g_free);
  for (i = 0; i < valid->len; i++) {
    Row *r = valid->pdata[i];
    char *sources = joined(r->sources), *labels = joined(r->labels);
    const char *cells[] = {r->name, r->pname, sources, labels, r->path};
    for (size_t j = 0; j < 5; j++) {
      if (j)
        g_string_append_c(csv, ',');
      csv_cell(csv, cells[j]);
    }
    g_string_append_printf(csv,
                           ",%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT
                           ",%" G_GUINT64_FORMAT ",%u,%zu\r\n",
                           r->direct, r->total, r->unique, r->closure->len,
                           r->unique_paths);
    char **parts = g_strsplit(sources, ",", -1);
    for (size_t j = 0; parts[j]; j++) {
      guint64 *total = g_hash_table_lookup(totals, parts[j]);
      if (!total) {
        total = g_new0(guint64, 1);
        g_hash_table_insert(totals, g_strdup(parts[j]), total);
        g_ptr_array_add(source_names, g_strdup(parts[j]));
      }
      *total += r->unique;
    }
    g_strfreev(parts);
    g_free(sources);
    g_free(labels);
  }
  g_ptr_array_sort_with_data(source_names, source_order, totals);
  GDateTime *now = g_date_time_new_now_utc();
  char *date = g_date_time_format(now, "%Y-%m-%d %H:%M:%S UTC"),
       *inv = human(inventory_total), *sys = human(system_total),
       *hm = human(home_total);
  g_date_time_unref(now);
  g_string_append_printf(
      md, "# Storage Costs for %s\n\nGenerated: %s\n\n## Scope\n\n", host,
      date);
  g_free(date);
  if (*system)
    g_string_append_printf(md, "- NixOS system: `%s`\n", system);
  if (*home)
    g_string_append_printf(md, "- Home Manager: `%s` -> `%s`\n", home_config,
                           home);
  g_string_append_printf(
      md, "- Inventory roots: %u de-duplicated store paths\n", valid->len);
  if (skipped->len)
    g_string_append_printf(md,
                           "- Skipped unrealized roots: %u (evaluated package "
                           "paths not present in the local store)\n",
                           skipped->len);
  g_string_append_printf(md, "- Inventory closure: %s\n", inv);
  if (*system)
    g_string_append_printf(md, "- NixOS system closure: %s\n", sys);
  if (*home)
    g_string_append_printf(md, "- Home Manager activation closure: %s\n", hm);
  g_string_append(
      md, "\n`closure` is the full dependency closure for that root. `unique` "
          "is the subset of that closure referenced by only one root in this "
          "inventory; it is the better first-pass signal for storage you might "
          "save by removing one item from these package/program roots.\n\n");
  if (skipped->len) {
    g_string_append(md,
                    "## Skipped Unrealized Roots\n\nThese package attribute "
                    "paths were evaluated from the flake but are not realized "
                    "locally (common when Home Manager installs an overridden "
                    "variant of `programs.<name>.package`). They are omitted "
                    "from size accounting.\n\n| Package | Source | Labels | "
                    "Store path |\n| --- | --- | --- | --- |\n");
    for (i = 0; i < skipped->len; i++) {
      Row *r = skipped->pdata[i];
      char *sources = joined(r->sources), *labels = joined(r->labels);
      g_string_append_printf(md, "| `%s` | `%s` | `%s` | `%s` |\n", r->name,
                             sources, labels, r->path);
      g_free(sources);
      g_free(labels);
    }
    g_string_append_c(md, '\n');
  }
  g_string_append(
      md,
      "## Unique Storage by Source\n\n| Source | Unique |\n| --- | ---: |\n");
  for (i = 0; i < source_names->len; i++) {
    const char *source = source_names->pdata[i];
    char *value = human(*(guint64 *)g_hash_table_lookup(totals, source));
    g_string_append_printf(md, "| `%s` | %s |\n", source, value);
    g_free(value);
  }
  g_string_append(md, "\n## Top Packages by Unique Inventory Size\n\n| Package "
                      "| Roots | Source | Unique | Closure | Direct | Labels "
                      "|\n| --- | ---: | --- | ---: | ---: | ---: | --- |\n");
  for (i = 0; i < MIN(groups->len, 40u); i++) {
    Row *r = groups->pdata[i];
    char *sources = joined(r->sources), *labels = joined(r->labels),
         *unique = human(r->unique), *total = human(r->total),
         *direct = human(r->direct);
    g_string_append_printf(md, "| `%s` | %zu | `%s` | %s | %s | %s | `%s` |\n",
                           r->name, r->roots, sources, unique, total, direct,
                           labels);
    g_free(sources);
    g_free(labels);
    g_free(unique);
    g_free(total);
    g_free(direct);
  }
  g_string_append_printf(md, "\nFull CSV: `%s`\n", csv_path);
  if (!failed)
    failed = !write_text(csv_path, csv->str, csv->len) ||
             !write_text(md_path, md->str, md->len);
  if (!failed) {
    printf("%s\n%s\ninventory_roots=%u\nskipped_unrealized_roots=%u\ninventory_"
           "closure=%s\nsystem_closure=%s\nhome_closure=%s\n",
           md_path, csv_path, valid->len, skipped->len, inv, sys, hm);
    if (summary) {
      size_t widths[] = {7, 5, 6, 11, 11, 11};
      size_t visible = MIN(limit, groups->len);
      for (i = 0; i < visible; i++) {
        Row *r = groups->pdata[i];
        widths[0] =
            MIN(36u, MAX(widths[0], (size_t)g_utf8_strlen(r->name, -1)));
        char *sources = joined(r->sources), *display = display_sources(sources);
        widths[2] =
            MIN(14u, MAX(widths[2], (size_t)g_utf8_strlen(display, -1)));
        g_free(sources);
        g_free(display);
      }
      char *heading = g_strdup_printf("Storage costs for %s (%s)", host,
                                      *home_config ? home_config : "NixOS"),
           *colored = paint(heading, "1;36");
      printf("\n%s\n", colored);
      g_free(heading);
      g_free(colored);
      colored = paint(inv, "1;33");
      printf("Inventory %s  ", colored);
      g_free(colored);
      if (*system) {
        colored = paint(sys, "1;32");
        printf("NixOS %s  ", colored);
        g_free(colored);
      }
      if (*home) {
        colored = paint(hm, "1;32");
        printf("Home %s  ", colored);
        g_free(colored);
      }
      char *roots = g_strdup_printf("%u", valid->len);
      colored = paint(roots, "1;34");
      printf("Roots %s", colored);
      g_free(roots);
      g_free(colored);
      if (skipped->len) {
        roots = g_strdup_printf("%u", skipped->len);
        colored = paint(roots, "1;33");
        printf("  Skipped %s", colored);
        g_free(roots);
        g_free(colored);
      }
      printf("\n\n");
      if (visible) {
        char *headers[] = {"Package", "Roots",   "Source",
                           "Unique",  "Closure", "Direct"};
        char *line = table(headers, widths);
        colored = paint(line, "1");
        puts(colored);
        g_free(colored);
        g_free(line);
        char *dashes[6];
        for (i = 0; i < 6; i++)
          dashes[i] = g_strnfill(widths[i], '-');
        line = table(dashes, widths);
        colored = paint(line, "2");
        puts(colored);
        g_free(colored);
        g_free(line);
        for (i = 0; i < 6; i++)
          g_free(dashes[i]);
        for (i = 0; i < visible; i++) {
          Row *r = groups->pdata[i];
          char *sources = joined(r->sources),
               *display = display_sources(sources), *unique = human(r->unique);
          char *cells[] = {shorten(r->name, widths[0]),
                           g_strdup_printf("%zu", r->roots),
                           shorten(display, widths[2]),
                           paint(unique, r->unique >= 1073741824  ? "31"
                                         : r->unique >= 536870912 ? "33"
                                                                  : "0"),
                           human(r->total),
                           human(r->direct)};
          line = table(cells, widths);
          puts(line);
          g_free(line);
          for (size_t j = 0; j < 6; j++)
            g_free(cells[j]);
          g_free(sources);
          g_free(display);
          g_free(unique);
        }
      } else {
        colored = paint("No realized inventory roots to report.", "33");
        puts(colored);
        g_free(colored);
      }
      char *a = paint(md_path, "36"), *b = paint(csv_path, "36");
      printf("\nFull report: %s\nCSV:         %s\n", a, b);
      g_free(a);
      g_free(b);
    }
  }
  g_free(inv);
  g_free(sys);
  g_free(hm);
  g_free(csv_path);
  g_free(md_path);
  g_string_free(csv, TRUE);
  g_string_free(md, TRUE);
  g_hash_table_destroy(totals);
  g_ptr_array_free(source_names, TRUE);
  g_hash_table_destroy(index);
  g_hash_table_destroy(group_index);
  g_hash_table_destroy(sizes);
  g_hash_table_destroy(all_paths);
  g_ptr_array_free(valid, TRUE);
  g_ptr_array_free(skipped, TRUE);
  g_ptr_array_free(groups, TRUE);
  g_ptr_array_free(all, TRUE);
  if (failed)
    fputs("Storage-cost report failed.\n", stderr);
  return failed ? 1 : 0;
}
