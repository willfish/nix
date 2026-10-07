#include "watch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  GString *input = g_string_new(NULL);
  char data[4096];
  size_t len;
  while ((len = fread(data, 1, sizeof(data), stdin)))
    g_string_append_len(input, data, len);
  yyjson_doc *request =
      yyjson_read(input->str, input->len, YYJSON_READ_ALLOW_INF_AND_NAN);
  g_string_free(input, true);
  if (!request)
    return 2;
  yyjson_val *root = yyjson_doc_get_root(request);
  yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *value = NULL;
  if (!strcmp(argv[1], "url")) {
    char *url = html_url(yyjson_get_str(root));
    value = yyjson_mut_strcpy(out, url);
    g_free(url);
  } else if (!strcmp(argv[1], "plan")) {
    yyjson_mut_doc_free(out);
    out = plan(field(root, "items"), field(root, "state"));
    value = yyjson_mut_doc_get_root(out);
  } else if (!strcmp(argv[1], "command")) {
    char **args = notify_command(root);
    value = yyjson_mut_arr(out);
    if (!args) {
      yyjson_doc_free(request);
      yyjson_mut_doc_free(out);
      return 1;
    }
    for (size_t i = 0; args[i]; i++)
      yyjson_mut_arr_append(value, yyjson_mut_strcpy(out, args[i]));
    g_strfreev(args);
  } else if (!strcmp(argv[1], "open")) {
    value =
        yyjson_mut_bool(out, open_chosen(yyjson_get_str(field(root, "output")),
                                         yyjson_get_str(field(root, "url"))));
  } else if (!strcmp(argv[1], "capture")) {
    yyjson_val *arg;
    size_t i, n;
    GPtrArray *args = g_ptr_array_new();
    yyjson_arr_foreach(field(root, "argv"), i, n, arg)
        g_ptr_array_add(args, (gpointer)yyjson_get_str(arg));
    g_ptr_array_add(args, NULL);
    Capture *c =
        capture_wait((const char *const *)args->pdata,
                     (guint)yyjson_get_uint(field(root, "timeout_ms")));
    value = yyjson_mut_obj(out);
    yyjson_mut_obj_add_bool(out, value, "started", c != NULL);
    yyjson_mut_obj_add_bool(out, value, "ok", c && c->ok);
    yyjson_mut_obj_add_bool(out, value, "timed_out", c && c->timed_out);
    yyjson_mut_obj_add_strcpy(out, value, "output",
                              c && c->output ? c->output : "");
    capture_free(c);
    g_ptr_array_free(args, true);
  } else {
    yyjson_doc_free(request);
    yyjson_mut_doc_free(out);
    return 2;
  }
  if (!out || !value) {
    yyjson_doc_free(request);
    yyjson_mut_doc_free(out);
    return 1;
  }
  yyjson_mut_doc_set_root(out, value);
  char *serialized = yyjson_mut_write(out, YYJSON_WRITE_ESCAPE_UNICODE, NULL);
  int result = serialized && puts(serialized) >= 0 ? 0 : 1;
  free(serialized);
  yyjson_doc_free(request);
  yyjson_mut_doc_free(out);
  return result;
}
