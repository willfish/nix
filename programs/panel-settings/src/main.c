#define _GNU_SOURCE
#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *parent_path(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash)
    return g_strdup("");
  size_t n = (size_t)(slash - path) + 1;
  bool all = true;
  for (size_t i = 0; i < n; i++)
    if (path[i] != '/')
      all = false;
  if (!all)
    while (n && path[n - 1] == '/')
      n--;
  return g_strndup(path, n);
}
static bool make_parent(const char *path, mode_t mode) {
  if (mkdir(path, mode) == 0)
    return true;
  int saved = errno;
  struct stat info;
  if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
    return true;
  if (saved != ENOENT || !*path)
    return false;
  char *head = parent_path(path);
  bool ok = *head && strcmp(head, path) && make_parent(head, 0777);
  g_free(head);
  if (!ok)
    return false;
  if (mkdir(path, mode) == 0)
    return true;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
static bool publish(const char *path, Json *value) {
  char *parent = parent_path(path), *temporary = NULL, *encoded = NULL;
  int fd = -1;
  bool ok = false, created = false;
  if (!make_parent(parent, 0700))
    goto done;
  temporary = g_strconcat(parent, "/.", PANEL_KIND, "-settings-XXXXXX", NULL);
  fd = g_mkstemp_full(temporary, O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0)
    goto done;
  created = true;
  // Keep parent creation ahead of encoding, without changing the destination.
  encoded = settings_encode(value);
  if (!encoded)
    goto done;
  for (size_t at = 0, n = strlen(encoded); at < n;) {
    ssize_t sent = write(fd, encoded + at, n - at);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      goto done;
    at += (size_t)sent;
  }
  if (fchmod(fd, 0600) < 0)
    goto done;
  if (close(fd) < 0) {
    fd = -1;
    goto done;
  }
  fd = -1;
  if (rename(temporary, path) < 0)
    goto done;
  ok = true;
done:
  if (fd >= 0)
    close(fd);
  if (created && !ok)
    unlink(temporary);
  g_free(parent);
  g_free(temporary);
  g_free(encoded);
  return ok;
}
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  bool too_long;
  char *input = settings_input(argv[2], &too_long);
  if (too_long)
    return 2;
  Json *value = input ? settings_parse(input) : NULL;
  g_free(input);
  int status = 1;
  if (value) {
    if (!settings_object(value))
      status = 2;
    else if (publish(argv[1], value))
      status = 0;
  }
  settings_free(value);
  if (status == 1)
    fputs("Panel settings could not be saved.\n", stderr);
  return status;
}
