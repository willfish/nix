#include "usage.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static Val *either(Val *a, Val *b) { return truth(a) ? a : b; }
static Val *field(Val *v, const char *a, const char *b) {
  Val *x = get(v, a);
  return x ? x : get(v, b);
}
static int64_t add(int64_t a, int64_t b) {
  if (b > 0 && a > INT64_MAX - b)
    return INT64_MAX;
  if (b < 0 && a < INT64_MIN - b)
    return INT64_MIN;
  return a + b;
}
static Val *limit(Doc *d, const char *label, const char *title, double percent,
                  const char *end) {
  Val *v = yyjson_mut_obj(d);
  strput(d, v, "label", label);
  strput(d, v, "title", title);
  put(d, v, "percent", yyjson_mut_real(d, percent));
  strput(d, v, "resetsAt", end);
  return v;
}
Allowance parse_codex(Doc *d, Val *p, double now) {
  Allowance a = {.limits = yyjson_mut_arr(d), .tier = "", .status = ""};
  char *plan = strdup(text(get(p, "plan_type")));
  bool start = true;
  for (char *s = plan; *s; s++) {
    if (*s == '_')
      *s = ' ';
    unsigned char c = (unsigned char)*s;
    *s = (char)(start ? toupper(c) : tolower(c));
    start = !isalpha(c);
  }
  char *s = plan;
  while (isspace((unsigned char)*s))
    s++;
  size_t length = strlen(s);
  while (length && isspace((unsigned char)s[length - 1]))
    s[--length] = 0;
  a.tier = yyjson_mut_get_str(yyjson_mut_strcpy(d, s));
  free(plan);
  Val *rate = get(p, "rate_limit");
  const char *keys[] = {"primary_window", "secondary_window"};
  for (size_t i = 0; i < 2; i++) {
    Val *w = get(rate, keys[i]), *v = get(w, "used_percent");
    if (!yyjson_mut_is_obj(w) || !v || yyjson_mut_is_null(v))
      continue;
    double used = fraction(v, true);
    if (used < 0)
      continue;
    int64_t seconds = number(get(w, "limit_window_seconds"));
    char label[80];
    const char *title = "Limit";
    if (seconds >= 6 * 86400) {
      strcpy(label, "Weekly (7-day)");
      title = "Weekly";
    } else if (seconds >= 3600) {
      snprintf(label, sizeof label, "%.0fh window",
               fmax(1, nearbyint(seconds / 3600.0)));
      title = "Session";
    } else
      strcpy(label, "Limit");
    const char *end = iso_timestamp(d, get(w, "reset_at"));
    if (!*end && get(w, "reset_after_seconds") &&
        !yyjson_mut_is_null(get(w, "reset_after_seconds")))
      end = iso_time(d, now + number(get(w, "reset_after_seconds")), false);
    yyjson_mut_arr_append(a.limits, limit(d, label, title, used, end));
  }
  Val *allowed = get(rate, "allowed");
  if (truth(get(rate, "limit_reached")) ||
      (yyjson_mut_is_bool(allowed) && !yyjson_mut_get_bool(allowed)))
    a.status = "Rate limit reached";
  return a;
}
Val *parse_grok(Doc *d, Val *p) {
  Val *out = yyjson_mut_arr(d), *cfg = get(p, "config");
  if (!yyjson_mut_is_obj(cfg))
    cfg = p;
  if (!yyjson_mut_is_obj(cfg))
    return out;
  Val *period = either(get(cfg, "currentPeriod"), get(cfg, "current_period"));
  const char *end =
      text(either(get(period, "end"), either(get(cfg, "billingPeriodEnd"),
                                             get(cfg, "billing_period_end"))));
  bool monthly = false;
  const char *kind = text(get(period, "type"));
  char *upper = strdup(kind);
  for (char *s = upper; *s; s++)
    *s = (char)toupper((unsigned char)*s);
  monthly = strstr(upper, "MONTH") != NULL;
  free(upper);
  Val *credit = field(cfg, "creditUsagePercent", "credit_usage_percent");
  double overall = -1;
  if (credit)
    overall = fraction(credit, true);
  else if (*end) {
    Val *unified = get(cfg, "isUnifiedBillingUser");
    bool known = yyjson_mut_is_bool(unified) && yyjson_mut_get_bool(unified);
    const char *keys[] = {"onDemandCap",      "on_demand_cap",
                          "prepaidBalance",   "prepaid_balance",
                          "billingPeriodEnd", "billing_period_end",
                          "currentPeriod",    "current_period"};
    for (size_t i = 0; i < 8; i++)
      if (get(cfg, keys[i]))
        known = true;
    if (known)
      overall = 0;
  }
  if (overall >= 0)
    yyjson_mut_arr_append(out,
                          limit(d, monthly ? "Monthly" : "Weekly (7-day)",
                                monthly ? "Monthly" : "Weekly", overall, end));
  Val *rows = either(get(cfg, "productUsage"), get(cfg, "product_usage"));
  size_t i, n;
  Val *row;
  yyjson_mut_arr_foreach(rows, i, n, row) {
    if (strcmp(text(get(row, "product")), "GrokBuild"))
      continue;
    double used = fraction(field(row, "usagePercent", "usage_percent"), true);
    if (used >= 0)
      yyjson_mut_arr_append(out,
                            limit(d, "Grok Build", "Grok Build", used, end));
  }
  return out;
}
Allowance parse_opencode(Doc *d, Val *p) {
  Allowance a = {.limits = yyjson_mut_arr(d), .tier = "", .status = ""};
  Val *usage = get(p, "usage");
  if (!yyjson_mut_is_obj(usage))
    usage = p;
  const char *keys[] = {"rolling", "weekly", "monthly"};
  const char *labels[] = {"5h window", "Weekly (7-day)", "Monthly"};
  const char *titles[] = {"Session", "Weekly", "Monthly"};
  for (size_t i = 0; i < 3; i++) {
    Val *w = get(usage, keys[i]);
    if (!yyjson_mut_is_obj(w))
      continue;
    double used = fraction(field(w, "percent", "usagePercent"), false);
    if (used < 0)
      continue;
    yyjson_mut_arr_append(
        a.limits, limit(d, labels[i], titles[i], used,
                        text(either(get(w, "resetsAt"), get(w, "resetAt")))));
  }
  const char *plan =
      text(either(get(p, "plan"),
                  either(get(p, "tier"),
                         either(get(p, "subscription"), get(usage, "plan")))));
  if (!*plan && yyjson_mut_arr_size(a.limits))
    plan = "Go";
  if (!strcasecmp(plan, "plus") || !strcasecmp(plan, "go plus") ||
      !strcasecmp(plan, "go-plus") || !strcasecmp(plan, "go_plus"))
    plan = "Go Plus";
  else if (!strcasecmp(plan, "go"))
    plan = "Go";
  a.tier = yyjson_mut_get_str(yyjson_mut_strcpy(d, plan));
  return a;
}
const char *grok_tier(Doc *d, Val *p) {
  const char *named =
      text(either(get(p, "subscription_tier"),
                  either(get(p, "subscriptionTier"), get(p, "plan"))));
  char *s = strdup(named), *begin = s;
  while (isspace((unsigned char)*begin))
    begin++;
  size_t n = strlen(begin);
  while (n && isspace((unsigned char)begin[n - 1]))
    begin[--n] = 0;
  if (n) {
    for (char *c = begin; *c; c++)
      if (*c == '_')
        *c = ' ';
    const char *out = yyjson_mut_get_str(yyjson_mut_strcpy(d, begin));
    free(s);
    return out;
  }
  free(s);
  Val *v = get(p, "tier");
  if (!v || (!yyjson_mut_is_num(v) && !yyjson_mut_is_str(v) &&
             !yyjson_mut_is_bool(v)))
    return "";
  char *end;
  long tier = strtol(text(v), &end, 10);
  if (yyjson_mut_is_num(v))
    tier = (long)yyjson_mut_get_num(v);
  else if (yyjson_mut_is_bool(v))
    tier = yyjson_mut_get_bool(v);
  else if (end == text(v) || *end)
    return "";
  const char *labels[] = {"Free",           "SuperGrok",  "X Basic",
                          "X Premium",      "X Premium+", "SuperGrok Heavy",
                          "SuperGrok Lite", "SuperGrok+"};
  return tier >= 0 && tier < 8 ? labels[tier] : "";
}
Val *grok_login(Val *auth, double now, const char **key) {
  time_t t = (time_t)now;
  struct tm utc;
  gmtime_r(&t, &utc);
  char stamp[48];
  strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &utc);
  Val *chosen = NULL, *k, *v;
  size_t i, n;
  yyjson_mut_obj_foreach(auth, i, n, k, v) {
    if (!yyjson_mut_is_obj(v) || !truth(get(v, "key")))
      continue;
    chosen = v;
    *key = text(k);
    const char *expires = text(get(v, "expires_at"));
    if (!*expires || strcmp(expires, stamp) > 0)
      break;
  }
  return chosen;
}
Val *merge_login(Doc *d, Val *entry, Val *p, double now) {
  Val *v = copy(d, entry);
  if (truth(get(p, "access_token")))
    strput(d, v, "key", text(get(p, "access_token")));
  if (truth(get(p, "refresh_token")))
    strput(d, v, "refresh_token", text(get(p, "refresh_token")));
  int64_t seconds = number(get(p, "expires_in"));
  if (seconds > 0)
    strput(d, v, "expires_at", iso_time(d, now + seconds, true));
  return v;
}
const char *extract_opencode_key(Val *auth) {
  const char *names[] = {"opencode-go", "opencode"};
  for (size_t i = 0; i < 2; i++) {
    Val *entry = get(auth, names[i]);
    if (yyjson_mut_is_obj(entry) && truth(get(entry, "key")))
      return text(get(entry, "key"));
    if (yyjson_mut_is_str(entry) && truth(entry))
      return text(entry);
  }
  return "";
}
bool token_current(Val *entry, int64_t now_ms, int64_t skew_ms) {
  return truth(get(entry, "access")) &&
         number(get(entry, "expires")) > add(now_ms, skew_ms);
}
Val *merge_oauth(Doc *d, Val *entry, Val *p, int64_t now_ms) {
  Val *v = copy(d, entry);
  if (truth(get(p, "access_token")))
    strput(d, v, "access", text(get(p, "access_token")));
  if (truth(get(p, "refresh_token")))
    strput(d, v, "refresh", text(get(p, "refresh_token")));
  int64_t seconds = number(get(p, "expires_in"));
  if (seconds > 0)
    intput(
        d, v, "expires",
        add(now_ms, seconds > INT64_MAX / 1000 ? INT64_MAX : seconds * 1000) -
            300000);
  put(d, v, "type",
      truth(get(entry, "type")) ? copy(d, get(entry, "type"))
                                : yyjson_mut_strcpy(d, "oauth"));
  return v;
}

