#ifndef AGENT_USAGE_H
#define AGENT_USAGE_H
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <yyjson.h>

typedef yyjson_mut_doc Doc;
typedef yyjson_mut_val Val;
typedef struct {
  Val *limits;
  const char *tier;
  const char *status;
} Allowance;
typedef struct {
  Val *body;
  long status;
  bool failed;
} Reply;
typedef Reply (*Transport)(Doc *, const char *, const char *, const char *,
                           const char *);
typedef struct {
  double now;
  Transport transport;
  bool real_time;
} Context;
double context_now(Context *ctx);

void *allocate(size_t n);
char *join(const char *a, const char *b);
char *strip_text(char *s);
const char *home_dir(void);
char *agent_dir(void);
Val *get(Val *v, const char *key);
const char *text(Val *v);
bool truth(Val *v);
void put(Doc *d, Val *o, const char *key, Val *v);
void strput(Doc *d, Val *o, const char *key, const char *s);
void intput(Doc *d, Val *o, const char *key, int64_t n);
Val *copy(Doc *d, Val *v);
Val *parse(Doc *d, const char *s);
Val *parse_bytes(Doc *d, const char *s, size_t len);
Val *load(Doc *d, const char *path);
char *encode(Val *v);
int64_t number(Val *v);
double fraction(Val *v, bool percent);
void local_day(Val *v, double now, char out[11]);
const char *iso_timestamp(Doc *d, Val *v);
const char *iso_time(Doc *d, double seconds, bool z);
Val *empty_stats(Doc *d, double now);
Val *scan_pi(Doc *d, const char *root, const char *provider, double now);
Val *scan_grok(Doc *d, const char *root, double now);
Val *scan_opencode(Doc *d, const char *root, double now);
Allowance parse_codex(Doc *d, Val *payload, double now);
Allowance parse_opencode(Doc *d, Val *payload);
Val *parse_grok(Doc *d, Val *payload);
const char *grok_tier(Doc *d, Val *claims);
Val *grok_login(Val *auth, double now, const char **key);
Val *merge_login(Doc *d, Val *entry, Val *payload, double now);
const char *extract_opencode_key(Val *auth);
bool token_current(Val *entry, int64_t now_ms, int64_t skew_ms);
Val *merge_oauth(Doc *d, Val *entry, Val *payload, int64_t now_ms);
Val *record(Doc *d, const char *id, const char *name, Val *stats, Allowance a,
            const char *help, bool retry, double now);
Val *waybar(Doc *d, Val *records);
Val *status_records(Doc *d);
Val *jwt_claims(Doc *d, const char *token);
Reply http_request(Doc *d, const char *url, const char *token,
                   const char *extra_headers, const char *form);
const char *oauth_access(Doc *d, Context *ctx, const char *provider,
                         const char *url, const char *client_id,
                         const char **failure);
bool write_auth(const char *path, Val *auth);
char *command_key(const char *raw, int timeout_ms);
char *opencode_key(Doc *d);
Val *collect(Doc *d, Context *ctx, const char *id);
#endif
