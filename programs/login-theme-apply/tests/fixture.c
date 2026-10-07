#define _POSIX_C_SOURCE 200809L
#include "greeter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char *bytes(const char *hex, size_t *length) {
  size_t n = strlen(hex);
  if (n % 2)
    return NULL;
  char *out = g_malloc(n / 2 + 1);
  for (size_t i = 0; i < n; i += 2) {
    if (!g_ascii_isxdigit(hex[i]) || !g_ascii_isxdigit(hex[i + 1])) {
      g_free(out);
      return NULL;
    }
    out[i / 2] = (char)(g_ascii_xdigit_value(hex[i]) * 16 +
                        g_ascii_xdigit_value(hex[i + 1]));
  }
  out[n / 2] = 0;
  *length = n / 2;
  return out;
}
int main(int argc, char **argv) {
  if (argc == 3 && (!strcmp(argv[1], "id") || !strcmp(argv[1], "store"))) {
    size_t n;
    char *data = bytes(argv[2], &n);
    if (!data)
      return 2;
    bool valid = !strcmp(argv[1], "id") ? greeter_id((unsigned char *)data, n)
                                        : greeter_store(data, n);
    puts(valid ? "true" : "false");
    g_free(data);
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "read")) {
    char id[65];
    int result = greeter_read(argv[2], argv[3], id);
    if (result < 0)
      return 1;
    puts(result ? id : "null");
    return 0;
  }
  if ((argc == 4 || argc == 5) && !strcmp(argv[1], "raw")) {
    size_t n;
    char *path = bytes(argv[2], &n);
    if (!path || memchr(path, 0, n)) {
      g_free(path);
      return 2;
    }
    char *args[] = {argv[3], argc == 5 ? argv[4] : path,
                    argc == 5 ? path : NULL, NULL};
    execvp(args[0], args);
    g_free(path);
    return 1;
  }
  return 2;
}
