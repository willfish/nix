#define _DEFAULT_SOURCE
#include "agenda.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
static yyjson_doc *input(void) {
  char buffer[MAX_BYTES + 1];
  size_t n = fread(buffer, 1, sizeof buffer, stdin);
  return yyjson_read(buffer, n, YYJSON_READ_ALLOW_INF_AND_NAN);
}
int main(int argc, char **argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  if (argc < 2)
    return 2;
  yyjson_mut_doc *out = NULL;
  yyjson_doc *doc = NULL;
  int status = 1;
  if (!strcmp(argv[1], "cli") || g_str_has_prefix(argv[1], "--parse-")) {
    const char *child = NULL;
    if (!strcmp(argv[1], "cli")) {
      argv++;
      argc--;
      child = g_getenv("DAILY_AGENDA_CHILD");
    }
    status = agenda_run(argc, argv, child);
    goto done;
  }
  if (!strcmp(argv[1], "feeds") && argc == 3)
    out = feeds(argv[2]);
  else if (!strcmp(argv[1], "url") && argc == 3)
    status = feed_url(argv[2], NULL) ? 0 : 1;
  else if (!strcmp(argv[1], "notes") && argc == 3) {
    const char *warning = NULL;
    GPtrArray *rows = reminders(argv[2], &warning);
    if (rows) {
      out = yyjson_mut_doc_new(NULL);
      yyjson_mut_val *root = yyjson_mut_obj(out), *array = yyjson_mut_arr(out);
      yyjson_mut_doc_set_root(out, root);
      for (size_t i = 0; i < rows->len; i++)
        yyjson_mut_arr_add_strcpy(out, array, rows->pdata[i]);
      yyjson_mut_obj_add_val(out, root, "rows", array);
      yyjson_mut_obj_add_val(out, root, "warning",
                             warning ? yyjson_mut_str(out, warning)
                                     : yyjson_mut_null(out));
      g_ptr_array_free(rows, true);
    }
  } else if (!strcmp(argv[1], "bar") && argc == 5) {
    doc = input();
    GTimeZone *z = zone(argv[3]);
    GPtrArray *secrets = feed_secrets(argv[4]);
    if (doc && z && secrets)
      out = bar_rows(yyjson_doc_get_root(doc), argv[2], z, secrets);
    if (z)
      g_time_zone_unref(z);
    if (secrets)
      g_ptr_array_free(secrets, true);
  } else if (!strcmp(argv[1], "save") && argc == 3) {
    doc = input();
    out = doc ? yyjson_doc_mut_copy(doc, NULL) : NULL;
    status = out && save_cache(argv[2], out) ? 0 : 1;
    yyjson_mut_doc_free(out);
    out = NULL;
  } else if (!strcmp(argv[1], "expand") && argc == 6) {
    GTimeZone *z = zone(argv[5]);
    GDateTime *a = z ? parse_day(argv[3], z) : NULL,
              *b = z ? parse_day(argv[4], z) : NULL;
    GByteArray *bytes = g_byte_array_new();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, stdin)))
      g_byte_array_append(bytes, (const guchar *)buf, (guint)n);
    GBytes *raw = g_byte_array_free_to_bytes(bytes);
    if (a && b)
      out = expand(raw, argv[2], a, b, argv[5], true);
    g_bytes_unref(raw);
    if (a)
      g_date_time_unref(a);
    if (b)
      g_date_time_unref(b);
    if (z)
      g_time_zone_unref(z);
  }
  if (out) {
    char *s = json_string(out, true);
    if (s) {
      puts(s);
      g_free(s);
      status = 0;
    }
    yyjson_mut_doc_free(out);
  }
done:
  if (doc)
    yyjson_doc_free(doc);
  curl_global_cleanup();
  icaltimezone_free_builtin_timezones();
  return status;
}
