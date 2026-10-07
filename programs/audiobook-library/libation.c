#include "common.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("usage: libation-inventory [FileLocationsV2.json]\nPrint indexed "
         "media paths that still exist; never read account credentials.");
    return 0;
  }
  if (argc > 2)
    return 2;
  const char *home = g_getenv("HOME");
  char *path =
      argc == 2 ? g_strdup(argv[1])
                : g_build_filename(home ? home : g_get_home_dir(),
                                   ".local/share/Libation/FileLocationsV2.json",
                                   NULL);
  yyjson_doc *doc = load_json(path);
  g_free(path);
  if (!doc) {
    fputs("Libation inventory could not be read.\n", stderr);
    return 1;
  }
  Val *root = yyjson_doc_get_root(doc), *dictionary = field(root, "Dictionary");
  bool ok = yyjson_is_obj(root) && (!dictionary || yyjson_is_obj(dictionary));
  Val *key, *entries;
  size_t i, n;
  if (ok)
    yyjson_obj_foreach(dictionary, i, n, key, entries) {
      if (!yyjson_is_arr(entries)) {
        ok = false;
        break;
      }
      Val *entry;
      size_t j, m;
      yyjson_arr_foreach(entries, j, m, entry) {
        if (!yyjson_is_obj(entry)) {
          ok = false;
          break;
        }
        Val *value = field(entry, "Path");
        if (yyjson_is_obj(value))
          value = field(value, "Path");
        if (!value || yyjson_is_null(value) || yyjson_is_false(value) ||
            (yyjson_is_num(value) && yyjson_get_num(value) == 0) ||
            (yyjson_is_arr(value) && !yyjson_arr_size(value)) ||
            (yyjson_is_str(value) && !yyjson_get_len(value)))
          continue;
        if (!yyjson_is_str(value)) {
          ok = false;
          break;
        }
        const char *s = yyjson_get_str(value);
        struct stat st;
        if (strlen(s) == yyjson_get_len(value) && stat(s, &st) == 0 &&
            puts(s) == EOF)
          ok = false;
      }
      if (!ok)
        break;
    }
  yyjson_doc_free(doc);
  if (!ok)
    fputs("Libation inventory is invalid.\n", stderr);
  return ok ? 0 : 1;
}
