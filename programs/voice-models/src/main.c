#include "models.h"
#include <curl/curl.h>
#include <getopt.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

static void usage(FILE *output) {
  fputs("usage: voice-model-setup [--data-dir DIR] [--check-only] "
        "[--experimental] [--stt-only] [--host HOST]\n",
        output);
}
int main(int argc, char **argv) {
  const char *root_arg = NULL, *host = g_get_host_name();
  bool check_only = false, experimental = false, stt_only = false;
  const struct option options[] = {{"data-dir", required_argument, NULL, 'd'},
                                   {"check-only", no_argument, NULL, 'c'},
                                   {"experimental", no_argument, NULL, 'e'},
                                   {"stt-only", no_argument, NULL, 's'},
                                   {"host", required_argument, NULL, 'H'},
                                   {"help", no_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
  int option;
  while ((option = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    switch (option) {
    case 'd':
      root_arg = optarg;
      break;
    case 'c':
      check_only = true;
      break;
    case 'e':
      experimental = true;
      break;
    case 's':
      stt_only = true;
      break;
    case 'H':
      host = optarg;
      break;
    case 'h':
      usage(stdout);
      return 0;
    default:
      return 2;
    }
  }
  if (optind != argc) {
    usage(stderr);
    return 2;
  }
  char *base = NULL;
  const char *xdg = g_getenv("XDG_DATA_HOME");
  if (!xdg) {
    base = g_build_filename(g_get_home_dir(), ".local/share", NULL);
    xdg = base;
  }
  char *root =
      root_arg ? g_strdup(root_arg) : g_build_filename(xdg, "pi-voice", NULL);
  int result = 0;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    result = 1;
  else {
    for (size_t i = 0; i < assets_count; i++) {
      if (selected(&assets[i], host, stt_only, experimental) &&
          install_model(root, &assets[i], check_only, &model_policy)) {
        result = 1;
        break;
      }
    }
    curl_global_cleanup();
  }
  g_free(root);
  g_free(base);
  return result;
}
