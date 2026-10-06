#define _POSIX_C_SOURCE 200809L
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc != 3 && argc != 4)
    return 2;
  size_t length = strlen(argv[1]);
  if (length % 2)
    return 2;
  char *data = g_malloc(length / 2 + 1);
  for (size_t i = 0; i < length; i += 2) {
    int a = g_ascii_xdigit_value(argv[1][i]),
        b = g_ascii_xdigit_value(argv[1][i + 1]);
    if (a < 0 || b < 0 || !(a || b)) {
      g_free(data);
      return 2;
    }
    data[i / 2] = (char)((a << 4) | b);
  }
  data[length / 2] = 0;
  int status = setenv("PI_AGENT_BUS_URL", data, 1);
  g_free(data);
  if (status != 0)
    return 1;
  char *args[] = {argv[2], argc == 4 ? argv[3] : NULL, NULL};
  execvp(args[0], args);
  return 1;
}
