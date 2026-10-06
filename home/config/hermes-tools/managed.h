#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum { NIL, BOOL, NUMBER, TEXT, SEQUENCE, MAPPING, SPECIAL } Kind;
typedef struct Value {
  Kind kind;
  char *text, *tag;
  size_t length;
  bool boolean;
  GPtrArray *keys, *values;
} Value;
extern bool failed;
void arena_start(void);
void arena_end(void);
Value *value(Kind kind);
Value *string(const char *text);
Value *string_n(const char *text, size_t length);
const char *cstring(Value *v);
Value *boolean(bool b);
Value *number(const char *s);
Value *get(Value *object, const char *key);
void put(Value *object, const char *key, Value *v);
void append(Value *array, Value *v);
bool map(Value *v);
bool array(Value *v);
bool truth(Value *v);
bool equal(Value *a, Value *b);
Value *copy(Value *v);
char *as_text(Value *v);
Value *parse_json(const char *data, size_t length);
Value *parse_yaml(const char *data, size_t length);
GBytes *json_bytes(Value *v, bool pretty);
GBytes *yaml_bytes(Value *v);
char *path_normal(const char *path);
char *path_resolve(const char *path);
char *path_join(const char *parent, const char *child);
char *safe_path(const char *root, const char *relative);
bool exists(const char *path);
bool regular(const char *path);
bool symlinked(const char *path);
bool parents(const char *path, mode_t mode);
GBytes *read_bytes(const char *path);
char *read_text(const char *path);
char *strip(char *s);
Value *read_json(const char *path);
bool atomic_write(const char *path, GBytes *data, mode_t mode,
                  const char *prefix, bool unchanged);
Value *merge_profile(Value *original, Value *overlay);
bool merge_routes(Value *config, Value *house);
bool inject_key(Value *profile, Value *key);
bool apply_profile(const char *profile, const char *overlay,
                   const char *key_file);
bool apply_routes(const char *path, const char *house);
bool apply_declaration(const char *declaration, const char *root,
                       const char *overlay, const char *key_file);
