#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stddef.h>

enum Kind { J_NULL, J_BOOL, J_INT, J_REAL, J_STRING, J_ARRAY, J_OBJECT };
typedef struct Json Json;
struct Json {
  enum Kind kind;
  unsigned refs;
  bool boolean;
  double real;
  GString *string;
  GPtrArray *values, *keys;
};
Json *json_parse(const char *input);
void json_free(Json *value);
Json *json_ref(Json *value);
Json *json_node(enum Kind kind);
Json *json_text(const char *text, size_t length);
Json *json_get(Json *object, const char *key, size_t length);
void json_set(Json *object, const char *key, size_t length, Json *value);
bool json_remove(Json *object, const char *key, size_t length);
char *json_encode(Json *value);
