#include "usage.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  const char *id = strstr(argv[0], "codex")      ? "codex"
                   : strstr(argv[0], "grok")     ? "grok"
                   : strstr(argv[0], "opencode") ? "opencode"
                                                 : NULL;
  if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
    if (id)
      fprintf(stderr,
              "usage: omarchy-agent-usage-%s [--force] [--limits-only]\n", id);
    else
      fputs("usage: hypr-agent-status\n", stderr);
    return 0;
  }
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  Context ctx = {.now = t.tv_sec + t.tv_nsec / 1000000000.0,
                 .transport = http_request,
                 .real_time = true};
  curl_global_init(CURL_GLOBAL_DEFAULT);
  Doc *d = yyjson_mut_doc_new(NULL);
  Val *v = id ? collect(d, &ctx, id) : waybar(d, status_records(d));
  char *output = encode(v);
  bool ok = output != NULL;
  if (output) {
    puts(output);
    free(output);
  } else
    fputs("agent-usage: could not encode record\n", stderr);
  yyjson_mut_doc_free(d);
  curl_global_cleanup();
  return ok ? 0 : 1;
}
