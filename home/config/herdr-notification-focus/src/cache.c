#define _DEFAULT_SOURCE
#include "focus.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
char *cache_path(void) {
  const char *state = g_getenv("XDG_STATE_HOME");
  char *home = NULL, *base;
  if (state && *state)
    base = normal_path(state);
  else {
    home = home_path();
    base = g_build_filename(home, ".local/state", NULL);
  }
  char *path = g_build_filename(base, "herdr-notification-focus/targets.json",
                                NULL),
       *normal = normal_path(path);
  g_free(home);
  g_free(base);
  g_free(path);
  return normal;
}
static Doc *cache_read(const char *path) {
  yyjson_doc *input = read_json(path, true);
  Val *root = input ? yyjson_doc_get_root(input) : NULL;
  Doc *out;
  if (yyjson_is_obj(root) && yyjson_is_arr(field(root, "entries")))
    out = yyjson_doc_mut_copy(input, NULL);
  else {
    out = empty_object();
    yyjson_mut_obj_add_arr(out, yyjson_mut_doc_get_root(out), "entries");
  }
  if (input)
    yyjson_doc_free(input);
  return out;
}
static char *cache_key(const char *summary, const char *body, const char *ts) {
  return g_strconcat(ts, "|", summary, "|", body, NULL);
}
Doc *recall(const char *summary, const char *body, const char *ts,
            const char *path) {
  Doc *cache = cache_read(path);
  yyjson_doc *raw = yyjson_mut_doc_imut_copy(cache, NULL);
  Val *items = field(yyjson_doc_get_root(raw), "entries");
  char *key = cache_key(summary, body, ts);
  Doc *out = NULL;
  for (size_t i = yyjson_arr_size(items); i > 0; i--) {
    Val *v = yyjson_arr_get(items, i - 1), *k = field(v, "key");
    if (!yyjson_is_obj(v)) {
      focus_error = true;
      break;
    }
    if (yyjson_is_str(k) && yyjson_get_len(k) == strlen(key) &&
        !memcmp(yyjson_get_str(k), key, strlen(key))) {
      out = yyjson_mut_doc_new(NULL);
      yyjson_mut_doc_set_root(out, yyjson_val_mut_copy(out, v));
      break;
    }
  }
  g_free(key);
  yyjson_doc_free(raw);
  yyjson_mut_doc_free(cache);
  return out;
}
static bool write_cache(const char *path, Doc *doc) {
  char *parent = g_path_get_dirname(path);
  bool ok = g_mkdir_with_parents(parent, 0700) == 0;
  char *dot = strrchr(path, '.'), *slash = strrchr(path, '/'),
       *stem = dot && (!slash || dot > slash)
                   ? g_strndup(path, (gsize)(dot - path))
                   : g_strdup(path),
       *temporary = g_strconcat(stem, ".tmp", NULL), *s = json(doc);
  FILE *file = ok && s ? fopen(temporary, "w") : NULL;
  if (!file)
    ok = false;
  else {
    ok = fwrite(s, 1, strlen(s), file) == strlen(s);
    if (fclose(file))
      ok = false;
    if (ok && chmod(temporary, 0600))
      ok = false;
    if (ok && rename(temporary, path))
      ok = false;
  }
  g_free(parent);
  g_free(stem);
  g_free(temporary);
  g_free(s);
  if (!ok)
    focus_error = true;
  return ok;
}
bool remember(const char *summary, const char *body, const char *ts,
              Val *target, const char *path) {
  if (!truth(target))
    return true;
  Doc *cache = cache_read(path);
  yyjson_doc *raw = yyjson_mut_doc_imut_copy(cache, NULL);
  Val *root = yyjson_doc_get_root(raw), *entries = field(root, "entries"), *v;
  GPtrArray *kept = g_ptr_array_new();
  char *key = cache_key(summary, body, ts);
  size_t i, max;
  yyjson_arr_foreach(entries, i, max, v) {
    if (!yyjson_is_obj(v)) {
      focus_error = true;
      break;
    }
    Val *k = field(v, "key");
    if (!yyjson_is_str(k) || yyjson_get_len(k) != strlen(key) ||
        memcmp(yyjson_get_str(k), key, strlen(key)))
      g_ptr_array_add(kept, v);
  }
  Mut *array = yyjson_mut_arr(cache);
  size_t first = kept->len > 99 ? kept->len - 99 : 0;
  for (size_t j = first; j < kept->len; j++)
    yyjson_mut_arr_add_val(array, yyjson_val_mut_copy(cache, kept->pdata[j]));
  Mut *entry = yyjson_mut_obj(cache);
  yyjson_mut_obj_add_strcpy(cache, entry, "key", key);
  yyjson_obj_iter it = yyjson_obj_iter_with(target);
  Val *k;
  while ((k = yyjson_obj_iter_next(&it)))
    yyjson_mut_obj_add(entry, yyjson_val_mut_copy(cache, k),
                       yyjson_val_mut_copy(cache, yyjson_obj_iter_get_val(k)));
  yyjson_mut_arr_add_val(array, entry);
  yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(cache), "entries");
  yyjson_mut_obj_add_val(cache, yyjson_mut_doc_get_root(cache), "entries",
                         array);
  bool ok = !focus_error && write_cache(path, cache);
  g_free(key);
  g_ptr_array_free(kept, true);
  yyjson_doc_free(raw);
  yyjson_mut_doc_free(cache);
  return ok;
}
