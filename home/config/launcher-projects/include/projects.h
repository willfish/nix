#pragma once
#include <glib.h>
#include <stdbool.h>
#include <yyjson.h>
typedef struct {
  size_t max_depth, max_entries, max_dirents;
} Limits;
typedef struct {
  size_t used, scans;
  bool failed;
} ScanStats;
extern const Limits project_limits;
char *path_normalize(const char *path, bool collapse_parent);
char *home_absolute(const char *path);
char *selected_home(const char *explicit_home);
char *project_id(const char *path);
char *safe_text(const char *value);
bool valid_id(const char *id);
bool skipped_name(const char *name);
bool real_dir(const char *path);
bool git_marker(const char *path);
GPtrArray *discover(const char *home, Limits limits, ScanStats *stats);
bool launchable(const char *home, const char *path);
yyjson_mut_doc *catalogue(const char *home, GPtrArray *paths);
bool spawn_project(const char *action, const char *path);
int open_project(const char *home, const char *id, const char *action);
int projects_main(int argc, char **argv);
