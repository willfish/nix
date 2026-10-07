#pragma once
#include <gio/gio.h>
#include <stdbool.h>
#include <yyjson.h>
typedef yyjson_val Val;
typedef yyjson_mut_val Mut;
typedef yyjson_mut_doc Doc;
extern bool focus_error;
Val *field(Val *v, const char *key);
bool truth(Val *v);
bool equal(Val *a, Val *b);
char *string(Val *v);
char *strip(const char *s);
char *integer(Val *v);
char *integer_text(const char *s);
int integer_compare(const char *a, const char *b);
char *home_path(void);
char *normal_path(const char *s);
char *json(Doc *doc);
bool utf8_bytes(const char *bytes, size_t length);
yyjson_doc *read_json(const char *path, bool tolerant);
Doc *empty_object(void);
void member(Doc *d, Mut *o, const char *key, Val *v);
bool digit_text(const char *s);
Doc *parse_toast(const char *summary, const char *body, const char *app);
Doc *resolve_target(Val *snapshot, Val *toast);
bool target_alive(Val *snapshot, Val *target);
Doc *focus_payload(Val *target);
Val *choose_window(Val *clients, const char *pid, GPtrArray *ancestors,
                   const char *label);
char *cache_path(void);
Doc *recall(const char *summary, const char *body, const char *ts,
            const char *path);
bool remember(const char *summary, const char *body, const char *ts,
              Val *target, const char *path);
GPtrArray *socket_paths(void);
yyjson_doc *herdr_call(const char *path, const char *method, Val *params);
Val *snapshot_from(yyjson_doc *response);
char *parent_pid(const char *pid, const char *proc);
GPtrArray *ancestor_pids(const char *start, const char *proc);
GPtrArray *attached_pids(const char *path, const char *proc);
yyjson_doc *hypr_clients(void);
bool focus_address(Val *address);
int focus_main(int argc, char **argv);
