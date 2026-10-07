#pragma once
#include <gio/gio.h>
#include <stdbool.h>
#include <yyjson.h>
typedef struct {
  yyjson_doc *doc;
  yyjson_val *catalogue;
  char *state, *config, *error;
  const char *greeter, *proc_root;
  GPtrArray *warnings;
  int fail_at, atomic_count;
} Themes;
typedef struct {
  bool ok, timed_out;
  int status;
  char *out, *err, *failure;
} Command;
yyjson_val *field(yyjson_val *object, const char *key);
bool truth(yyjson_val *value);
char *scalar(yyjson_val *value, const char *fallback);
const char *string(Themes *t, yyjson_val *value);
GPtrArray *keys(yyjson_val *object);
char *trim(const char *text);
char **lines(const char *text);
char *path_join(const char *root, const char *name);
bool fail(Themes *t, const char *format, ...) G_GNUC_PRINTF(2, 3);
void warn(Themes *t, const char *text);
GBytes *read_bytes(Themes *t, const char *path);
char *read_text(Themes *t, const char *path);
bool exists(const char *path);
bool path_is_symlink(const char *path);
bool remove_file(Themes *t, const char *path);
bool atomic_write(Themes *t, const char *path, GBytes *content);
Command *command(const char *const *argv, const char *input, guint timeout_ms,
                 bool inherit);
void command_free(Command *c);
bool checked(Themes *t, Command *c);
char *selection(Themes *t);
char *current_mode(Themes *t);
yyjson_val *resolve(Themes *t, const char *selected);
yyjson_val *session(yyjson_val *palette, const char *mode);
bool apply(Themes *t, const char *selected);
bool set_mode(Themes *t, const char *mode);
char *choose(Themes *t);
GHashTable *theme_variables(const char *text);
const char *selection_text(const char *background, const char *paper,
                           const char *ink, const char *bright);
bool publish_session(Themes *t);
void reload_apps(Themes *t);
void publish_greeter(Themes *t, const char *selected);
int theme_run(int argc, char **argv, const char *greeter, const char *proc_root,
              int fail_at);
