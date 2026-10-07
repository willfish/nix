#include "tools.h"
#include <curl/curl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>
int main(int argc, char **argv) {
  char *error = NULL;
  bool ok = false;
  if (argc == 2 && !strcmp(argv[1], "assets")) {
    printf("{\"base\":\"%s\",\"idle_ms\":60000,\"assets\":[", pp_base_url);
    for (size_t i = 0; i < pp_assets_count; i++) {
      if (i)
        putchar(',');
      printf("{\"name\":\"%s\",\"bytes\":%" PRIu64 ",\"sha256\":\"%s\"}",
             pp_assets[i].name, pp_assets[i].bytes, pp_assets[i].sha256);
    }
    puts("]}");
    return 0;
  } else if (argc == 3 && !strcmp(argv[1], "token")) {
    char *value = pp_token(&error);
    if (value) {
      ok = !strcmp(value, argv[2]);
      puts(ok ? "match" : "mismatch");
      g_free(value);
    }
  } else if (argc >= 2 && !strcmp(argv[1], "patch")) {
    GString *input = g_string_new(NULL);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)))
      g_string_append_len(input, buf, (gssize)n);
    char *out = pp_patch(input->str, argc == 3 ? argv[2] : NULL, &error);
    g_string_free(input, true);
    if (out) {
      fputs(out, stdout);
      g_free(out);
      ok = true;
    }
  } else if (argc == 4 && !strcmp(argv[1], "extract"))
    ok = pp_extract(argv[2], argv[3], &error);
  else if (argc == 7 && !strcmp(argv[1], "install")) {
    yyjson_doc *doc = yyjson_read_file(argv[3], 0, NULL, NULL);
    yyjson_val *list = yyjson_doc_get_root(doc);
    size_t count = yyjson_arr_size(list);
    Asset *assets = g_new0(Asset, count);
    size_t i, n;
    yyjson_val *value;
    yyjson_arr_foreach(list, i, n, value) {
      assets[i].name = yyjson_get_str(yyjson_obj_get(value, "name"));
      assets[i].sha256 = yyjson_get_str(yyjson_obj_get(value, "sha256"));
      assets[i].bytes = yyjson_get_uint(yyjson_obj_get(value, "bytes"));
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
      ok = pp_install(argv[2], assets, count, atoi(argv[5]) != 0, argv[4],
                      strtol(argv[6], NULL, 10), &error);
      curl_global_cleanup();
    }
    g_free(assets);
    yyjson_doc_free(doc);
  } else
    return 2;
  if (!ok && error)
    fprintf(stderr, "%s\n", error);
  g_free(error);
  return ok ? 0 : 1;
}
