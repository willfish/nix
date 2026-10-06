#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc != 4 && argc != 5)
    return 2;
  const char *mode = argv[1];
  char *path = argv[2], *allocated = NULL;
  char *payload = "{\"x\":\"\xff\"}";
  if (!strcmp(mode, "surrogate"))
    payload = "{\"x\":\"\xed\xa0\x80\"}";
  else if (!strcmp(mode, "bad-destination")) {
    size_t n = strlen(path) + 32;
    allocated = malloc(n);
    if (!allocated)
      return 1;
    snprintf(allocated, n, "%s-\xff/settings.json", path);
    path = allocated;
    payload = "{\"x\":1}";
  } else if (strcmp(mode, "bad-byte"))
    return 2;
  char *args[] = {argv[3], argc == 5 ? argv[4] : path,
                  argc == 5 ? path : payload, argc == 5 ? payload : NULL, NULL};
  execvp(args[0], args);
  free(allocated);
  return 1;
}
