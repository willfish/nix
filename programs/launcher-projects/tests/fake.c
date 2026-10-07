#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

static void write_all(int fd, const void *data, size_t n) {
  const char *p = data;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      _exit(1);
    }
    p += (size_t)w;
    n -= (size_t)w;
  }
}

static bool read_ids(int *pgrp, int *sid) {
  char buf[1024];
  int fd = open("/proc/self/stat", O_RDONLY);
  if (fd < 0)
    return false;
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0)
    return false;
  buf[n] = 0;
  char *rest = strstr(buf, ") ");
  if (!rest)
    return false;
  int pg = 0, se = 0;
  if (sscanf(rest + 2, "%*s %*d %d %d", &pg, &se) != 2)
    return false;
  *pgrp = pg;
  *sid = se;
  return true;
}

static char *fd_target(int fd) {
  char link[64], buf[512];
  int wrote = snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
  if (wrote < 0 || (size_t)wrote >= sizeof link)
    return NULL;
  ssize_t n = readlink(link, buf, sizeof buf - 1);
  if (n < 0)
    return NULL;
  buf[n] = 0;
  size_t len = (size_t)n;
  char *out = malloc(len + 1);
  if (!out)
    return NULL;
  memcpy(out, buf, len + 1);
  return out;
}

int main(int argc, char **argv) {
  const char *log = getenv("LP_LOG");
  if (!log)
    return 1;
  int pgrp = 0, sid = 0;
  if (!read_ids(&pgrp, &sid))
    return 1;
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd))
    return 1;
  struct stat st;
  bool inherited = fstat(98, &st) == 0;
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_val *args = yyjson_mut_arr(doc);
  for (int i = 1; i < argc; i++)
    yyjson_mut_arr_add_strcpy(doc, args, argv[i]);
  yyjson_mut_obj_add_val(doc, root, "args", args);
  yyjson_mut_obj_add_strcpy(doc, root, "cwd", cwd);
  yyjson_mut_obj_add_int(doc, root, "pid", (int)getpid());
  yyjson_mut_obj_add_int(doc, root, "pgrp", pgrp);
  yyjson_mut_obj_add_int(doc, root, "sid", sid);
  yyjson_mut_val *stdio = yyjson_mut_arr(doc);
  for (int fd = 0; fd < 3; fd++) {
    char *target = fd_target(fd);
    if (!target)
      return 1;
    yyjson_mut_arr_add_strcpy(doc, stdio, target);
    free(target);
  }
  yyjson_mut_obj_add_val(doc, root, "stdio", stdio);
  yyjson_mut_obj_add_bool(doc, root, "inherited", inherited);
  const char *marker = getenv("LP_MARKER");
  if (marker)
    yyjson_mut_obj_add_strcpy(doc, root, "marker", marker);
  size_t n = 0;
  char *text = yyjson_mut_write(doc, 0, &n);
  yyjson_mut_doc_free(doc);
  if (!text)
    return 1;
  char *line = malloc(n + 2);
  if (!line)
    return 1;
  memcpy(line, text, n);
  line[n] = '\n';
  int fd = open(log, O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0)
    return 1;
  write_all(fd, line, n + 1);
  close(fd);
  free(line);
  free(text);
  return 0;
}
