#include "common.h"
#include <stdio.h>
#include <string.h>

static char *failure;
static bool require(bool condition, const char *label, const char *reason) {
  if (!condition && !failure)
    failure =
        label ? g_strdup_printf("%s: %s", label, reason) : g_strdup(reason);
  return condition;
}
static bool key_is(Val *key, const char *const *names, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (text_is(key, names[i]))
      return true;
  return false;
}
static bool allowed_keys(Val *object, const char *const *names, size_t count) {
  Val *key, *v;
  size_t i, n;
  yyjson_obj_foreach(object, i, n, key,
                     v) if (!key_is(key, names, count)) return false;
  return true;
}
static bool matches(Val *v, const char *pattern) {
  return yyjson_is_str(v) && strlen(yyjson_get_str(v)) == yyjson_get_len(v) &&
         g_regex_match_simple(pattern, yyjson_get_str(v), 0, 0);
}
static bool digest(Val *v) {
  if (!yyjson_is_str(v) || yyjson_get_len(v) != 51)
    return false;
  const char *s = yyjson_get_str(v);
  if (memcmp(s, "sha256-", 7) || s[50] != '=')
    return false;
  for (size_t i = 7; i < 50; i++)
    if (!g_ascii_isalnum(s[i]) && s[i] != '+' && s[i] != '/')
      return false;
  gsize n;
  guchar *out = g_base64_decode(s + 7, &n);
  g_free(out);
  return n == 32;
}
static bool source(Val *v, const char *label, bool locked) {
  if (!require(yyjson_is_obj(v), label, "expected a source object"))
    return false;
  const char *types[] = {"github", "gitlab",    "sourcehut",
                         "git",    "mercurial", "tarball",
                         "file",   "path",      "indirect"};
  size_t type = 0;
  for (; type < G_N_ELEMENTS(types); type++)
    if (text_is(field(v, "type"), types[type]))
      break;
  if (!require(type < G_N_ELEMENTS(types), label, "unsupported source type") ||
      !require(!locked || type != 8, label, "unresolved source"))
    return false;
  const char *required = type < 3    ? "owner"
                         : type < 7  ? "url"
                         : type == 7 ? "path"
                                     : "id";
  if (!require(nonempty(field(v, required)), label,
               "missing or invalid source identity") ||
      (type < 3 &&
       !require(nonempty(field(v, "repo")), label, "missing or invalid repo")))
    return false;
  Val *key, *item;
  size_t i, n;
  yyjson_obj_foreach(
      v, i, n, key,
      item) if (!require(yyjson_is_str(item) || yyjson_is_bool(item) ||
                             integer(item),
                         label, "unsupported source attributes")) return false;
  const char *strings[] = {"rev", "ref", "host", "dir"};
  for (i = 0; i < G_N_ELEMENTS(strings); i++) {
    Val *item = field(v, strings[i]);
    if (item && !require(yyjson_is_str(item), label, "invalid source string"))
      return false;
  }
  if (type == 0 &&
      !require(matches(field(v, "owner"), "\\A[A-Za-z0-9][A-Za-z0-9-]*\\z") &&
                   matches(field(v, "repo"), "\\A[A-Za-z0-9_.-]+\\z") &&
                   !text_is(field(v, "repo"), ".") &&
                   !text_is(field(v, "repo"), ".."),
               label, "invalid GitHub repository"))
    return false;
  if (locked) {
    if (!require(digest(field(v, "narHash")), label,
                 "missing or invalid content hash"))
      return false;
    if (type == 0 && !require(matches(field(v, "rev"), "\\A[0-9a-f]{40}\\z"),
                              label, "missing or invalid GitHub revision"))
      return false;
  }
  const char *counts[] = {"lastModified", "revCount"};
  for (i = 0; i < G_N_ELEMENTS(counts); i++) {
    Val *item = field(v, counts[i]);
    if (!item)
      continue;
    bool positive = integer(item);
    if (yyjson_is_sint(item))
      positive = yyjson_get_sint(item) >= 0;
    else if (yyjson_is_raw(item))
      positive = yyjson_get_raw(item)[0] != '-';
    if (!require(positive, label, "invalid revision metadata"))
      return false;
  }
  return true;
}
typedef struct {
  Val *nodes, *root;
  GHashTable *active, *resolved;
} Graph;
static Val *resolve(Graph *g, Val *node, const char *name, size_t length,
                    unsigned depth) {
  Val *target = yyjson_obj_getn(field(node, "inputs"), name, length);
  if (!require(target != NULL, NULL, "dangling follows path") ||
      !require(depth < 1000 && !g_hash_table_contains(g->active, target), NULL,
               "follows cycle"))
    return NULL;
  Val *cached = g_hash_table_lookup(g->resolved, target);
  if (cached)
    return cached;
  g_hash_table_add(g->active, target);
  Val *destination;
  if (yyjson_is_arr(target)) {
    destination = g->root;
    Val *part;
    size_t i, n;
    yyjson_arr_foreach(target, i, n, part) {
      destination = resolve(g, destination, yyjson_get_str(part),
                            yyjson_get_len(part), depth + 1);
      if (!destination)
        break;
    }
  } else
    destination = yyjson_obj_getn(g->nodes, yyjson_get_str(target),
                                  yyjson_get_len(target));
  g_hash_table_remove(g->active, target);
  if (destination)
    g_hash_table_insert(g->resolved, target, destination);
  return destination;
}
static bool validate(Val *lock) {
  const char *top[] = {"nodes", "root", "version"};
  if (!require(yyjson_is_obj(lock) && yyjson_obj_size(lock) == 3 &&
                   allowed_keys(lock, top, 3),
               NULL, "unsupported lock-file structure"))
    return false;
  Val *version = field(lock, "version"), *nodes = field(lock, "nodes"),
      *root_name = field(lock, "root");
  if (!require(yyjson_is_int(version) && yyjson_get_uint(version) == 7, NULL,
               "unsupported lock-file version"))
    return false;
  Val *root = nonempty(root_name)
                  ? yyjson_obj_getn(nodes, yyjson_get_str(root_name),
                                    yyjson_get_len(root_name))
                  : NULL;
  if (!require(yyjson_is_obj(nodes) && yyjson_obj_size(nodes) && root != NULL,
               NULL, "missing or invalid root node"))
    return false;
  Val *name, *node;
  size_t i, n;
  yyjson_obj_foreach(nodes, i, n, name, node) {
    if (!require(nonempty(name) && yyjson_is_obj(node), NULL, "invalid node"))
      return false;
    const char *label = yyjson_get_str(name);
    const char *keys[] = {"inputs", "locked", "original", "flake"};
    if (!require(allowed_keys(node, keys, node == root ? 1 : 4), label,
                 "unsupported node attributes"))
      return false;
    Val *inputs = field(node, "inputs"), *flake = field(node, "flake");
    if (inputs && !require(yyjson_is_obj(inputs), label, "invalid inputs"))
      return false;
    if (flake &&
        (!require(yyjson_is_bool(flake), label, "invalid flake flag") ||
         !require(yyjson_get_bool(flake) || !yyjson_obj_size(inputs), label,
                  "non-flake has inputs")))
      return false;
    if (node != root && (!source(field(node, "locked"), label, true) ||
                         !source(field(node, "original"), label, false)))
      return false;
    Val *key, *target;
    size_t j, m;
    yyjson_obj_foreach(inputs, j, m, key, target) {
      if (!require(nonempty(key), label, "invalid input name"))
        return false;
      if (yyjson_is_str(target)) {
        if (!require(yyjson_obj_getn(nodes, yyjson_get_str(target),
                                     yyjson_get_len(target)) != NULL,
                     label, "dangling input reference"))
          return false;
      } else {
        if (!require(yyjson_is_arr(target), label,
                     "unsupported input reference"))
          return false;
        Val *part;
        size_t a, b;
        yyjson_arr_foreach(
            target, a, b,
            part) if (!require(nonempty(part), label,
                               "unsupported input reference")) return false;
      }
    }
  }
  Graph graph = {nodes, root, g_hash_table_new(g_direct_hash, g_direct_equal),
                 g_hash_table_new(g_direct_hash, g_direct_equal)};
  bool ok = true;
  yyjson_obj_foreach(nodes, i, n, name, node) {
    Val *key, *v;
    size_t j, m;
    yyjson_obj_foreach(field(node, "inputs"), j, m, key,
                       v) if (!resolve(&graph, node, yyjson_get_str(key),
                                       yyjson_get_len(key), 0)) ok = false;
  }
  g_hash_table_destroy(graph.active);
  g_hash_table_destroy(graph.resolved);
  return ok;
}
static bool excluding(Val *a, Val *b, const char *const *skip,
                      size_t skip_count) {
  size_t count = 0, other = 0, i, n;
  Val *key, *v;
  yyjson_obj_foreach(a, i, n, key, v) {
    if (key_is(key, skip, skip_count))
      continue;
    count++;
    if (!same(v, yyjson_obj_getn(b, yyjson_get_str(key), yyjson_get_len(key))))
      return false;
  }
  yyjson_obj_foreach(b, i, n, key, v) if (!key_is(key, skip, skip_count))
      other++;
  return count == other;
}
static GHashTable *owners(void) {
  GHashTable *set =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  const char *raw = g_getenv("AUTO_MERGE_GITHUB_OWNERS");
  if (!raw)
    return set;
  const char *start = raw, *p = raw;
  while (*p) {
    gunichar c = g_utf8_get_char_validated(p, -1);
    if (c == (gunichar)-1 || c == (gunichar)-2) {
      p++;
      continue;
    }
    bool line = (c >= 10 && c <= 13) || (c >= 28 && c <= 30) || c == 0x85 ||
                c == 0x2028 || c == 0x2029;
    if (line) {
      if (p > start)
        g_hash_table_add(set, g_strndup(start, (size_t)(p - start)));
      p = g_utf8_next_char(p);
      if (c == '\r' && *p == '\n')
        p++;
      start = p;
    } else
      p = g_utf8_next_char(p);
  }
  if (p > start)
    g_hash_table_add(set, g_strndup(start, (size_t)(p - start)));
  return set;
}
static gint compare(gconstpointer a, gconstpointer b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static int usage(void) {
  fputs("usage: check-flake-lock-update [-h] base head\n", stderr);
  return 2;
}
int main(int argc, char **argv) {
  const char *base_path = NULL, *head_path = NULL;
  bool options = true, bad = false;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (options && !strcmp(arg, "--")) {
      options = false;
      continue;
    }
    if (options && (!strcmp(arg, "-h") || !strcmp(arg, "--help"))) {
      puts("usage: check-flake-lock-update [-h] base head\n\nValidate lock "
           "data without evaluating flake inputs.");
      return 0;
    }
    if (options && arg[0] == '-' && arg[1]) {
      bad = true;
      continue;
    }
    if (!base_path)
      base_path = arg;
    else if (!head_path)
      head_path = arg;
    else
      bad = true;
  }
  if (bad || !base_path || !head_path)
    return usage();
  yyjson_doc *base = load_json(base_path), *head = load_json(head_path);
  GHashTable *approved = owners();
  GPtrArray *changed = g_ptr_array_new();
  int status = 1;
  Val *a = base ? yyjson_doc_get_root(base) : NULL,
      *b = head ? yyjson_doc_get_root(head) : NULL;
  if (!require(base && head, NULL, "invalid or unreadable JSON") ||
      !require(unique(a, 0) && unique(b, 0), NULL,
               "duplicate JSON key or excessive nesting") ||
      !validate(a) || !validate(b))
    goto done;
  if (!require(same(field(a, "root"), field(b, "root")), NULL,
               "root node changed"))
    goto done;
  Val *old_nodes = field(a, "nodes"), *new_nodes = field(b, "nodes"), *name,
      *before;
  size_t i, n;
  if (!require(yyjson_obj_size(old_nodes) == yyjson_obj_size(new_nodes), NULL,
               "nodes were added or removed"))
    goto done;
  yyjson_obj_foreach(old_nodes, i, n, name, before) {
    Val *after =
        yyjson_obj_getn(new_nodes, yyjson_get_str(name), yyjson_get_len(name));
    const char *label = yyjson_get_str(name);
    if (!require(after != NULL, NULL, "nodes were added or removed"))
      goto done;
    const char *locked[] = {"locked"};
    if (!require(excluding(before, after, locked, 1), label,
                 "input graph or source metadata changed"))
      goto done;
    Val *old = field(before, "locked"), *next = field(after, "locked");
    if (same(old, next))
      continue;
    if (!require(text_is(field(old, "type"), "github") &&
                     text_is(field(next, "type"), "github"),
                 label, "changed non-GitHub source"))
      goto done;
    const char *mutable[] = {"rev", "narHash", "lastModified"};
    if (!require(excluding(old, next, mutable, 3), label,
                 "locked source changed"))
      goto done;
    const char *allowed[] = {"rev",   "narHash", "lastModified", "type",
                             "owner", "repo",    "host",         "dir"};
    if (!require(allowed_keys(next, allowed, G_N_ELEMENTS(allowed)), label,
                 "unsupported GitHub attributes"))
      goto done;
    Val *host = field(next, "host");
    if (!require(!host || text_is(host, "github.com"), label,
                 "unapproved GitHub host") ||
        !require(g_hash_table_contains(approved,
                                       yyjson_get_str(field(next, "owner"))),
                 label, "GitHub owner is not auto-mergeable"))
      goto done;
    g_ptr_array_add(changed, (void *)label);
  }
  g_ptr_array_sort(changed, compare);
  if (!changed->len)
    puts("No changed lock nodes found.");
  else {
    fputs("Auto-merge allowed for changed lock node(s): ", stdout);
    for (size_t j = 0; j < changed->len; j++)
      printf("%s%s", j ? ", " : "", (char *)changed->pdata[j]);
    putchar('\n');
  }
  status = 0;
done:
  if (status)
    fprintf(stderr, "Manual review required: %s.\n",
            failure ? failure : "invalid lock data");
  g_free(failure);
  g_ptr_array_free(changed, TRUE);
  g_hash_table_destroy(approved);
  if (base)
    yyjson_doc_free(base);
  if (head)
    yyjson_doc_free(head);
  return status;
}
