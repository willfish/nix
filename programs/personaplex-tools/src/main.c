#include "tools.h"
#include <curl/curl.h>
#include <getopt.h>
#include <stdio.h>
int main(int argc, char **argv) {
  const char *root = NULL;
  bool check = false;
  int option;
  const struct option options[] = {{"data-dir", required_argument, NULL, 'd'},
                                   {"check-only", no_argument, NULL, 'c'},
                                   {"help", no_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
  while ((option = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    if (option == 'd')
      root = optarg;
    else if (option == 'c')
      check = true;
    else if (option == 'h') {
      puts("usage: personaplex-models --data-dir DIR [--check-only]");
      return 0;
    } else
      return 2;
  }
  if (!root || optind != argc) {
    fputs("usage: personaplex-models --data-dir DIR [--check-only]\n", stderr);
    return 2;
  }
  char *error = NULL;
  bool ok = false;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
    ok = pp_install(*root ? root : ".", pp_assets, pp_assets_count, check,
                    pp_base_url, 60000, &error);
    curl_global_cleanup();
  }
  if (!ok)
    fprintf(stderr, "PersonaPlex setup: %s\n",
            error ? error : "Could not initialize model download");
  g_free(error);
  return ok ? 0 : 1;
}
