#pragma once
#include "common.h"
#include "json.h"
#include <sys/stat.h>
Json *a_text(const char *s);
Json *a_int(const char *s);
Json *a_size(guint64 n);
Json *a_bool(bool b);
void a_put(Json *object, const char *key, Json *value);
void a_add(Json *array, Json *value);
char *a_integer(const char *s);
char *a_normal(const char *path);
char *a_join(const char *root, const char *name);
char *a_resolve(const char *path, bool strict);
char *a_expand(const char *path);
char *a_lower(const char *text);
char *a_repr(const char *text);
GPtrArray *a_names(const char *path, bool sorted, int *error);
bool a_suffix(const char *path, bool all, bool cue);
bool a_size_tree(const char *path, guint64 *size, bool *audio);
bool a_output(const char *path, GString *data, bool binary);
GString *a_json(Json *value);
char *a_error(const char *path, int error);
