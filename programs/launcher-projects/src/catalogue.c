#define _DEFAULT_SOURCE
#include "projects.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int byte_compare(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static GPtrArray *candidate_names(const char *directory, Limits limits,
                                  ScanStats *stats) {
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  DIR *dir = opendir(directory);
  if (!dir)
    return names;
  stats->scans++;
  while (stats->used < limits.max_dirents) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      stats->failed = errno != 0;
      break;
    }
    const char *name = entry->d_name;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
      continue;
    stats->used++;
    if (skipped_name(name))
      continue;
    char *path = g_build_filename(directory, name, NULL);
    bool is_dir = real_dir(path);
    g_free(path);
    if (is_dir)
      g_ptr_array_add(names, g_strdup(name));
  }
  closedir(dir);
  g_ptr_array_sort(names, byte_compare);
  return names;
}
static void walk(const char *directory, size_t depth, GPtrArray *found,
                 Limits limits, ScanStats *stats) {
  if (found->len >= limits.max_entries || stats->used >= limits.max_dirents)
    return;
  GPtrArray *names = candidate_names(directory, limits, stats);
  for (size_t i = 0; i < names->len && !stats->failed; i++) {
    if (found->len >= limits.max_entries)
      break;
    char *child = g_build_filename(directory, (char *)names->pdata[i], NULL);
    if (!real_dir(child)) {
      g_free(child);
      continue;
    }
    if (git_marker(child))
      g_ptr_array_add(found, child);
    else {
      if (depth < limits.max_depth && stats->used < limits.max_dirents)
        walk(child, depth + 1, found, limits, stats);
      g_free(child);
    }
  }
  g_ptr_array_free(names, true);
}
GPtrArray *discover(const char *home, Limits limits, ScanStats *stats) {
  ScanStats unused = {0};
  if (!stats)
    stats = &unused;
  GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
  char *dotfiles = g_build_filename(home, ".dotfiles", NULL);
  if (real_dir(dotfiles) && limits.max_entries)
    g_ptr_array_add(found, dotfiles);
  else
    g_free(dotfiles);
  char *repositories = g_build_filename(home, "Repositories", NULL);
  if (real_dir(repositories) && found->len < limits.max_entries)
    walk(repositories, 1, found, limits, stats);
  g_free(repositories);
  if (stats->failed) {
    g_ptr_array_free(found, true);
    return NULL;
  }
  g_ptr_array_sort(found, byte_compare);
  return found;
}
static void add_text(yyjson_mut_doc *doc, yyjson_mut_val *o, const char *key,
                     const char *raw) {
  char *s = safe_text(raw);
  yyjson_mut_obj_add_strcpy(doc, o, key, s);
  g_free(s);
}
yyjson_mut_doc *catalogue(const char *home, GPtrArray *paths) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *array = yyjson_mut_arr(doc);
  yyjson_mut_doc_set_root(doc, array);
  for (size_t i = 0; i < paths->len; i++) {
    const char *path = paths->pdata[i];
    size_t n = strlen(home);
    const char *relative = path + n;
    while (*relative == '/')
      relative++;
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    char *base = g_path_get_basename(path), *id = project_id(path);
    add_text(doc, o, "Text", base);
    add_text(doc, o, "Subtext", relative);
    yyjson_mut_obj_add_strcpy(doc, o, "Value", id);
    yyjson_mut_obj_add_str(doc, o, "Icon", "folder");
    yyjson_mut_val *keywords = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, o, "Keywords", keywords);
    char **parts = g_strsplit(relative, "/", -1);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (size_t j = 0; parts[j]; j++) {
      const char *candidates[] = {parts[j],
                                  parts[j][0] == '.' ? parts[j] + 1 : parts[j]};
      for (size_t k = 0; k < 2; k++) {
        const char *raw = candidates[k];
        if (*raw && !g_hash_table_contains(seen, raw)) {
          g_hash_table_add(seen, (void *)raw);
          char *s = safe_text(raw);
          yyjson_mut_arr_add_strcpy(doc, keywords, s);
          g_free(s);
        }
      }
    }
    g_hash_table_destroy(seen);
    g_strfreev(parts);
    g_free(base);
    g_free(id);
    yyjson_mut_arr_add_val(array, o);
  }
  return doc;
}
static int list_projects(const char *home) {
  GPtrArray *paths = discover(home, project_limits, NULL);
  if (!paths) {
    fputs("unable to list\n", stderr);
    return 1;
  }
  yyjson_mut_doc *doc = catalogue(home, paths);
  char *json = yyjson_mut_write(doc, 0, NULL);
  int result = 0;
  if (!json) {
    fputs("unable to list\n", stderr);
    result = 1;
  } else {
    bool quoted = false, escaped = false;
    for (const char *p = json; *p; p++) {
      putchar((unsigned char)*p);
      if (quoted) {
        if (escaped)
          escaped = false;
        else if (*p == '\\')
          escaped = true;
        else if (*p == '"')
          quoted = false;
      } else if (*p == '"')
        quoted = true;
      else if (*p == ',' || *p == ':')
        putchar(' ');
    }
    putchar('\n');
    if (ferror(stdout))
      result = 1;
  }
  free(json);
  yyjson_mut_doc_free(doc);
  g_ptr_array_free(paths, true);
  return result;
}
static void usage(FILE *f) {
  fputs("usage: launcher-projects [--home HOME] {list,open} ...\n", f);
}
static bool help_option(const char *s) {
  return !strcmp(s, "-h") || !strcmp(s, "--help") || !strcmp(s, "--he") ||
         !strcmp(s, "--hel");
}
static bool option_token(const char *s) {
  return s[0] == '-' && s[1] &&
         !g_regex_match_simple("^-[0-9]+$|^-[0-9]*\\.[0-9]+$", s, 0, 0);
}
int projects_main(int argc, char **argv) {
  const char *explicit_home = NULL;
  int i = 1;
  while (i < argc && option_token(argv[i])) {
    const char *option = argv[i];
    if (!strcmp(option, "--")) {
      i++;
      break;
    }
    if (help_option(option)) {
      usage(stdout);
      return 0;
    }
    const char *eq = strchr(option, '=');
    size_t len = eq ? (size_t)(eq - option) : strlen(option);
    if (len >= 4 && len <= 6 && !strncmp(option, "--home", len)) {
      if (eq) {
        explicit_home = eq + 1;
        i++;
      } else {
        if (++i >= argc || option_token(argv[i])) {
          usage(stderr);
          return 2;
        }
        explicit_home = argv[i++];
      }
    } else {
      usage(stderr);
      return 2;
    }
  }
  if (i >= argc) {
    usage(stderr);
    return 2;
  }
  const char *operation = argv[i++];
  bool listed = !strcmp(operation, "list"), opened = !strcmp(operation, "open");
  if (!listed && !opened) {
    usage(stderr);
    return 2;
  }
  const char *positionals[2] = {NULL};
  size_t count = 0;
  bool stopped = false, invalid = false;
  for (; i < argc; i++) {
    if (!stopped && help_option(argv[i])) {
      usage(stdout);
      return 0;
    }
    if (opened && !stopped && !strcmp(argv[i], "--")) {
      stopped = true;
      continue;
    }
    if ((!stopped && option_token(argv[i])) || count >= 2)
      invalid = true;
    else
      positionals[count++] = argv[i];
  }
  if (invalid || (listed && count) || (opened && count != 2)) {
    usage(stderr);
    return 2;
  }
  char *home = selected_home(explicit_home);
  if (!home) {
    fputs(listed ? "unable to list\n" : "unknown project\n", stderr);
    return 1;
  }
  int result = listed ? list_projects(home)
                      : open_project(home, positionals[0], positionals[1]);
  g_free(home);
  return result;
}
