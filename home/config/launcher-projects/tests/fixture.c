#define _GNU_SOURCE
#include "projects.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
struct dirent64 *__real_readdir64(DIR *dir);
struct dirent64 *__wrap_readdir64(DIR *dir) {
  static size_t calls;
  const char *after = getenv("LP_READDIR_FAIL_AFTER");
  if (after && calls++ >= strtoul(after, NULL, 10)) {
    errno = EIO;
    return NULL;
  }
  return __real_readdir64(dir);
}
#endif
int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "cli"))
    return projects_main(argc - 1, argv + 1);
  if (argc == 3 && !strcmp(argv[1], "id")) {
    char *s = project_id(argv[2]);
    puts(s);
    g_free(s);
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "absolute")) {
    char *s = home_absolute(argv[2]);
    if (!s)
      return 1;
    puts(s);
    g_free(s);
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "launchable")) {
    char *home = home_absolute(argv[2]);
    if (!home)
      return 1;
    puts(launchable(home, argv[3]) ? "true" : "false");
    g_free(home);
    return 0;
  }
  if (argc == 5 && !strcmp(argv[1], "fd-open")) {
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, 98) < 0)
      return 1;
    close(fd);
    int result = open_project(argv[2], argv[3], argv[4]);
    close(98);
    return result;
  }
  if (argc == 6 && !strcmp(argv[1], "discover")) {
    char *home = home_absolute(argv[2]);
    if (!home)
      return 1;
    Limits limits = {.max_depth = strtoul(argv[3], NULL, 10),
                     .max_entries = strtoul(argv[4], NULL, 10),
                     .max_dirents = strtoul(argv[5], NULL, 10)};
    ScanStats stats = {0};
    GPtrArray *paths = discover(home, limits, &stats);
    if (!paths) {
      g_free(home);
      return 1;
    }
    yyjson_mut_doc *doc = catalogue(home, paths);
    yyjson_mut_val *items = yyjson_mut_doc_get_root(doc),
                   *root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "entries", items);
    yyjson_mut_obj_add_uint(doc, root, "used", stats.used);
    yyjson_mut_obj_add_uint(doc, root, "scans", stats.scans);
    yyjson_mut_doc_set_root(doc, root);
    char *json = yyjson_mut_write(doc, 0, NULL);
    puts(json);
    free(json);
    yyjson_mut_doc_free(doc);
    g_ptr_array_free(paths, true);
    g_free(home);
    return 0;
  }
  return 64;
}
