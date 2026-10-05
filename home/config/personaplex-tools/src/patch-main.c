#include "tools.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc != 3) {
    fputs("usage: personaplex-patch SOURCE BROWSER-GUARD\n", stderr);
    return 2;
  }
  char *source = NULL, *error = NULL;
  gsize size;
  if (!g_file_get_contents(argv[1], &source, &size, NULL) ||
      !g_utf8_validate(source, (gssize)size, NULL)) {
    fputs("Could not read PersonaPlex server source\n", stderr);
    g_free(source);
    return 1;
  }
  char *patched = pp_patch(source, argv[2], &error);
  g_free(source);
  if (!patched) {
    fprintf(stderr, "%s\n", error);
    g_free(error);
    return 1;
  }
  FILE *file = fopen(argv[1], "w");
  bool ok =
      file && fwrite(patched, 1, strlen(patched), file) == strlen(patched);
  if (file && fclose(file))
    ok = false;
  g_free(patched);
  if (!ok)
    fputs("Could not write PersonaPlex server source\n", stderr);
  return ok ? 0 : 1;
}
