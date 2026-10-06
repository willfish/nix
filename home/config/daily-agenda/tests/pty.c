#define _DEFAULT_SOURCE
#include <errno.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  int fd;
  pid_t child = forkpty(&fd, NULL, NULL, NULL);
  if (child < 0)
    return 1;
  if (!child) {
    execv(argv[1], argv + 1);
    _exit(127);
  }
  char buf[8192];
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0)
    if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
      return 1;
  close(fd);
  int status;
  while (waitpid(child, &status, 0) < 0)
    if (errno != EINTR)
      return 1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
