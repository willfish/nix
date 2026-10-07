#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static bool named(const char *path) {
  const char *expected = getenv("ADAPT_FAILURE_PATH");
  return path && expected && !strcmp(path, expected);
}
static bool selected(int fd) {
  char name[64], path[4096];
  snprintf(name, sizeof name, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(name, path, sizeof path - 1);
  if (n < 0)
    return false;
  path[n] = 0;
  return named(path);
}
static bool fail(const char *mode) {
  const char *current = getenv("ADAPT_FAIL");
  return current && !strcmp(current, mode);
}
FILE *fopen(const char *path, const char *mode) {
  static FILE *(*real)(const char *, const char *);
  if (!real) {
#ifdef __USE_FILE_OFFSET64
    void *symbol = dlsym(RTLD_NEXT, "fopen64");
#else
    void *symbol = dlsym(RTLD_NEXT, "fopen");
#endif
    memcpy(&real, &symbol, sizeof real);
  }
  if (named(path) && mode[0] == 'w' && fail("open")) {
    errno = EACCES;
    return NULL;
  }
  return real(path, mode);
}
size_t fwrite(const void *data, size_t size, size_t count, FILE *file) {
  static size_t (*real)(const void *, size_t, size_t, FILE *);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "fwrite");
    memcpy(&real, &symbol, sizeof real);
  }
  if (selected(fileno(file)) && fail("write")) {
    if (count > 3)
      count = 3;
    else
      count = 0;
    size_t n = real(data, size, count, file);
    errno = EIO;
    return n;
  }
  return real(data, size, count, file);
}
int fclose(FILE *file) {
  static int (*real)(FILE *);
  if (!real) {
    void *symbol = dlsym(RTLD_NEXT, "fclose");
    memcpy(&real, &symbol, sizeof real);
  }
  bool bad = selected(fileno(file)) && fail("close");
  int status = real(file);
  if (bad) {
    errno = EIO;
    return EOF;
  }
  return status;
}
struct dirent *readdir(DIR *dir) {
  static struct dirent *(*real)(DIR *);
  static unsigned seen;
  if (!real) {
#ifdef __USE_FILE_OFFSET64
    void *symbol = dlsym(RTLD_NEXT, "readdir64");
#else
    void *symbol = dlsym(RTLD_NEXT, "readdir");
#endif
    memcpy(&real, &symbol, sizeof real);
  }
  if (selected(dirfd(dir)) && fail("scan") && ++seen == 4) {
    errno = EIO;
    return NULL;
  }
  return real(dir);
}
