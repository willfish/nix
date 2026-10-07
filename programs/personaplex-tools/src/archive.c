#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "tools.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char *path, *name;
  struct timespec time;
  size_t order;
} Directory;
static void directory_free(void *raw) {
  Directory *d = raw;
  g_free(d->path);
  g_free(d->name);
  g_free(d);
}
static int directory_compare(const void *a, const void *b) {
  const Directory *x = *(Directory *const *)a, *y = *(Directory *const *)b;
  int c = strcmp(x->name, y->name);
  return c ? c : (x->order > y->order) - (x->order < y->order);
}
static char *resolve_missing(const char *path, unsigned links) {
  if (links > 40) {
    errno = ELOOP;
    return NULL;
  }
  char *resolved = realpath(path, NULL);
  if (resolved)
    return resolved;
  struct stat st;
  if (!lstat(path, &st) && S_ISLNK(st.st_mode)) {
    char *target = g_file_read_link(path, NULL),
         *parent = g_path_get_dirname(path);
    char *next = target ? (g_path_is_absolute(target)
                               ? g_strdup(target)
                               : g_build_filename(parent, target, NULL))
                        : NULL;
    char *result = next ? resolve_missing(next, links + 1) : NULL;
    g_free(target);
    g_free(parent);
    g_free(next);
    return result;
  }
  if (!strcmp(path, "/"))
    return g_strdup("/");
  if (errno != ENOENT && errno != ENOTDIR)
    return NULL;
  char *parent = g_path_get_dirname(path), *leaf = g_path_get_basename(path);
  char *base = resolve_missing(parent, links), *result = NULL;
  if (base) {
    if (!strcmp(leaf, ".."))
      result = g_path_get_dirname(base);
    else if (!strcmp(leaf, "."))
      result = g_strdup(base);
    else
      result = g_build_filename(base, leaf, NULL);
  }
  g_free(parent);
  g_free(leaf);
  g_free(base);
  return result;
}
static char *safe_target(const char *root, const char *prefix,
                         struct archive_entry *entry, bool confinement,
                         char **error) {
  const char *name = archive_entry_pathname(entry);
  mode_t type = archive_entry_filetype(entry);
  if (!name || !*name || *name == '/' ||
      (type != AE_IFREG && type != AE_IFDIR) || archive_entry_hardlink(entry) ||
      archive_entry_symlink(entry)) {
    pp_error(error, "Unsafe member in %s archive", prefix);
    return NULL;
  }
  char **parts = g_strsplit(name, "/", -1);
  GString *relative = g_string_new(NULL);
  bool first = true, ok = true;
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    if (!strcmp(parts[i], "..") || (first && strcmp(parts[i], prefix))) {
      ok = false;
      break;
    }
    if (!first)
      g_string_append_c(relative, '/');
    g_string_append(relative, parts[i]);
    first = false;
  }
  if (first)
    ok = false;
  char *target = ok ? g_build_filename(root, relative->str, NULL) : NULL;
  if (confinement && target) {
    char *resolved = resolve_missing(target, 0);
    size_t len = strlen(root);
    if (!resolved ||
        (strcmp(resolved, root) && !(g_str_has_prefix(resolved, root) &&
                                     (len == 1 || resolved[len] == '/'))))
      ok = false;
    g_free(resolved);
  }
  g_strfreev(parts);
  g_string_free(relative, true);
  if (!ok) {
    g_free(target);
    pp_error(error, "Unsafe member in %s archive", prefix);
    return NULL;
  }
  return target;
}
static struct archive *open_archive(const char *path, char **error) {
  struct archive *a = archive_read_new();
  archive_read_support_format_tar(a);
  archive_read_support_filter_none(a);
  archive_read_support_filter_gzip(a);
  archive_read_support_filter_bzip2(a);
  archive_read_support_filter_xz(a);
  if (archive_read_open_filename(a, path, 65536) != ARCHIVE_OK) {
    pp_error(error, "Could not open model archive");
    archive_read_free(a);
    return NULL;
  }
  return a;
}
static bool timestamp(const char *path, struct timespec time, char **error) {
  struct timespec times[2] = {time, time};
  return utimensat(AT_FDCWD, path, times, 0) == 0 ||
         pp_error(error, "Could not set model archive timestamps");
}
bool pp_extract(const char *root_arg, const char *name, char **error) {
  char *root = realpath(root_arg, NULL);
  if (!root)
    return pp_error(error, "Could not resolve model directory");
  char *filename = g_strdup_printf("%s.tgz", name),
       *path = g_build_filename(root, filename, NULL);
  g_free(filename);
  struct archive *a = open_archive(path, error);
  bool ok = a != NULL;
  struct archive_entry *entry;
  int status = ARCHIVE_EOF;
  // Validate names/types first; apply data-filter confinement per extracted
  // entry.
  while (ok && (status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    char *target = safe_target(root, name, entry, false, error);
    ok = target != NULL;
    g_free(target);
    if (ok && archive_read_data_skip(a) != ARCHIVE_OK)
      ok = pp_error(error, "Could not read model archive");
  }
  if (ok && status != ARCHIVE_EOF)
    ok = pp_error(error, "Could not read model archive");
  if (a)
    archive_read_free(a);
  a = ok ? open_archive(path, error) : NULL;
  if (ok && !a)
    ok = false;
  GPtrArray *directories = g_ptr_array_new_with_free_func(directory_free);
  size_t order = 0;
  while (ok && (status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    char *target = safe_target(root, name, entry, true, error);
    if (!target) {
      ok = false;
      break;
    }
    struct timespec time = {archive_entry_mtime(entry),
                            archive_entry_mtime_nsec(entry)};
    if (archive_entry_filetype(entry) == AE_IFDIR) {
      if (g_mkdir_with_parents(target, 0777))
        ok = pp_error(error, "Could not create model archive directory");
      else {
        Directory *d = g_new0(Directory, 1);
        d->path = g_strdup(target);
        d->name = g_strdup(archive_entry_pathname(entry));
        d->time = time;
        d->order = order++;
        g_ptr_array_add(directories, d);
      }
    } else {
      char *parent = g_path_get_dirname(target);
      if (g_mkdir_with_parents(parent, 0777))
        ok = pp_error(error, "Could not create model archive directory");
      g_free(parent);
      FILE *file = ok ? fopen(target, "wb") : NULL;
      if (ok && !file)
        ok = pp_error(error, "Could not write model archive file");
      char data[65536];
      la_ssize_t n = 0;
      while (ok && (n = archive_read_data(a, data, sizeof(data))) > 0)
        if (fwrite(data, 1, (size_t)n, file) != (size_t)n)
          ok = pp_error(error, "Could not write model archive file");
      if (ok && n < 0)
        ok = pp_error(error, "Could not read model archive");
      if (file && fclose(file))
        ok = pp_error(error, "Could not write model archive file");
      mode_t mode = archive_entry_perm(entry) & 0755;
      if (!(mode & 0100))
        mode &= ~0111;
      mode |= 0600;
      if (ok && chmod(target, mode))
        ok = pp_error(error, "Could not set model archive permissions");
      if (ok)
        ok = timestamp(target, time, error);
    }
    g_free(target);
  }
  if (ok && status != ARCHIVE_EOF)
    ok = pp_error(error, "Could not read model archive");
  if (a)
    archive_read_free(a);
  g_ptr_array_sort(directories, directory_compare);
  for (size_t i = directories->len; ok && i > 0; i--) {
    Directory *d = directories->pdata[i - 1];
    ok = timestamp(d->path, d->time, error);
  }
  g_ptr_array_free(directories, true);
  g_free(root);
  g_free(path);
  return ok;
}
