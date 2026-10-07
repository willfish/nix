#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef STUB_PARTIAL
#define STUB_PARTIAL 0
#endif
static int write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  if (!file)
    return -1;
  size_t len = strlen(text);
  int ok = fwrite(text, 1, len, file) == len && fclose(file) == 0;
  if (!ok)
    fclose(file);
  return ok ? 0 : -1;
}
static int mkdir_p(char *path) {
  for (char *p = path + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    if (mkdir(path, 0777) && errno != EEXIST) {
      *p = '/';
      return -1;
    }
    *p = '/';
  }
  return mkdir(path, 0777) && errno != EEXIST ? -1 : 0;
}
static int json_string(FILE *out, const char *text) {
  if (fputc('"', out) == EOF)
    return -1;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    char escaped[8];
    const char *piece = (const char *)p;
    size_t n = 1;
    if (*p == '"' || *p == '\\') {
      escaped[0] = '\\';
      escaped[1] = (char)*p;
      piece = escaped;
      n = 2;
    } else if (*p < 0x20) {
      int wrote = snprintf(escaped, sizeof escaped, "\\u%04x", *p);
      if (wrote < 0)
        return -1;
      piece = escaped;
      n = (size_t)wrote;
    }
    if (fwrite(piece, 1, n, out) != n)
      return -1;
  }
  return fputc('"', out) == EOF ? -1 : 0;
}
static int has_arg(int argc, char **argv, const char *wanted) {
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], wanted))
      return 1;
  return 0;
}
int main(int argc, char **argv) {
  const char *log = getenv("DOWNLOAD_LOG");
  const char *mode = getenv("DOWNLOAD_MODE");
  if (!log)
    return 2;
  FILE *record = fopen(log, "wb");
  if (!record)
    return 2;
  if (fputc('[', record) == EOF)
    return 2;
  for (int i = 1; i < argc; i++) {
    if (i > 1 && fputc(',', record) == EOF)
      return 2;
    if (json_string(record, argv[i]))
      return 2;
  }
  if (fputc(']', record) == EOF || fclose(record))
    return 2;
  if (mode && !strcmp(mode, "fail"))
    return 17;
  if (STUB_PARTIAL && mode && !strcmp(mode, "strict") &&
      has_arg(argc, argv, "--ignore-errors"))
    return 17;
  const char *out = NULL;
  for (int i = 1; i + 1 < argc; i++)
    if (!strcmp(argv[i], "-o"))
      out = argv[i + 1];
  if (!out)
    return 0;
  char *dir = strdup(out);
  if (!dir)
    return 2;
  char *slash = strrchr(dir, '/');
  if (slash)
    *slash = 0;
  if (mkdir_p(dir)) {
    free(dir);
    return 2;
  }
  int rc = 0;
  size_t dir_len = strlen(dir);
  char *child = malloc(dir_len + 32);
  if (!child)
    rc = 2;
  if (!rc && (STUB_PARTIAL || !mode || strcmp(mode, "no-info"))) {
    snprintf(child, dir_len + 32, "%s/video.info.json", dir);
    if (write_file(child, "{\"title\":\"stub title\",\"uploader\":\"stub "
                          "uploader\",\"duration\":61}"))
      rc = 2;
  }
  if (!rc && STUB_PARTIAL) {
    snprintf(child, dir_len + 32, "%s/a.auto.vtt", dir);
    if (write_file(child, "0:00 --> 0:01\nauto\n"))
      rc = 2;
    snprintf(child, dir_len + 32, "%s/video.en.vtt", dir);
    if (!rc && write_file(child, "0:00 --> 0:01\nkept\n"))
      rc = 2;
  } else if (!rc && (!mode || strcmp(mode, "no-subs"))) {
    snprintf(child, dir_len + 32, "%s/a.auto.vtt", dir);
    if (write_file(child, "0:00 --> 0:01\nauto\n"))
      rc = 2;
    snprintf(child, dir_len + 32, "%s/z.srt", dir);
    if (!rc && write_file(child, "0:00 --> 0:01\nmanual\n"))
      rc = 2;
  }
  free(child);
  free(dir);
  return rc;
}
