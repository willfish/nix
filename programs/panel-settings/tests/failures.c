#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool temporary(int fd) {
  char path[64], target[4096];
  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(path, target, sizeof target - 1);
  if (n < 0)
    return false;
  target[n] = 0;
  return strstr(target, "/.calendar-settings-") ||
         strstr(target, "/.weather-settings-");
}
static bool fail(const char *operation) {
  const char *value = getenv("PANEL_FIXTURE_FAIL");
  return value && !strcmp(value, operation);
}
int g_mkstemp_full(char *pattern, int flags, int mode) {
  static int (*real)(char *, int, int);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "g_mkstemp_full");
    memcpy(&real, &symbol, sizeof real);
  }
  if (fail("create")) {
    size_t n = strlen(pattern);
    memcpy(pattern + n - 6, "ownedx", 6);
    errno = EEXIST;
    return -1;
  }
  return real(pattern, flags, mode);
}
ssize_t write(int fd, const void *data, size_t length) {
  static ssize_t (*real)(int, const void *, size_t);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "write");
    memcpy(&real, &symbol, sizeof real);
  }
  if (temporary(fd)) {
    const char *release = getenv("PANEL_FIXTURE_RELEASE");
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
    if (fail("short-write") && length > 1)
      length /= 2;
  }
  return real(fd, data, length);
}
int fchmod(int fd, mode_t mode) {
  static int (*real)(int, mode_t);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "fchmod");
    memcpy(&real, &symbol, sizeof real);
  }
  if (temporary(fd) && fail("chmod")) {
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
  bool bad = temporary(fd) && fail("close");
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
  if (strstr(old, "-settings-") && fail("rename")) {
    errno = EIO;
    return -1;
  }
  int status = real(old, new);
  if (!status && fail("post-rename")) {
    int fd = open(old, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      ssize_t written = write(fd, "other", 5);
      close(fd);
      if (written != 5)
        return -1;
    }
  }
  return status;
}
