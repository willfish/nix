#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  mode_t mask = umask(0);
  umask(mask);
  const char *base = getenv("ANTHROPIC_BASE_URL");
  FILE *file = fopen(argv[1], "w");
  if (!file)
    return 1;
  if (fprintf(file, "{\"base\":\"%s\",\"umask\":%u}\n", base ? base : "",
              (unsigned)mask) < 0 ||
      fclose(file))
    return 1;
  for (;;)
    pause();
}
