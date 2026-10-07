#include "managed.h"
#include <getopt.h>
#include <stdio.h>
#include <string.h>
static int usage(void) {
  fputs("usage: hermes-declaration DECLARATION HOME [--qwen-overlay FILE "
        "--key-file FILE]\n       hermes-profile PROFILE OVERLAY [--key-file "
        "FILE]\n       hermes-telegram-route CONFIG [--house FILE]\n",
        stderr);
  return 2;
}
int main(int argc, char **argv) {
  const char *overlay = NULL, *key = NULL, *house = NULL;
  const struct option options[] = {
      {"qwen-overlay", required_argument, NULL, 'q'},
      {"key-file", required_argument, NULL, 'k'},
      {"house", required_argument, NULL, 'H'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  opterr = 0;
  int option;
  while ((option = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    if (option == 'h') {
      puts("Hermes managed configuration helper");
      return 0;
    }
    if (option == 'q' && MODE == 0)
      overlay = optarg;
    else if (option == 'k' && MODE != 2)
      key = optarg;
    else if (option == 'H' && MODE == 2)
      house = optarg;
    else
      return usage();
  }
  if (argc - optind != (MODE == 2 ? 1 : 2))
    return usage();
  arena_start();
  bool changed = false;
  char *first = path_normal(argv[optind]),
       *second = MODE == 2 ? NULL : path_normal(argv[optind + 1]);
  if (MODE == 0)
    apply_declaration(first, second, overlay, key);
  else if (MODE == 1)
    changed = apply_profile(first, second, key);
  else
    apply_routes(first, house);
  int status = failed ? 1 : 0;
  if (failed)
    fputs("Hermes configuration failed; secret values have not been logged\n",
          stderr);
  else if (MODE == 1 && changed)
    puts("Updated Qwen profile; previous config backed up if present.");
  g_free(first);
  g_free(second);
  arena_end();
  return status;
}
