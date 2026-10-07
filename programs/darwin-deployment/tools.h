#pragma once
#include <gio/gio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <yyjson.h>
typedef yyjson_val Val;
typedef yyjson_mut_doc Doc;
typedef yyjson_mut_val Mut;
typedef struct {
  int code;
  bool error;
  GString *out;
} Result;
typedef Result (*Run)(void *context, const char *const *argv, unsigned timeout,
                      bool capture);
typedef void (*Report)(void *context, const char *message);
typedef struct {
  const char *system, *machine;
  bool found;
  uid_t uid;
  const char *home;
} Identity;
Result command(void *context, const char *const *argv, unsigned timeout,
               bool capture);
void result_free(Result *r);
Val *field(Val *v, const char *key);
const char *text(Val *v);
char **strings(Val *v);
char *resolve_path(const char *path, bool strict);
char *linked(const char *path);
bool exists(const char *path);
bool is_link(const char *path);
bool is_file(const char *path);
bool make_parent(const char *path, mode_t mode);
yyjson_doc *load(const char *path);
char *json(Doc *doc, bool pretty);
bool atomic_file(const char *path, const void *data, size_t length,
                 const char *prefix);
Mut *copy_sorted(Doc *doc, Val *value);
Doc *health_snapshot(Val *config, Run run, void *context);
int rotate_log(const char *path, size_t limit);
bool health_record(Val *config, Doc *snapshot);
GPtrArray *preflight_problems(Val *config, const Identity *identity, Run run,
                              void *context, bool *error);
char *metadata_diff(Val *config);
bool deploy_system(const char *target, const char *profile, const char *current,
                   const char *state, const char *home, Run run, void *context,
                   Report report);
#define NIX_ENV "/nix/var/nix/profiles/default/bin/nix-env"
