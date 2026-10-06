#include "focus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static yyjson_doc *input(void) {
  GString *s = g_string_new(NULL);
  char bytes[8192];
  size_t n;
  while ((n = fread(bytes, 1, sizeof bytes, stdin)))
    g_string_append_len(s, bytes, (gssize)n);
  yyjson_doc *doc =
      yyjson_read(s->str, s->len,
                  YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_BIGNUM_AS_RAW);
  g_string_free(s, true);
  return doc;
}
static Doc *list(GPtrArray *a, bool numbers) {
  Doc *d = yyjson_mut_doc_new(NULL);
  Mut *root = yyjson_mut_arr(d);
  yyjson_mut_doc_set_root(d, root);
  for (size_t i = 0; i < a->len; i++)
    yyjson_mut_arr_add_val(root, numbers ? yyjson_mut_rawcpy(d, a->pdata[i])
                                         : yyjson_mut_strcpy(d, a->pdata[i]));
  g_ptr_array_free(a, true);
  return d;
}
int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  Doc *out = NULL;
  yyjson_doc *raw = NULL;
  const char *mode = argv[1];
  if (!strcmp(mode, "parse") && argc >= 4)
    out = parse_toast(argv[2], argv[3], argc > 4 ? argv[4] : "");
  else if (!strcmp(mode, "paths"))
    out = list(socket_paths(), false);
  else if (!strcmp(mode, "parent") && argc == 4) {
    char *p = parent_pid(argv[2], argv[3]);
    out = empty_object();
    yyjson_mut_doc_set_root(out, yyjson_mut_rawcpy(out, p));
    g_free(p);
  } else if (!strcmp(mode, "ancestors") && argc == 4)
    out = list(ancestor_pids(argv[2], argv[3]), true);
  else if (!strcmp(mode, "attached") && argc == 4)
    out = list(attached_pids(argv[2], argv[3]), true);
  else if (!strcmp(mode, "cache") && argc == 6)
    out = recall(argv[2], argv[3], argv[4], argv[5]);
  else {
    raw = input();
    if (!raw)
      return 2;
    Val *root = yyjson_doc_get_root(raw);
    if (!strcmp(mode, "resolve"))
      out = resolve_target(field(root, "snapshot"), field(root, "toast"));
    else if (!strcmp(mode, "alive")) {
      out = empty_object();
      yyjson_mut_doc_set_root(
          out, yyjson_mut_bool(out, target_alive(field(root, "snapshot"),
                                                 field(root, "target"))));
    } else if (!strcmp(mode, "payload"))
      out = focus_payload(root);
    else if (!strcmp(mode, "remember") && argc == 6) {
      if (!remember(argv[2], argv[3], argv[4], root, argv[5]))
        focus_error = true;
      out = empty_object();
    } else if (!strcmp(mode, "choose")) {
      GPtrArray *ancestors = g_ptr_array_new_with_free_func(g_free);
      Val *a;
      size_t i, max;
      yyjson_arr_foreach(field(root, "ancestors"), i, max, a)
          g_ptr_array_add(ancestors, integer(a));
      char *pid = integer(field(root, "pid")),
           *label = string(field(root, "label"));
      Val *v = choose_window(field(root, "clients"), pid ? pid : "0", ancestors,
                             label);
      out = yyjson_mut_doc_new(NULL);
      yyjson_mut_doc_set_root(out, v ? yyjson_val_mut_copy(out, v)
                                     : yyjson_mut_null(out));
      g_free(pid);
      g_free(label);
      g_ptr_array_free(ancestors, true);
    } else
      return 2;
  }
  if (!out) {
    out = empty_object();
    yyjson_mut_doc_set_root(out, yyjson_mut_null(out));
  }
  char *s = json(out);
  if (s)
    puts(s);
  g_free(s);
  yyjson_mut_doc_free(out);
  if (raw)
    yyjson_doc_free(raw);
  return focus_error ? 1 : 0;
}
