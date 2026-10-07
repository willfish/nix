#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc >= 6 && !strcmp(argv[1], "run"))
    return theme_run(argc - 4, argv + 4, argv[2], argv[3], atoi(argv[4]));
  if (argc == 6 && !strcmp(argv[1], "selection-text")) {
    puts(selection_text(argv[2], argv[3], argv[4], argv[5]));
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "variables")) {
    GString *s = g_string_new(NULL);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)))
      g_string_append_len(s, buf, (gssize)n);
    GHashTable *values = theme_variables(s->str);
    g_string_free(s, true);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, o);
    GHashTableIter it;
    void *key, *value;
    g_hash_table_iter_init(&it, values);
    while (g_hash_table_iter_next(&it, &key, &value))
      yyjson_mut_obj_add_str(doc, o, key, value);
    char *out = yyjson_mut_write(doc, 0, NULL);
    puts(out);
    free(out);
    yyjson_mut_doc_free(doc);
    g_hash_table_destroy(values);
    return 0;
  }
  return 2;
}
