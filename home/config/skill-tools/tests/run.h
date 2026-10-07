#ifndef SKILL_TOOLS_CHECK_RUN_H
#define SKILL_TOOLS_CHECK_RUN_H
#include <yyjson.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct {
  int status;
  unsigned char *out;
  size_t out_len;
  unsigned char *err;
  size_t err_len;
  bool exited;
} Proc;
void check_begin(const char *name);
void check_fail(const char *fmt, ...);
int check_finish(void);
char *check_exe_dir(void);
char *check_join(const char *a, const char *b);
char *check_temp(const char *prefix);
int check_mkdir(const char *path);
int check_mkdir_p(const char *path);
int check_write(const char *path, const void *data, size_t len);
unsigned char *check_read(const char *path, size_t *len);
int check_mode(const char *path);
void check_rm_rf(const char *path);
Proc check_run(const char *program, const char *const *args, const void *input,
               size_t input_len, const char *const *env, int timeout_sec);
void proc_free(Proc *proc);
bool contains_text(const unsigned char *data, size_t len, const char *text);
bool ends_with(const unsigned char *data, size_t len, const char *text);
yyjson_doc *parse_json(const void *data, size_t len);
yyjson_val *jget(yyjson_val *value, const char *key);
const char *jstr(yyjson_val *value, size_t *len);
bool json_equal(yyjson_val *a, yyjson_val *b);
char *env_or(const char *name, const char *fallback);
#endif
