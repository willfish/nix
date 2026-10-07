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
static bool fail(const char *name) {
  const char *value = getenv("GREETER_FIXTURE_FAIL");
  return value && !strcmp(value, name);
}
static bool selected(int fd) {
  const char *expected = getenv("GREETER_FIXTURE_SELECTION");
  if (!expected)
    return false;
  char path[64], target[4096];
  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(path, target, sizeof target - 1);
  if (n < 0)
    return false;
  target[n] = 0;
  return !strcmp(target, expected);
}
uid_t geteuid(void) {
  static uid_t (*real)(void);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "geteuid");
    memcpy(&real, &symbol, sizeof real);
  }
  uid_t uid = real();
  return fail("uid") ? uid + 1 : uid;
}
char *g_mkdtemp(char *pattern) {
  static char *(*real)(char *);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "g_mkdtemp");
    memcpy(&real, &symbol, sizeof real);
  }
  if (fail("create")) {
    size_t n = strlen(pattern);
    memcpy(pattern + n - 6, "ownedx", 6);
    errno = EEXIST;
    return NULL;
  }
  return real(pattern);
}
static ssize_t fixture_read(int fd, void *data, size_t length) {
  static ssize_t (*real)(int, void *, size_t);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "read");
    memcpy(&real, &symbol, sizeof real);
  }
  if (selected(fd)) {
    if (fail("read")) {
      errno = EIO;
      return -1;
    }
    if (fail("short-read") && length > 2)
      length = 2;
  }
  return real(fd, data, length);
}
ssize_t read(int fd, void *data, size_t length) {
  return fixture_read(fd, data, length);
}
ssize_t __read_chk(int fd, void *data, size_t length, size_t capacity) {
  if (length <= capacity)
    return fixture_read(fd, data, length);
  static ssize_t (*real)(int, void *, size_t, size_t);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "__read_chk");
    memcpy(&real, &symbol, sizeof real);
  }
  return real(fd, data, length, capacity);
}
int fstat(int fd, struct stat *info) {
  static int (*real)(int, struct stat *);
  if (!real) {
#ifdef __USE_FILE_OFFSET64
    void *symbol = dlsym(RTLD_NEXT, "fstat64");
#else
    void *symbol = dlsym(RTLD_NEXT, "fstat");
#endif
    memcpy(&real, &symbol, sizeof real);
  }
  if (selected(fd) && fail("stat")) {
    errno = EIO;
    return -1;
  }
  return real(fd, info);
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
int symlink(const char *target, const char *path) {
  static int (*real)(const char *, const char *);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "symlink");
    memcpy(&real, &symbol, sizeof real);
  }
  if (fail("symlink")) {
    errno = EIO;
    return -1;
  }
  return real(target, path);
}
int rename(const char *old, const char *new) {
  static int (*real)(const char *, const char *);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "rename");
    memcpy(&real, &symbol, sizeof real);
  }
  const char *release = getenv("GREETER_FIXTURE_RELEASE");
  if (release && strstr(old, "/.theme-"))
    for (size_t i = 0; access(release, F_OK) != 0; i++) {
      if (i >= 5000) {
        errno = ETIMEDOUT;
        return -1;
      }
      usleep(1000);
    }
  if (fail("rename")) {
    errno = EIO;
    return -1;
  }
  return real(old, new);
}
int rmdir(const char *path) {
  static int (*real)(const char *);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "rmdir");
    memcpy(&real, &symbol, sizeof real);
  }
  if (fail("rmdir") && strstr(path, "/.theme-")) {
    errno = EIO;
    return -1;
  }
  return real(path);
}