// Keep counters outside the JSON arena so memory grows with distinct sessions
// and models, not with every assistant turn in a long-lived history.
typedef struct {
  char **items;
  size_t n;
} Set;
typedef struct {
  char *name;
  int64_t tokens[4];
  int64_t today;
  bool today_seen;
} Model;
typedef struct {
  Set sessions, today_sessions, days;
  Model *models;
  size_t model_count;
  char today[11], dates[7][11];
  int64_t recent[7];
  int64_t prompts, today_prompts, today_tokens;
} Stats;
static void insert(Set *s, const char *key) {
  for (size_t i = 0; i < s->n; i++)
    if (!strcmp(s->items[i], key))
      return;
  char **p = realloc(s->items, (s->n + 1) * sizeof *p);
  if (!p) {
    fputs("agent-usage: allocation failed\n", stderr);
    exit(1);
  }
  s->items = p;
  s->items[s->n++] = strdup(key);
}
static void clear_set(Set *s) {
  for (size_t i = 0; i < s->n; i++)
    free(s->items[i]);
  free(s->items);
}
static void init_stats(Stats *s, double now) {
  time_t t = (time_t)now;
  struct tm local;
  localtime_r(&t, &local);
  strftime(s->today, 11, "%Y-%m-%d", &local);
  local.tm_hour = 12;
  local.tm_min = 0;
  local.tm_sec = 0;
  time_t civil = timegm(&local);
  for (int i = 0; i < 7; i++) {
    time_t day = civil - (6 - i) * 86400;
    struct tm date;
    gmtime_r(&day, &date);
    strftime(s->dates[i], 11, "%Y-%m-%d", &date);
  }
}
static void add_model(Stats *s, const char *day, const char *session,
                      const char *name, int64_t tokens[4], int64_t prompts) {
  int64_t total = 0;
  for (size_t i = 0; i < 4; i++)
    total = add(total, tokens[i]);
  if (total <= 0 && prompts <= 0)
    return;
  insert(&s->sessions, session);
  insert(&s->days, day);
  s->prompts = add(s->prompts, prompts);
  bool today = !strcmp(day, s->today);
  if (today) {
    insert(&s->today_sessions, session);
    s->today_prompts = add(s->today_prompts, prompts);
    s->today_tokens = add(s->today_tokens, total);
  }
  if (!total)
    return;
  size_t m;
  for (m = 0; m < s->model_count; m++)
    if (!strcmp(s->models[m].name, name))
      break;
  if (m == s->model_count) {
    Model *p = realloc(s->models, (m + 1) * sizeof *p);
    if (!p)
      exit(1);
    s->models = p;
    s->models[m] = (Model){.name = strdup(name)};
    s->model_count++;
  }
  for (size_t i = 0; i < 4; i++)
    s->models[m].tokens[i] = add(s->models[m].tokens[i], tokens[i]);
  if (today) {
    s->models[m].today = add(s->models[m].today, total);
    s->models[m].today_seen = true;
  }
  for (size_t i = 0; i < 7; i++)
    if (!strcmp(s->dates[i], day))
      s->recent[i] = add(s->recent[i], total);
}
static int compare_string(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static Val *finish(Doc *d, Stats *s) {
  Val *v = yyjson_mut_obj(d), *models = yyjson_mut_obj(d),
      *today = yyjson_mut_obj(d), *recent = yyjson_mut_arr(d),
      *active = yyjson_mut_arr(d);
  intput(d, v, "todayPrompts", s->today_prompts);
  intput(d, v, "todaySessions", (int64_t)s->today_sessions.n);
  intput(d, v, "todayTotalTokens", s->today_tokens);
  intput(d, v, "totalPrompts", s->prompts);
  intput(d, v, "totalSessions", (int64_t)s->sessions.n);
  intput(d, v, "activeDays", (int64_t)s->days.n);
  const char *keys[] = {"inputTokens", "outputTokens", "cacheReadInputTokens",
                        "cacheCreationInputTokens"};
  for (size_t i = 0; i < s->model_count; i++) {
    Val *bucket = yyjson_mut_obj(d);
    for (size_t k = 0; k < 4; k++)
      intput(d, bucket, keys[k], s->models[i].tokens[k]);
    put(d, models, s->models[i].name, bucket);
    if (s->models[i].today_seen)
      intput(d, today, s->models[i].name, s->models[i].today);
    free(s->models[i].name);
  }
  for (size_t i = 0; i < 7; i++) {
    Val *row = yyjson_mut_obj(d);
    strput(d, row, "date", s->dates[i]);
    intput(d, row, "messageCount", s->recent[i]);
    yyjson_mut_arr_append(recent, row);
  }
  if (s->days.n)
    qsort(s->days.items, s->days.n, sizeof(char *), compare_string);
  for (size_t i = 0; i < s->days.n; i++)
    yyjson_mut_arr_append(active, yyjson_mut_strcpy(d, s->days.items[i]));
  put(d, v, "todayTokensByModel", today);
  put(d, v, "recentDays", recent);
  put(d, v, "activeDates", active);
  put(d, v, "modelUsage", models);
  clear_set(&s->sessions);
  clear_set(&s->today_sessions);
  clear_set(&s->days);
  free(s->models);
  return v;
}
Val *empty_stats(Doc *d, double now) {
  Stats s = {0};
  init_stats(&s, now);
  return finish(d, &s);
}
static void walk(const char *root, const char *suffix,
                 void (*visit)(const char *, void *), void *ctx) {
  DIR *dir = opendir(root);
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    char *path = join(root, entry->d_name);
    struct stat st;
    if (!lstat(path, &st)) {
      if (S_ISDIR(st.st_mode))
        walk(path, suffix, visit, ctx);
      else {
        size_t len = strlen(entry->d_name), n = strlen(suffix);
        if (len >= n && !strcmp(entry->d_name + len - n, suffix) &&
            (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)))
          visit(path, ctx);
      }
    }
    free(path);
  }
  closedir(dir);
}
static char *stem(const char *path) {
  char *s = strdup(strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
  char *dot = strrchr(s, '.');
  if (dot && dot != s)
    *dot = 0;
  return s;
}
static char *parent_name(const char *path) {
  char *s = strdup(path);
  char *slash = strrchr(s, '/');
  if (slash)
    *slash = 0;
  char *out = strdup(strrchr(s, '/') ? strrchr(s, '/') + 1 : s);
  free(s);
  return out;
}
typedef struct {
  Stats stats;
  double now;
  const char *provider;
} Scan;
static void pi_file(const char *path, void *raw) {
  Scan *ctx = raw;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char *line = NULL, *session = stem(path);
  size_t capacity = 0;
  ssize_t length;
  while ((length = getline(&line, &capacity, f)) >= 0) {
    Doc *d = yyjson_mut_doc_new(NULL);
    Val *event = parse_bytes(d, line, (size_t)length),
        *m = get(event, "message");
    Val *role = get(m, "role");
    if (yyjson_mut_is_obj(m) &&
        (!role || yyjson_mut_is_null(role) ||
         !strcmp(text(role), "assistant")) &&
        !strcmp(text(get(m, "provider")), ctx->provider)) {
      Val *u = get(m, "usage");
      if (!yyjson_mut_is_obj(u))
        u = NULL;
      if (truth(u) || truth(get(m, "model"))) {
        int64_t tokens[] = {
            number(get(u, "input")),
            add(number(get(u, "output")), number(get(u, "reasoning"))),
            number(get(u, "cacheRead")), number(get(u, "cacheWrite"))};
        char day[11];
        local_day(either(get(m, "timestamp"), get(event, "timestamp")),
                  ctx->now, day);
        add_model(
            &ctx->stats, day, session,
            text(either(get(m, "model"), yyjson_mut_str(d, ctx->provider))),
            tokens, 1);
      }
    }
    yyjson_mut_doc_free(d);
  }
  free(session);
  free(line);
  fclose(f);
}
Val *scan_pi(Doc *d, const char *root, const char *provider, double now) {
  Scan ctx = {.now = now, .provider = provider};
  init_stats(&ctx.stats, now);
  walk(root, ".jsonl", pi_file, &ctx);
  return finish(d, &ctx.stats);
}
static void opencode_file(const char *path, void *raw) {
  Scan *ctx = raw;
  Doc *d = yyjson_mut_doc_new(NULL);
  Val *m = load(d, path), *role = get(m, "role");
  const char *provider = text(get(m, "providerID")),
             *model = text(get(m, "modelID"));
  if (yyjson_mut_is_obj(m) &&
      (!role || yyjson_mut_is_null(role) || !strcmp(text(role), "assistant")) &&
      (!strcmp(provider, "opencode") || !strcmp(provider, "opencode-go") ||
       !strncmp(model, "opencode-go/", 12))) {
    Val *u = get(m, "tokens"), *cache = get(u, "cache");
    int64_t tokens[] = {
        number(get(u, "input")),
        add(number(get(u, "output")), number(get(u, "reasoning"))),
        number(get(cache, "read")), number(get(cache, "write"))};
    char day[11];
    local_day(get(get(m, "time"), "created"), ctx->now, day);
    char *fallback = parent_name(path);
    const char *session =
        text(either(get(m, "sessionID"), yyjson_mut_str(d, fallback)));
    add_model(&ctx->stats, day, session, *model ? model : "opencode", tokens,
              1);
    free(fallback);
  }
  yyjson_mut_doc_free(d);
}
Val *scan_opencode(Doc *d, const char *root, double now) {
  Scan ctx = {.now = now};
  init_stats(&ctx.stats, now);
  char *messages = join(root, "message");
  walk(messages, ".json", opencode_file, &ctx);
  free(messages);
  return finish(d, &ctx.stats);
}
static void split_grok(Val *u, int64_t tokens[4]) {
  tokens[2] = number(field(u, "cachedReadTokens", "cacheReadInputTokens"));
  tokens[3] =
      number(field(u, "cacheCreationTokens", "cacheCreationInputTokens"));
  int64_t input = number(get(u, "inputTokens"));
  tokens[0] =
      tokens[2] <= input
          ? (tokens[2] < 0 && input > INT64_MAX + tokens[2] ? INT64_MAX
                                                            : input - tokens[2])
          : input;
  tokens[1] =
      add(number(get(u, "outputTokens")), number(get(u, "reasoningTokens")));
}
typedef struct {
  char *id;
  int64_t total;
  char day[11];
  Doc *doc;
  Val *usage;
} Winner;
typedef struct {
  double now;
  Winner *winners;
  size_t n;
} GrokScan;
static void grok_file(const char *path, void *raw) {
  if (strcmp(strrchr(path, '/') + 1, "updates.jsonl"))
    return;
  GrokScan *ctx = raw;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char *line = NULL, *parent = parent_name(path);
  size_t capacity = 0;
  ssize_t length;
  while ((length = getline(&line, &capacity, f)) >= 0) {
    Doc *d = yyjson_mut_doc_new(NULL);
    Val *e = parse_bytes(d, line, (size_t)length), *params = get(e, "params"),
        *update = get(params, "update"), *u = get(update, "usage");
    int64_t tokens[4];
    split_grok(u, tokens);
    int64_t total = number(get(u, "totalTokens")), sum = 0;
    bool any = false;
    for (size_t k = 0; k < 4; k++) {
      if (tokens[k])
        any = true;
      sum = add(sum, tokens[k]);
    }
    if (!strcmp(text(get(update, "sessionUpdate")), "turn_completed") &&
        yyjson_mut_is_obj(u) && (total > 0 || any)) {
      if (!total)
        total = sum;
      const char *id =
          text(either(get(params, "sessionId"), yyjson_mut_str(d, parent)));
      size_t i;
      for (i = 0; i < ctx->n; i++)
        if (!strcmp(ctx->winners[i].id, id))
          break;
      if (i == ctx->n) {
        Winner *p = realloc(ctx->winners, (i + 1) * sizeof *p);
        if (!p)
          exit(1);
        ctx->winners = p;
        p[i] = (Winner){.id = strdup(id), .total = INT64_MIN};
        ctx->n++;
      }
      Winner *w = &ctx->winners[i];
      if (total >= w->total) {
        if (w->doc)
          yyjson_mut_doc_free(w->doc);
        w->doc = d;
        w->usage = u;
        w->total = total;
        local_day(get(e, "timestamp"), ctx->now, w->day);
        d = NULL;
      }
    }
    if (d)
      yyjson_mut_doc_free(d);
  }
  free(parent);
  free(line);
  fclose(f);
}
Val *scan_grok(Doc *d, const char *root, double now) {
  GrokScan ctx = {.now = now};
  walk(root, "updates.jsonl", grok_file, &ctx);
  Stats stats = {0};
  init_stats(&stats, now);
  for (size_t i = 0; i < ctx.n; i++) {
    Winner *w = &ctx.winners[i];
    int64_t prompts = number(get(w->usage, "numTurns"));
    if (prompts < 1)
      prompts = 1;
    Val *models = get(w->usage, "modelUsage"), *k, *v;
    size_t j, n;
    bool emitted = false;
    yyjson_mut_obj_foreach(models, j, n, k, v) {
      if (!yyjson_mut_is_obj(v))
        continue;
      int64_t tokens[4];
      split_grok(v, tokens);
      bool any = false;
      for (size_t x = 0; x < 4; x++)
        if (tokens[x])
          any = true;
      if (!any)
        continue;
      add_model(&stats, w->day, w->id, *text(k) ? text(k) : "grok", tokens,
                emitted ? 0 : prompts);
      emitted = true;
    }
    if (!emitted) {
      int64_t tokens[4];
      split_grok(w->usage, tokens);
      add_model(&stats, w->day, w->id, "grok", tokens, prompts);
    }
    yyjson_mut_doc_free(w->doc);
    free(w->id);
  }
  free(ctx.winners);
  return finish(d, &stats);
}
Val *record(Doc *d, const char *id, const char *name, Val *stats, Allowance a,
            const char *help, bool retry, double now) {
  Val *v = copy(d, stats);
  intput(d, v, "schemaVersion", 1);
  strput(d, v, "id", id);
  strput(d, v, "name", name);
  strput(d, v, "updatedAt", iso_time(d, now, false));
  put(d, v, "ready", yyjson_mut_bool(d, true));
  put(d, v, "hasLocalStats", yyjson_mut_bool(d, true));
  strput(d, v, "tierLabel", a.tier);
  strput(d, v, "usageStatusText", a.status);
  strput(d, v, "authHelpText", help);
  put(d, v, "limits", a.limits ? a.limits : yyjson_mut_arr(d));
  if (retry)
    put(d, v, "retryAdvised", yyjson_mut_bool(d, true));
  return v;
}
static void append(char **s, const char *suffix) {
  size_t n = (*s ? strlen(*s) : 0) + strlen(suffix) + 1;
  char *p = realloc(*s, n);
  if (!p)
    exit(1);
  if (!*s)
    *p = 0;
  *s = p;
  strcat(p, suffix);
}
Val *waybar(Doc *d, Val *records) {
  char *lines = NULL;
  double fullest = -1;
  size_t i, n;
  Val *item;
  yyjson_mut_arr_foreach(records, i, n, item) {
    if (!yyjson_mut_is_obj(item) || !truth(get(item, "id")))
      continue;
    if (lines)
      append(&lines, "\n");
    append(&lines, text(either(get(item, "name"), get(item, "id"))));
    const char *tier = text(get(item, "tierLabel"));
    if (*tier) {
      append(&lines, " · ");
      append(&lines, tier);
    }
    Val *limits = get(item, "limits"), *l;
    size_t j, m;
    yyjson_mut_arr_foreach(limits, j, m, l) {
      if (!yyjson_mut_is_obj(l))
        continue;
      double used = fraction(get(l, "percent"), false);
      if (used < 0)
        continue;
      fullest = fmax(fullest, used);
      append(&lines, " · ");
      Val *title = either(get(l, "title"), get(l, "label"));
      append(&lines, truth(title) ? text(title) : "Limit");
      char percent[32];
      snprintf(percent, sizeof percent, " %.0f%%", nearbyint(used * 100));
      append(&lines, percent);
    }
    if (!yyjson_mut_arr_size(limits)) {
      const char *note =
          text(either(get(item, "usageStatusText"), get(item, "authHelpText")));
      if (*note) {
        append(&lines, " · ");
        append(&lines, note);
      }
    }
  }
  Val *out = yyjson_mut_obj(d);
  strput(d, out, "text", "󱚣");
  strput(d, out, "class", fullest >= 0.9 ? "alarm" : lines ? "ready" : "idle");
  char *tooltip = NULL;
  append(&tooltip, lines ? "Agents\n" : "Agents · collecting usage");
  if (lines)
    append(&tooltip, lines);
  strput(d, out, "tooltip", tooltip);
  free(tooltip);
  free(lines);
  return out;
}
Val *status_records(Doc *d) {
  const char *base = getenv("XDG_STATE_HOME");
  char *fallback = NULL;
  if (!base || !*base)
    base = fallback = join(home_dir(), ".local/state");
  char *root = join(base, "omarchy/agents/usage");
  Val *records = yyjson_mut_arr(d);
  struct dirent **entries = NULL;
  int count = scandir(root, &entries, NULL, alphasort);
  for (int i = 0; i < count; i++) {
    const char *name = entries[i]->d_name;
    size_t n = strlen(name);
    if (n >= 5 && !strcmp(name + n - 5, ".json")) {
      char *path = join(root, name);
      Val *v = load(d, path);
      if (yyjson_mut_is_obj(v))
        yyjson_mut_arr_append(records, v);
      free(path);
    }
    free(entries[i]);
  }
  free(entries);
  free(root);
  free(fallback);
  return records;
}
