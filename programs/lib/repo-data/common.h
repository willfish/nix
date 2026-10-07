#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <yyjson.h>
typedef yyjson_val Val;
Val *field(Val *object, const char *name);
bool text_is(Val *value, const char *text);
bool nonempty(Val *value);
bool integer(Val *value);
bool same(Val *a, Val *b);
bool unique(Val *value, unsigned depth);
yyjson_doc *load_json(const char *path);
GString *read_text(const char *path);
bool write_text(const char *path, const char *data, size_t length);
bool unicode_space(gunichar cp);
char *strip(const char *text);
