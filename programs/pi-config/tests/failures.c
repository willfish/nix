#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool named(const char *path) {
  const char *expected = getenv("PI_CONFIG_FAILURE_PATH");
  return expected && path && !strcmp(expected, path);
}
static bool selected(int fd) {
  char path[64], target[4096];
  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(path, target, sizeof target - 1);
  if (n < 0)
    return false;
  target[n] = 0;
  return named(target);
}
static bool fail(const char *op) {
  const char *value = getenv("PI_CONFIG_FAIL");
  return value && !strcmp(value, op);
}
int open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  static int (*real)(const char *, int, ...);
  if (!real) {
#ifdef __USE_FILE_OFFSET64
    void *symbol = dlsym(RTLD_NEXT, "open64");
#else
    void *symbol = dlsym(RTLD_NEXT, "open");
#endif
    memcpy(&real, &symbol, sizeof real);
  }
  if (named(path) && fail("create")) {
    errno = EACCES;
    return -1;
  }
  return real(path, flags, mode);
}
ssize_t write(int fd, const void *data, size_t n) {
  static ssize_t (*real)(int, const void *, size_t);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "write");
    memcpy(&real, &symbol, sizeof real);
  }
  if (selected(fd)) {
    const char *release = getenv("PI_CONFIG_RELEASE");
    if (release)
      for (size_t i = 0; access(release, F_OK) != 0; i++) {
        if (i >= 5000) {
          errno = ETIMEDOUT;
          return -1;
        }
        usleep(1000);
      }
    if (fail("write")) {
      errno = EIO;
      return -1;
    }
    if (fail("short-write") && n > 1)
      n /= 2;
  }
  return real(fd, data, n);
}
int fchmod(int fd, mode_t mode) {
  static int (*real)(int, mode_t);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "fchmod");
    memcpy(&real, &symbol, sizeof real);
  }
  if (selected(fd) && fail("chmod")) {
    errno = EIO;
    return -1;
  }
  return real(fd, mode);
}
int close(int fd) {
  static int (*real)(int);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "close");
    memcpy(&real, &symbol, sizeof real);
  }
  bool bad = selected(fd) && fail("close");
  int status = real(fd);
  if (bad) {
    errno = EIO;
    return -1;
  }
  return status;
}
int rename(const char *old, const char *new) {
  static int (*real)(const char *, const char *);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "rename");
    memcpy(&real, &symbol, sizeof real);
  }
  if (named(old) && fail("rename")) {
    errno = EIO;
    return -1;
  }
  return real(old, new);
}
