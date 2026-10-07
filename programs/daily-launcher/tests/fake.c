#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_all(int fd, const void *data, size_t n) {
  const char *p = data;
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    off += (size_t)w;
  }
  return 0;
}
static int write_u32(int fd, uint32_t v) { return write_all(fd, &v, sizeof v); }
static int write_i32(int fd, int32_t v) { return write_all(fd, &v, sizeof v); }
static int write_blob(int fd, const void *data, uint32_t n) {
  return write_u32(fd, n) || write_all(fd, data, n);
}
static char *read_link(int fd) {
  char path[64], buf[256];
  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(path, buf, sizeof buf - 1);
  if (n < 0)
    return NULL;
  buf[n] = 0;
  return strndup(buf, (size_t)n);
}
int main(int argc, char **argv) {
  const char *log = getenv("DW_LOG");
  if (!log)
    return 2;
  char *copy = strdup(argv[0] ? argv[0] : "");
  if (!copy)
    return 1;
  const char *name = basename(copy);
  char statbuf[512];
  int sfd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
  ssize_t sn = sfd < 0 ? -1 : read(sfd, statbuf, sizeof statbuf - 1);
  if (sfd >= 0)
    close(sfd);
  if (sn < 0) {
    free(copy);
    return 1;
  }
  statbuf[sn] = 0;
  char *fields = strstr(statbuf, ") ");
  if (!fields) {
    free(copy);
    return 1;
  }
  fields += 2;
  int pgrp = 0, sid = 0;
  if (sscanf(fields, "%*s %*s %d %d", &pgrp, &sid) != 2) {
    free(copy);
    return 1;
  }
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) {
    free(copy);
    return 1;
  }
  char *stdio[3];
  for (int fd = 0; fd < 3; fd++) {
    stdio[fd] = read_link(fd);
    if (!stdio[fd])
      stdio[fd] = strdup("");
  }
  const char *keys[] = {"MUX_BACKEND", "MUX_HERDR_ATTACH", "HERDR_TEST",
                        "HERDR", "NOT_HERDR_TEST", "DW_MARKER"};
  const char *names[] = {"backend", "attach", "herdr", "plain", "other", "marker"};
  const char *values[6];
  uint32_t env_n = 0;
  for (size_t i = 0; i < 6; i++) {
    values[i] = getenv(keys[i]);
    if (values[i])
      env_n++;
  }
  char *herdr[64];
  uint32_t herdr_n = 0;
  extern char **environ;
  for (char **e = environ; *e && herdr_n < 64; e++)
    if (!strncmp(*e, "HERDR_", 6) && strchr(*e, '=')) {
      size_t n = (size_t)(strchr(*e, '=') - *e);
      herdr[herdr_n++] = strndup(*e, n);
    }
  char *input = NULL;
  uint32_t input_len = 0;
  int has_input = getenv("DW_READ_STDIN") && !strcmp(getenv("DW_READ_STDIN"), "1");
  if (has_input) {
    size_t cap = 0, len = 0;
    char tmp[4096];
    ssize_t r;
    while ((r = read(0, tmp, sizeof tmp)) > 0) {
      if (len + (size_t)r > cap) {
        size_t next = cap ? cap * 2 : 4096;
        while (next < len + (size_t)r)
          next *= 2;
        char *grown = realloc(input, next);
        if (!grown)
          return 1;
        input = grown;
        cap = next;
      }
      memcpy(input + len, tmp, (size_t)r);
      len += (size_t)r;
    }
    input_len = (uint32_t)len;
  }
  int fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    free(copy);
    return 1;
  }
  int bad = write_u32(fd, 0x31465744) || write_blob(fd, name, (uint32_t)strlen(name)) ||
            write_u32(fd, (uint32_t)(argc - 1));
  for (int i = 1; !bad && i < argc; i++)
    bad = write_blob(fd, argv[i], (uint32_t)strlen(argv[i]));
  bad = bad || write_blob(fd, cwd, (uint32_t)strlen(cwd)) || write_i32(fd, (int)getpid()) ||
        write_i32(fd, pgrp) || write_i32(fd, sid);
  for (int i = 0; !bad && i < 3; i++)
    bad = write_blob(fd, stdio[i], (uint32_t)strlen(stdio[i]));
  bad = bad || write_u32(fd, env_n);
  for (size_t i = 0; !bad && i < 6; i++)
    if (values[i])
      bad = write_blob(fd, names[i], (uint32_t)strlen(names[i])) ||
            write_blob(fd, values[i], (uint32_t)strlen(values[i]));
  bad = bad || write_u32(fd, herdr_n);
  for (uint32_t i = 0; !bad && i < herdr_n; i++)
    bad = write_blob(fd, herdr[i], (uint32_t)strlen(herdr[i]));
  struct stat st;
  bad = bad || write_u32(fd, fstat(98, &st) == 0) || write_u32(fd, (uint32_t)has_input);
  if (!bad && has_input)
    bad = write_blob(fd, input ? input : "", input_len);
  if (close(fd) < 0)
    bad = 1;
  for (int i = 0; i < 3; i++)
    free(stdio[i]);
  for (uint32_t i = 0; i < herdr_n; i++)
    free(herdr[i]);
  free(input);
  if (bad) {
    free(copy);
    return 1;
  }
  if (strcmp(name, "systemd-run")) {
    char line[128];
    int wrote = snprintf(line, sizeof line, "fixture-%s\n", name);
    if (wrote < 0 || (size_t)wrote >= sizeof line ||
        write(1, line, (size_t)wrote) != wrote) {
      free(copy);
      return 1;
    }
  }
  const char *large = getenv("DW_LARGE");
  if (large) {
    size_t count = strtoul(large, NULL, 10);
    char block[4096];
    memset(block, 'x', sizeof block);
    while (count) {
      size_t chunk = count < sizeof block ? count : sizeof block;
      if (write(1, block, chunk) != (ssize_t)chunk) {
        free(copy);
        return 1;
      }
      count -= chunk;
    }
  }
  const char *err = getenv("DW_STDERR");
  if (err && write(2, err, strlen(err)) != (ssize_t)strlen(err)) {
    free(copy);
    return 1;
  }
  const char *delay = getenv("DW_DELAY");
  if (delay && *delay)
    usleep((useconds_t)strtoul(delay, NULL, 10) * 1000);
  if (getenv("DW_SIGNAL") && !strcmp(getenv("DW_SIGNAL"), "SIGTERM")) {
    free(copy);
    raise(SIGTERM);
    return 1;
  }
  int code = getenv("DW_STATUS") ? atoi(getenv("DW_STATUS")) : 0;
  free(copy);
  return code;
}
