#include "run.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static const char *fixture_bin, *bin_dir, *openssl_bin;
static char *http_log;
static void expect(int cond, const char *what) {
  if (!cond)
    check_fail("%s", what);
}
static char *which(const char *name) {
  if (name[0] == '/')
    return access(name, X_OK) == 0 ? strdup(name) : NULL;
  const char *path = getenv("PATH");
  if (!path)
    return NULL;
  char *copy = strdup(path);
  for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir;
       dir = strtok_r(NULL, ":", &save)) {
    char *full = check_join(dir, name);
    if (full && access(full, X_OK) == 0) {
      free(copy);
      return full;
    }
    free(full);
  }
  free(copy);
  return NULL;
}
static char *fmt(const char *spec, const char *value) {
  size_t n = strlen(spec) + strlen(value) + 1;
  char *out = malloc(n);
  if (!out)
    return NULL;
  snprintf(out, n, spec, value);
  return out;
}
static yyjson_doc *null_doc(void) {
  yyjson_doc *doc = yyjson_read("null", 4, 0);
  if (!doc)
    exit(1);
  return doc;
}
static yyjson_doc *call_op(const char *op, const char *fields, const char *const *env,
                           int timeout) {
  char *input = NULL;
  size_t len = 0;
  FILE *mem = open_memstream(&input, &len);
  if (!mem)
    return null_doc();
  fputs("{\"now\":1790683200", mem);
  if (fields && *fields) {
    fputc(',', mem);
    fputs(fields, mem);
  }
  fputs("}\n", mem);
  fclose(mem);
  const char *args[] = {fixture_bin, op, NULL};
  const char *fallback[] = {"TZ=UTC", "OPENCODE_GO_API_KEY=", NULL};
  Proc proc = check_run(fixture_bin, args, input, len, env ? env : fallback, timeout);
  free(input);
  yyjson_doc *doc = proc.exited && proc.status == 0 ? parse_json(proc.out, proc.out_len) : NULL;
  if (!doc)
    check_fail("%s status %d: %s", op, proc.status, proc.err ? (char *)proc.err : "");
  proc_free(&proc);
  return doc ? doc : null_doc();
}
static int is_str(yyjson_val *value, const char *text) {
  size_t n = 0;
  const char *got = jstr(value, &n);
  return got && n == strlen(text) && !memcmp(got, text, n);
}
static int is_num(yyjson_val *value, double expected) {
  if (!value || !yyjson_is_num(value))
    return 0;
  double got = yyjson_get_num(value);
  double delta = got - expected;
  if (delta < 0)
    delta = -delta;
  double scale = expected < 0 ? -expected : expected;
  return delta <= 1e-9 || delta <= 1e-9 * scale;
}
static int has_text_ci(const char *text, const char *needle) {
  size_t n = strlen(needle);
  for (const char *p = text; *p; p++) {
    size_t i = 0;
    for (; i < n; i++) {
      char c = p[i];
      if (c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
      if (c != needle[i])
        break;
    }
    if (i == n)
      return 1;
  }
  return 0;
}
static char *b64url(const unsigned char *data, size_t len) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  char *out = malloc((len + 2) / 3 * 4 + 1);
  size_t at = 0;
  for (size_t i = 0; i < len; i += 3) {
    unsigned n = (unsigned)data[i] << 16;
    if (i + 1 < len)
      n |= (unsigned)data[i + 1] << 8;
    if (i + 2 < len)
      n |= data[i + 2];
    out[at++] = tbl[(n >> 18) & 63];
    out[at++] = tbl[(n >> 12) & 63];
    if (i + 1 < len)
      out[at++] = tbl[(n >> 6) & 63];
    if (i + 2 < len)
      out[at++] = tbl[n & 63];
  }
  out[at] = 0;
  return out;
}
static void save_json(const char *path, const char *json) {
  char *parent = strdup(path);
  char *slash = strrchr(parent, '/');
  if (slash) {
    *slash = 0;
    check_mkdir_p(parent);
  }
  free(parent);
  check_write(path, json, strlen(json));
}
typedef struct {
  char *root, *agent, *state;
  char *slots[6];
  const char *env[7];
} Box;
static Box make_box(const char *key) {
  Box box = {.root = check_temp("usage-test-")};
  box.agent = check_join(box.root, "agent");
  box.state = check_join(box.root, "state");
  check_mkdir_p(box.agent);
  check_mkdir_p(box.state);
  box.slots[0] = fmt("HOME=%s", box.root);
  box.slots[1] = fmt("PI_AGENT_DIR=%s", box.agent);
  box.slots[2] = fmt("XDG_STATE_HOME=%s", box.state);
  box.slots[3] = strdup("TZ=UTC");
  box.slots[4] = fmt("OPENCODE_GO_API_KEY=%s", key ? key : "");
  for (int i = 0; i < 5; i++)
    box.env[i] = box.slots[i];
  return box;
}
static void free_box(Box *box) {
  check_rm_rf(box->root);
  free(box->root);
  free(box->agent);
  free(box->state);
  for (int i = 0; i < 6; i++)
    free(box->slots[i]);
}
static int only_name(const char *dir, const char *name) {
  DIR *handle = opendir(dir);
  if (!handle)
    return 0;
  int found = 0, extra = 0;
  struct dirent *entry;
  while ((entry = readdir(handle))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (!strcmp(entry->d_name, name))
      found = 1;
    else
      extra = 1;
  }
  closedir(handle);
  return found && !extra;
}
static char *form_value(const char *form, const char *key) {
  size_t key_len = strlen(key);
  const char *p = form ? form : "";
  while (*p) {
    const char *amp = strchr(p, '&');
    size_t n = amp ? (size_t)(amp - p) : strlen(p);
    if (n > key_len && !strncmp(p, key, key_len) && p[key_len] == '=') {
      char *raw = malloc(n - key_len);
      memcpy(raw, p + key_len + 1, n - key_len - 1);
      raw[n - key_len - 1] = 0;
      char *out = malloc(strlen(raw) + 1);
      size_t at = 0;
      for (char *q = raw; *q; q++) {
        if (*q == '%' && q[1] && q[2]) {
          int hi = q[1], lo = q[2];
          int hv = hi >= '0' && hi <= '9' ? hi - '0' : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10
                                                     : hi >= 'A' && hi <= 'F'   ? hi - 'A' + 10
                                                                                : -1;
          int lv = lo >= '0' && lo <= '9' ? lo - '0' : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10
                                                     : lo >= 'A' && lo <= 'F'   ? lo - 'A' + 10
                                                                                : -1;
          if (hv >= 0 && lv >= 0) {
            out[at++] = (char)(hv * 16 + lv);
            q += 2;
            continue;
          }
        }
        out[at++] = *q == '+' ? ' ' : *q;
      }
      out[at] = 0;
      free(raw);
      return out;
    }
    p += n;
    if (*p == '&')
      p++;
  }
  return NULL;
}
static void scales(void) {
  check_begin("numbers, fractions and provider percentages retain their distinct scales");
  const char *numbers[] = {"\"value\":null", "\"value\":\"bad\"", "\"value\":\"2.5\"",
                           "\"value\":3.5", "\"value\":true"};
  const double expected_n[] = {0, 0, 2, 4, 1};
  for (size_t i = 0; i < 5; i++) {
    yyjson_doc *doc = call_op("number", numbers[i], NULL, 10);
    expect(is_num(yyjson_doc_get_root(doc), expected_n[i]), numbers[i]);
    yyjson_doc_free(doc);
  }
  const char *fractions[] = {"\"value\":1", "\"value\":4", "\"value\":\"40%\"", "\"value\":\"bad\"",
                             "\"value\":-1", "\"value\":101", "\"value\":null"};
  const double expected_f[] = {1, 0.04, 0.4, -1, -1, 1, -1};
  for (size_t i = 0; i < 7; i++) {
    yyjson_doc *doc = call_op("fraction", fractions[i], NULL, 10);
    expect(is_num(yyjson_doc_get_root(doc), expected_f[i]), fractions[i]);
    yyjson_doc_free(doc);
  }
  yyjson_doc *doc = call_op("fraction", "\"value\":1,\"percent\":true", NULL, 10);
  expect(is_num(yyjson_doc_get_root(doc), 0.01), "percent");
  yyjson_doc_free(doc);
}
static void whitespace(void) {
  check_begin("configured keys, provider labels and numeric/date text retain whitespace handling");
  struct {
    const char *op, *fields, *text;
    double number;
    int textual;
  } rows[] = {
      {"command", "\"value\":\"\\u00a0fixture-key\\u00a0\",\"timeout_ms\":1000", "fixture-key", 0, 1},
      {"command", "\"value\":\"!printf '\\\\302\\\\240fixture-key\\\\302\\\\240'\",\"timeout_ms\":1000",
       "fixture-key", 0, 1},
      {"number", "\"value\":\"\\u00a02.5\\u00a0\"", NULL, 2, 0},
      {"fraction", "\"value\":\"40%\\u00a0\"", NULL, 0.4, 0},
      {"timestamp", "\"value\":\"\\u00a01791050635\\u00a0\"", "2026-10-03T18:03:55+00:00", 0, 1},
      {"day", "\"value\":\"\\u00a01791050635\\u00a0\"", "2026-10-03", 0, 1},
      {"day", "\"value\":\"\\u00a02026-10-03T12:00:00Z\\u00a0\"", "2026-10-03", 0, 1},
      {"number", "\"value\":\"\\u001c2\\u001c\"", NULL, 0, 0},
      {"tier", "\"value\":{\"tier\":\"\\u001c5\\u001c\"}", "", 0, 1}};
  for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
    yyjson_doc *doc = call_op(rows[i].op, rows[i].fields, NULL, 10);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (rows[i].textual)
      expect(is_str(root, rows[i].text), rows[i].op);
    else
      expect(is_num(root, rows[i].number), rows[i].op);
    yyjson_doc_free(doc);
  }
  yyjson_doc *codex = call_op("codex", "\"value\":{\"plan_type\":\"\\u00a0pro_plus\\u00a0\"}", NULL, 10);
  expect(is_str(jget(yyjson_doc_get_root(codex), "plan"), "Pro Plus"), "pro plus");
  yyjson_doc_free(codex);
  yyjson_doc *tier = call_op("tier", "\"value\":{\"plan\":\"\\u00a0go_plus\\u00a0\"}", NULL, 10);
  expect(is_str(yyjson_doc_get_root(tier), "go plus"), "go plus");
  yyjson_doc_free(tier);
  tier = call_op("tier", "\"value\":{\"tier\":\" 5 \"}", NULL, 10);
  expect(is_str(yyjson_doc_get_root(tier), "SuperGrok Heavy"), "heavy");
  yyjson_doc_free(tier);
}
static void timestamps(void) {
  check_begin("timestamps, milliseconds, offsets, malformed dates and recent calendar days");
  yyjson_doc *doc = call_op("day", "\"value\":1790683200000", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "2026-09-29"), "ms day");
  yyjson_doc_free(doc);
  doc = call_op("day", "\"value\":\"1790683200\"", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "2026-09-29"), "string day");
  yyjson_doc_free(doc);
  doc = call_op("day", "\"value\":\"2026-09-30T00:30:00+02:00\"", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "2026-09-29"), "offset day");
  yyjson_doc_free(doc);
  const char *bad[] = {"\"value\":null", "\"value\":\"\"", "\"value\":\"bad\"",
                       "\"value\":\"2026-02-30\""};
  for (size_t i = 0; i < 4; i++) {
    doc = call_op("day", bad[i], NULL, 10);
    expect(is_str(yyjson_doc_get_root(doc), "2026-09-29"), bad[i]);
    yyjson_doc_free(doc);
  }
  doc = call_op("timestamp", "\"value\":1791050635", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "2026-10-03T18:03:55+00:00"), "unix");
  yyjson_doc_free(doc);
  doc = call_op("timestamp", "\"value\":1790683200000", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "2026-09-29T12:00:00+00:00"), "ms stamp");
  yyjson_doc_free(doc);
  doc = call_op("timestamp", "\"value\":\"unchanged\"", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "unchanged"), "unchanged");
  yyjson_doc_free(doc);
  doc = call_op("empty", "", NULL, 10);
  yyjson_val *days = jget(yyjson_doc_get_root(doc), "recentDays");
  const char *dates[] = {"2026-09-23", "2026-09-24", "2026-09-25", "2026-09-26",
                         "2026-09-27", "2026-09-28", "2026-09-29"};
  expect(yyjson_arr_size(days) == 7, "day count");
  for (size_t i = 0; i < 7; i++)
    expect(is_str(jget(yyjson_arr_get(days, i), "date"), dates[i]), dates[i]);
  yyjson_doc_free(doc);
}
static const char *event_json =
    "{\"timestamp\":1790683200000,\"message\":{\"role\":\"assistant\",\"provider\":\"%s\","
    "\"model\":\"%s\",\"timestamp\":1790683200000,\"usage\":{\"input\":10,\"output\":2,"
    "\"reasoning\":3,\"cacheRead\":1,\"cacheWrite\":0},\"content\":\"PRIVATE MESSAGE MUST NOT APPEAR\"}}";
static void pi_scan(void) {
  check_begin("Pi scans only matching assistant usage, includes reasoning and never message text");
  char *root = check_temp("usage-test-");
  char *dir = check_join(root, "nested");
  check_mkdir_p(dir);
  char *path = check_join(dir, "session.jsonl");
  char first[512], second[512];
  snprintf(first, sizeof first, event_json, "openai-codex", "gpt-6-astra");
  snprintf(second, sizeof second, event_json, "xai", "grok-4.7");
  char *body = NULL;
  size_t len = 0;
  FILE *mem = open_memstream(&body, &len);
  fprintf(mem, "%s\n%s\n", first, second);
  fputs("{\"message\":{\"role\":\"user\",\"provider\":\"openai-codex\",\"model\":\"gpt-6-astra\","
        "\"timestamp\":1790683200000,\"usage\":{\"input\":10,\"output\":2,\"reasoning\":3,"
        "\"cacheRead\":1,\"cacheWrite\":0},\"content\":\"PRIVATE MESSAGE MUST NOT APPEAR\"}}\n",
        mem);
  fputs("[]\n{}\n{\"message\":{\"provider\":\"openai-codex\",\"model\":\"zero\"}}\nmalformed\n", mem);
  fclose(mem);
  check_write(path, body, len);
  free(body);
  char fields[512];
  snprintf(fields, sizeof fields, "\"path\":\"%s\",\"provider\":\"openai-codex\"", root);
  const char *env[] = {"TZ=UTC", "OPENCODE_GO_API_KEY=", NULL};
  yyjson_doc *doc = call_op("pi", fields, env, 10);
  yyjson_val *stats = yyjson_doc_get_root(doc);
  expect(is_num(jget(stats, "totalPrompts"), 2), "prompts");
  expect(is_num(jget(stats, "totalSessions"), 1), "sessions");
  expect(is_num(jget(stats, "todayTotalTokens"), 16), "tokens");
  expect(is_num(jget(jget(jget(stats, "modelUsage"), "gpt-6-astra"), "outputTokens"), 5), "output");
  expect(is_num(jget(yyjson_arr_get(jget(stats, "recentDays"), 6), "messageCount"), 16), "messages");
  expect(is_num(jget(stats, "todaySessions"), 1), "today sessions");
  yyjson_val *dates = jget(stats, "activeDates");
  expect(yyjson_arr_size(dates) == 1 && is_str(yyjson_arr_get(dates, 0), "2026-09-29"), "dates");
  char *encoded = yyjson_val_write(stats, 0, NULL);
  expect(encoded && !strstr(encoded, "PRIVATE MESSAGE"), "private");
  free(encoded);
  yyjson_doc_free(doc);
  snprintf(fields, sizeof fields, "\"path\":\"%s\",\"provider\":\"xai\"", root);
  doc = call_op("pi", fields, env, 10);
  expect(is_num(jget(yyjson_doc_get_root(doc), "totalPrompts"), 1), "xai");
  yyjson_doc_free(doc);
  char *missing = check_join(root, "missing");
  snprintf(fields, sizeof fields, "\"path\":\"%s\",\"provider\":\"xai\"", missing);
  doc = call_op("pi", fields, env, 10);
  expect(is_num(jget(yyjson_doc_get_root(doc), "totalSessions"), 0), "missing");
  yyjson_doc_free(doc);
  free(missing);
  char *loop = check_join(root, "loop");
  expect(symlink(root, loop) == 0, "symlink");
  snprintf(fields, sizeof fields, "\"path\":\"%s\",\"provider\":\"xai\"", root);
  doc = call_op("pi", fields, env, 10);
  expect(is_num(jget(yyjson_doc_get_root(doc), "totalPrompts"), 1), "loop");
  yyjson_doc_free(doc);
  free(loop);
  const char *pre = "{\"message\":{\"provider\":\"openai-codex\",\"model\":\"gpt-";
  const char *post = "\",\"usage\":{\"input\":1}}}\n";
  unsigned char raw[128];
  size_t n = 0;
  memcpy(raw, pre, strlen(pre));
  n += strlen(pre);
  raw[n++] = 0xff;
  memcpy(raw + n, post, strlen(post));
  n += strlen(post);
  check_write(path, raw, n);
  snprintf(fields, sizeof fields, "\"path\":\"%s\",\"provider\":\"openai-codex\"", root);
  doc = call_op("pi", fields, env, 10);
  yyjson_val *models = jget(yyjson_doc_get_root(doc), "modelUsage");
  const char *key = "gpt-\xef\xbf\xbd";
  expect(is_num(jget(jget(models, key), "inputTokens"), 1), "replacement model");
  yyjson_doc_free(doc);
  free(path);
  free(dir);
  check_rm_rf(root);
  free(root);
}
static void grok_scan(void) {
  check_begin("Grok keeps the largest session snapshot, subtracts cached input and counts turns once across models");
  char *root = check_temp("usage-test-");
  char *dir = check_join(root, "demo/session-a");
  check_mkdir_p(dir);
  char *path = check_join(dir, "updates.jsonl");
  const char *fmt_line =
      "{\"timestamp\":1790683200,\"params\":{\"sessionId\":\"session-a\",\"update\":{"
      "\"sessionUpdate\":\"turn_completed\",\"usage\":{\"totalTokens\":%d,\"numTurns\":%d,"
      "\"inputTokens\":%d,\"outputTokens\":%d,\"cachedReadTokens\":%d,\"modelUsage\":{"
      "\"grok-4.5\":{\"inputTokens\":%d,\"outputTokens\":%d,\"cachedReadTokens\":%d}}}}}}";
  char a[640], b[640], c[640];
  snprintf(a, sizeof a, fmt_line, 100, 1, 80, 20, 10, 80, 20, 10);
  snprintf(b, sizeof b, fmt_line, 250, 2, 200, 50, 40, 200, 50, 40);
  snprintf(c, sizeof c, fmt_line, 110, 1, 90, 20, 5, 90, 20, 5);
  char *body = NULL;
  size_t len = 0;
  FILE *mem = open_memstream(&body, &len);
  fprintf(mem, "%s\n%s\n%s\n", a, b, c);
  fputs("{\"params\":{\"update\":{\"sessionUpdate\":\"tool_call\",\"usage\":{\"totalTokens\":9999}}}}",
        mem);
  fclose(mem);
  check_write(path, body, len);
  free(body);
  char fields[512];
  snprintf(fields, sizeof fields, "\"path\":\"%s\"", root);
  const char *env[] = {"TZ=UTC", "OPENCODE_GO_API_KEY=", NULL};
  yyjson_doc *doc = call_op("grok", fields, env, 10);
  yyjson_val *stats = yyjson_doc_get_root(doc);
  expect(is_num(jget(stats, "totalSessions"), 1), "sessions");
  expect(is_num(jget(stats, "totalPrompts"), 2), "prompts");
  expect(is_num(jget(jget(jget(stats, "modelUsage"), "grok-4.5"), "inputTokens"), 160), "input");
  yyjson_doc_free(doc);
  const char *fallback =
      "{\"timestamp\":1790683200,\"params\":{\"update\":{\"sessionUpdate\":\"turn_completed\","
      "\"usage\":{\"inputTokens\":5,\"cachedReadTokens\":9,\"numTurns\":3}}}}";
  check_write(path, fallback, strlen(fallback));
  doc = call_op("grok", fields, env, 10);
  stats = yyjson_doc_get_root(doc);
  expect(is_num(jget(stats, "totalPrompts"), 3), "fallback prompts");
  expect(is_num(jget(jget(jget(stats, "modelUsage"), "grok"), "inputTokens"), 5), "fallback input");
  yyjson_doc_free(doc);
  free(path);
  free(dir);
  check_rm_rf(root);
  free(root);
}
static void opencode_scan(void) {
  check_begin("OpenCode storage scans include only Go models/providers and all token buckets");
  char *root = check_temp("usage-test-");
  char *go = check_join(root, "message/ses/go.json");
  char *router = check_join(root, "message/ses/router.json");
  save_json(go,
            "{\"role\":\"assistant\",\"providerID\":\"opencode\",\"modelID\":\"deepseek-v4-flash\","
            "\"sessionID\":\"ses\",\"time\":{\"created\":1790683200000},\"tokens\":{\"input\":10,"
            "\"output\":4,\"reasoning\":1,\"cache\":{\"read\":2,\"write\":3}}}");
  save_json(router,
            "{\"role\":\"assistant\",\"providerID\":\"openrouter\",\"modelID\":\"other\",\"tokens\":{\"input\":100}}");
  char fields[512];
  snprintf(fields, sizeof fields, "\"path\":\"%s\"", root);
  const char *env[] = {"TZ=UTC", "OPENCODE_GO_API_KEY=", NULL};
  yyjson_doc *doc = call_op("opencode", fields, env, 10);
  yyjson_val *stats = yyjson_doc_get_root(doc);
  expect(is_num(jget(stats, "totalPrompts"), 1), "prompts");
  expect(is_num(jget(jget(jget(stats, "modelUsage"), "deepseek-v4-flash"), "outputTokens"), 5),
         "output");
  expect(is_num(jget(stats, "todayTotalTokens"), 20), "tokens");
  yyjson_doc_free(doc);
  free(go);
  free(router);
  check_rm_rf(root);
  free(root);
}
static void billing(void) {
  check_begin("Grok billing reads weekly/monthly pool, separate Build share and proto3 omitted zero");
  const int percents[] = {0, 1, 1, 2, 32, 100, 150};
  const char *end = "2026-10-08T20:25:49+00:00";
  for (size_t i = 0; i < 7; i++) {
    char fields[640];
    snprintf(fields, sizeof fields,
             "\"value\":{\"config\":{\"creditUsagePercent\":%d,\"currentPeriod\":{\"type\":"
             "\"USAGE_PERIOD_TYPE_WEEKLY\",\"end\":\"%s\"},\"productUsage\":[{\"product\":"
             "\"GrokBuild\",\"usagePercent\":%d},{\"product\":\"GrokChat\"}]}}",
             percents[i], end, percents[i]);
    yyjson_doc *doc = call_op("billing", fields, NULL, 10);
    yyjson_val *limits = yyjson_doc_get_root(doc);
    double expect_pct = percents[i] / 100.0 > 1 ? 1 : percents[i] / 100.0;
    expect(is_num(jget(yyjson_arr_get(limits, 0), "percent"), expect_pct), "weekly percent");
    expect(is_num(jget(yyjson_arr_get(limits, 1), "percent"), expect_pct), "build percent");
    expect(is_str(jget(yyjson_arr_get(limits, 0), "title"), "Weekly"), "weekly");
    expect(is_str(jget(yyjson_arr_get(limits, 1), "title"), "Grok Build"), "build");
    yyjson_doc_free(doc);
  }
  char omitted[512];
  snprintf(omitted, sizeof omitted,
           "\"value\":{\"config\":{\"currentPeriod\":{\"end\":\"%s\"},\"onDemandCap\":{\"val\":0},"
           "\"isUnifiedBillingUser\":true}}",
           end);
  yyjson_doc *doc = call_op("billing", omitted, NULL, 10);
  yyjson_val *row = yyjson_arr_get(yyjson_doc_get_root(doc), 0);
  expect(yyjson_arr_size(yyjson_doc_get_root(doc)) == 1, "omitted length");
  expect(is_num(jget(row, "percent"), 0), "omitted percent");
  expect(is_str(jget(row, "resetsAt"), end), "omitted reset");
  yyjson_doc_free(doc);
  doc = call_op("billing", "\"value\":{\"config\":{\"isUnifiedBillingUser\":true}}", NULL, 10);
  expect(yyjson_arr_size(yyjson_doc_get_root(doc)) == 0, "unified empty");
  yyjson_doc_free(doc);
  snprintf(omitted, sizeof omitted,
           "\"value\":{\"config\":{\"currentPeriod\":{\"end\":\"%s\"},\"creditUsagePercent\":\"nope\"}}",
           end);
  doc = call_op("billing", omitted, NULL, 10);
  expect(yyjson_arr_size(yyjson_doc_get_root(doc)) == 0, "bad percent");
  yyjson_doc_free(doc);
  snprintf(omitted, sizeof omitted,
           "\"value\":{\"current_period\":{\"type\":\"MONTHLY\",\"end\":\"%s\"},\"credit_usage_percent\":50}",
           end);
  doc = call_op("billing", omitted, NULL, 10);
  expect(is_str(jget(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "title"), "Monthly"), "monthly");
  yyjson_doc_free(doc);
}
static void go_windows(void) {
  check_begin("OpenCode windows and upgraded plan normalization remain on the Go record");
  yyjson_doc *doc = call_op(
      "go",
      "\"value\":{\"plan\":\"go plus\",\"usage\":{\"rolling\":{\"percent\":4,\"resetsAt\":\"reset\"},"
      "\"weekly\":{\"percent\":8},\"monthly\":{\"percent\":2}}}",
      NULL, 10);
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *limits = jget(root, "limits");
  expect(is_str(jget(root, "plan"), "Go Plus"), "plan");
  expect(yyjson_arr_size(limits) == 3, "windows");
  expect(is_str(jget(yyjson_arr_get(limits, 0), "title"), "Session"), "session");
  expect(is_str(jget(yyjson_arr_get(limits, 1), "title"), "Weekly"), "weekly");
  expect(is_str(jget(yyjson_arr_get(limits, 2), "title"), "Monthly"), "monthly");
  expect(is_num(jget(yyjson_arr_get(limits, 0), "percent"), 0.04), "percent");
  yyjson_doc_free(doc);
  doc = call_op("go", "\"value\":{\"rolling\":{\"usagePercent\":0.4,\"resetAt\":\"reset\"}}", NULL, 10);
  expect(is_str(jget(yyjson_doc_get_root(doc), "plan"), "Go"), "default plan");
  yyjson_doc_free(doc);
}
static void codex_windows(void) {
  check_begin("Codex windows always use a 0-100 percent scale and readable UTC resets");
  yyjson_doc *doc = call_op(
      "codex",
      "\"value\":{\"plan_type\":\"pro\",\"rate_limit\":{\"primary_window\":{\"used_percent\":1,"
      "\"limit_window_seconds\":604800,\"reset_after_seconds\":3600}}}",
      NULL, 10);
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *limit = yyjson_arr_get(jget(root, "limits"), 0);
  expect(is_str(jget(root, "plan"), "Pro"), "plan");
  expect(is_str(jget(root, "status"), ""), "status");
  expect(is_num(jget(limit, "percent"), 0.01), "percent");
  expect(is_str(jget(limit, "resetsAt"), "2026-09-29T13:00:00+00:00"), "reset");
  yyjson_doc_free(doc);
  doc = call_op("codex",
                "\"value\":{\"rate_limit\":{\"allowed\":false,\"primary_window\":{\"used_percent\":100,"
                "\"limit_window_seconds\":18000,\"reset_at\":1791050635}}}",
                NULL, 10);
  root = yyjson_doc_get_root(doc);
  limit = yyjson_arr_get(jget(root, "limits"), 0);
  size_t n = 0;
  const char *reset = jstr(jget(limit, "resetsAt"), &n);
  expect(is_num(jget(limit, "percent"), 1), "full");
  expect(is_str(jget(limit, "label"), "5h window"), "label");
  expect(is_str(jget(root, "status"), "Rate limit reached"), "reached");
  expect(reset && n >= 11 && !memcmp(reset, "2026-10-03T", 11), "reset prefix");
  yyjson_doc_free(doc);
}
static void waybar(void) {
  check_begin("Waybar retains icon, alarm threshold, subscription lines and missing-data messages");
  yyjson_doc *doc = call_op(
      "waybar",
      "\"value\":[{\"id\":\"codex\",\"name\":\"Codex\",\"tierLabel\":\"Max 20x\",\"limits\":[{"
      "\"title\":\"Session\",\"percent\":0.4}]},{\"id\":\"grok\",\"name\":\"Grok\",\"tierLabel\":"
      "\"SuperGrok\",\"limits\":[{\"title\":\"Weekly\",\"percent\":0.91}]}]",
      NULL, 10);
  yyjson_val *root = yyjson_doc_get_root(doc);
  size_t n = 0;
  const char *tip = jstr(jget(root, "tooltip"), &n);
  expect(is_str(jget(root, "text"), "\xf3\xb1\x9a\xa3"), "icon");
  expect(is_str(jget(root, "class"), "alarm"), "alarm");
  expect(tip && strstr(tip, "Codex \xc2\xb7 Max 20x \xc2\xb7 Session 40%"), "codex line");
  expect(tip && strstr(tip, "Grok \xc2\xb7 SuperGrok \xc2\xb7 Weekly 91%"), "grok line");
  expect(tip && !has_text_ci(tip, "token"), "no token");
  yyjson_doc_free(doc);
  doc = call_op("waybar", "\"value\":[]", NULL, 10);
  root = yyjson_doc_get_root(doc);
  expect(is_str(jget(root, "text"), "\xf3\xb1\x9a\xa3"), "idle icon");
  expect(is_str(jget(root, "class"), "idle"), "idle");
  expect(is_str(jget(root, "tooltip"), "Agents \xc2\xb7 collecting usage"), "idle tip");
  yyjson_doc_free(doc);
  doc = call_op("waybar",
                "\"value\":[{\"id\":\"c\",\"limits\":[],\"authHelpText\":\"Login\"},null,{}]", NULL, 10);
  expect(is_str(jget(yyjson_doc_get_root(doc), "tooltip"), "Agents\nc \xc2\xb7 Login"), "login tip");
  yyjson_doc_free(doc);
}
static void legacy_credentials(void) {
  check_begin("legacy credential helpers retain login order, profile fields, tiers and Go-key preference");
  yyjson_doc *doc = call_op("tier", "\"value\":{\"tier\":5}", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "SuperGrok Heavy"), "tier 5");
  yyjson_doc_free(doc);
  doc = call_op("tier", "\"value\":{\"plan\":\" go_plus \"}", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "go plus"), "plan");
  yyjson_doc_free(doc);
  doc = call_op("login",
                "\"value\":{\"bad\":{},\"expired\":{\"key\":\"old\",\"expires_at\":\"2000-01-01\"},"
                "\"current\":{\"key\":\"fixture\"}}",
                NULL, 10);
  expect(is_str(jget(yyjson_doc_get_root(doc), "key"), "current"), "login");
  yyjson_doc_free(doc);
  doc = call_op("merge_login",
                "\"entry\":{\"key\":\"old\",\"refresh_token\":\"old-refresh\",\"email\":\"kept@example\"},"
                "\"payload\":{\"access_token\":\"new\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}",
                NULL, 10);
  yyjson_val *merged = yyjson_doc_get_root(doc);
  expect(is_str(jget(merged, "key"), "new"), "merged key");
  expect(is_str(jget(merged, "email"), "kept@example"), "email");
  expect(is_str(jget(merged, "expires_at"), "2026-09-29T13:00:00Z"), "expires");
  yyjson_doc_free(doc);
  doc = call_op("key", "\"value\":{\"opencode-go\":{\"key\":\"fixture\"},\"opencode\":\"other\"}", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "fixture"), "key");
  yyjson_doc_free(doc);
  doc = call_op("key", "\"value\":{}", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), ""), "empty key");
  yyjson_doc_free(doc);
}
static void oauth_merge(void) {
  check_begin("Pi token validity preserves refresh skew and rotated fields without losing account or extensions");
  const char *entry =
      "\"value\":{\"type\":\"oauth\",\"access\":\"old\",\"refresh\":\"old\",\"accountId\":\"acct\","
      "\"extension\":{\"keep\":true},\"expires\":1790683400000}";
  char fields[640];
  snprintf(fields, sizeof fields, "%s,\"now_ms\":1790683200000", entry);
  yyjson_doc *doc = call_op("current", fields, NULL, 10);
  expect(yyjson_is_false(yyjson_doc_get_root(doc)), "skew current");
  yyjson_doc_free(doc);
  snprintf(fields, sizeof fields, "%s,\"now_ms\":1790683200000,\"skew_ms\":0", entry);
  doc = call_op("current", fields, NULL, 10);
  expect(yyjson_is_true(yyjson_doc_get_root(doc)), "no skew");
  yyjson_doc_free(doc);
  doc = call_op("merge",
                "\"entry\":{\"type\":\"oauth\",\"access\":\"old\",\"refresh\":\"old\",\"accountId\":\"acct\","
                "\"extension\":{\"keep\":true},\"expires\":1790683400000},\"payload\":{\"access_token\":\"new\","
                "\"refresh_token\":\"newer\",\"expires_in\":3600},\"now_ms\":1790683200000",
                NULL, 10);
  yyjson_val *merged = yyjson_doc_get_root(doc);
  yyjson_val *extension = jget(merged, "extension");
  expect(is_str(jget(merged, "access"), "new"), "access");
  expect(is_str(jget(merged, "refresh"), "newer"), "refresh");
  expect(is_str(jget(merged, "accountId"), "acct"), "account");
  expect(yyjson_obj_size(extension) == 1 && yyjson_is_true(jget(extension, "keep")), "extension");
  expect(is_num(jget(merged, "expires"), 1790686500000), "expires");
  yyjson_doc_free(doc);
}
static void jwt(void) {
  check_begin("JWT claims accept unpadded URL-safe payloads and reject malformed content");
  const char *claims = "{\"principal_id\":\"user\",\"tier\":5}";
  char *payload = b64url((const unsigned char *)claims, strlen(claims));
  char *fields = malloc(strlen(payload) + 64);
  snprintf(fields, strlen(payload) + 64, "\"value\":\"header.%s.signature\"", payload);
  yyjson_doc *doc = call_op("jwt", fields, NULL, 10);
  yyjson_val *root = yyjson_doc_get_root(doc);
  expect(is_str(jget(root, "principal_id"), "user") && is_num(jget(root, "tier"), 5), "claims");
  yyjson_doc_free(doc);
  free(fields);
  free(payload);
  const char *bad[] = {"\"value\":\"\"", "\"value\":\"bad\"", "\"value\":\"x.!?.y\"", "\"value\":\"x.W10.y\""};
  for (size_t i = 0; i < 4; i++) {
    doc = call_op("jwt", bad[i], NULL, 10);
    expect(yyjson_is_obj(yyjson_doc_get_root(doc)) &&
               yyjson_obj_size(yyjson_doc_get_root(doc)) == 0,
           bad[i]);
    yyjson_doc_free(doc);
  }
}
static void commands(void) {
  check_begin("trusted command keys are trimmed, errors suppressed and process groups bounded");
  yyjson_doc *doc = call_op("command", "\"value\":\" plain-key \",\"timeout_ms\":1000", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "plain-key"), "plain");
  yyjson_doc_free(doc);
  doc = call_op("command",
                "\"value\":\"!printf ' fixture-key\\\\n'; printf diagnostic >&2\",\"timeout_ms\":1000",
                NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), "fixture-key"), "diagnostic");
  yyjson_doc_free(doc);
  doc = call_op("command", "\"value\":\"!exit 1\",\"timeout_ms\":1000", NULL, 10);
  expect(is_str(yyjson_doc_get_root(doc), ""), "exit");
  yyjson_doc_free(doc);
  struct timespec start, stop;
  clock_gettime(CLOCK_MONOTONIC, &start);
  doc = call_op("command", "\"value\":\"!sleep 10\",\"timeout_ms\":30", NULL, 10);
  clock_gettime(CLOCK_MONOTONIC, &stop);
  double elapsed = (double)(stop.tv_sec - start.tv_sec) +
                   (double)(stop.tv_nsec - start.tv_nsec) / 1e9;
  expect(is_str(yyjson_doc_get_root(doc), ""), "sleep");
  expect(elapsed < 2, "bounded");
  yyjson_doc_free(doc);
}
static void installed(void) {
  check_begin("all installed command names work offline, including ignored updater flags and exact help");
  Box box = make_box("");
  const char *ids[] = {"codex", "grok", "opencode"};
  for (size_t i = 0; i < 3; i++) {
    char leaf[64];
    snprintf(leaf, sizeof leaf, "omarchy-agent-usage-%s", ids[i]);
    char *bin = check_join(bin_dir, leaf);
    const char *args[] = {bin, "--force", "--limits-only", NULL};
    Proc proc = check_run(bin, args, NULL, 0, box.env, 15);
    expect(proc.exited && proc.status == 0 && proc.err_len == 0, ids[i]);
    yyjson_doc *doc = parse_json(proc.out, proc.out_len);
    yyjson_val *record = doc ? yyjson_doc_get_root(doc) : NULL;
    expect(is_str(jget(record, "id"), ids[i]), "id");
    expect(is_num(jget(record, "schemaVersion"), 1), "schema");
    expect(yyjson_is_true(jget(record, "ready")), "ready");
    expect(yyjson_is_true(jget(record, "hasLocalStats")), "stats");
    expect(is_str(jget(record, "usageStatusText"), "Waiting for auth"), "waiting");
    expect(!jget(record, "retryAdvised"), "no retry");
    yyjson_doc_free(doc);
    proc_free(&proc);
    const char *help_args[] = {bin, "--help", NULL};
    Proc help = check_run(bin, help_args, NULL, 0, box.env, 10);
    char expected[96];
    snprintf(expected, sizeof expected, "usage: omarchy-agent-usage-%s [--force] [--limits-only]\n",
             ids[i]);
    expect(help.status == 0 && help.out_len == 0 && help.err_len == strlen(expected) &&
               !memcmp(help.err, expected, help.err_len),
           "help");
    proc_free(&help);
    free(bin);
  }
  free_box(&box);
}
static void status_exe(void) {
  check_begin("status executable loads sorted object records only from the existing state path");
  Box box = make_box("");
  char *dir = check_join(box.state, "omarchy/agents/usage");
  check_mkdir_p(dir);
  char *b = check_join(dir, "b.json");
  char *a = check_join(dir, "a.json");
  char *array = check_join(dir, "array.json");
  char *bad = check_join(dir, "bad.json");
  save_json(b, "{\"id\":\"grok\",\"name\":\"Grok\",\"limits\":[{\"title\":\"Weekly\",\"percent\":0.9}]}");
  save_json(a, "{\"id\":\"codex\",\"name\":\"Codex\"}");
  save_json(array, "[]");
  check_write(bad, "{", 1);
  char *bin = check_join(bin_dir, "hypr-agent-status");
  const char *args[] = {bin, NULL};
  Proc proc = check_run(bin, args, NULL, 0, box.env, 10);
  yyjson_doc *doc = parse_json(proc.out, proc.out_len);
  yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
  expect(is_str(jget(root, "class"), "alarm"), "alarm");
  expect(is_str(jget(root, "tooltip"), "Agents\nCodex\nGrok \xc2\xb7 Weekly 90%"), "tooltip");
  yyjson_doc_free(doc);
  proc_free(&proc);
  free(bin);
  free(bad);
  free(array);
  free(a);
  free(b);
  free(dir);
  free_box(&box);
}
static yyjson_doc *collect(const char *id, Box *box, const char *replies) {
  char *fields = malloc(strlen(replies) + 64);
  snprintf(fields, strlen(replies) + 64, "\"id\":\"%s\",\"replies\":%s", id, replies);
  yyjson_doc *doc = call_op("collect", fields, box->env, 15);
  free(fields);
  return doc;
}
static void fresh_oauth(void) {
  check_begin("fresh OAuth credentials use fixed usage URLs and required account/JWT headers without refresh");
  Box box = make_box("");
  char *auth = check_join(box.agent, "auth.json");
  save_json(auth,
            "{\"openai-codex\":{\"access\":\"fixture-access\",\"expires\":1790684100000,\"accountId\":\"acct\"}}");
  yyjson_doc *doc = collect(
      "codex", &box,
      "[{\"status\":200,\"body\":{\"plan_type\":\"pro\",\"rate_limit\":{\"primary_window\":{"
      "\"used_percent\":1,\"limit_window_seconds\":604800,\"reset_after_seconds\":3600}}}}]");
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *request = yyjson_arr_get(jget(root, "requests"), 0);
  yyjson_val *record = jget(root, "record");
  char *encoded = yyjson_val_write(record, 0, NULL);
  expect(yyjson_arr_size(jget(root, "requests")) == 1, "one request");
  expect(is_str(jget(request, "url"), "https://chatgpt.com/backend-api/wham/usage"), "url");
  expect(is_str(jget(request, "headers"), "ChatGPT-Account-Id: acct"), "account header");
  expect(is_str(jget(record, "tierLabel"), "Pro"), "tier");
  expect(is_num(jget(yyjson_arr_get(jget(record, "limits"), 0), "percent"), 0.01), "percent");
  expect(encoded && !strstr(encoded, "fixture-access"), "no access");
  free(encoded);
  yyjson_doc_free(doc);
  const char *claims = "{\"principal_id\":\"user\",\"tier\":5}";
  char *payload = b64url((const unsigned char *)claims, strlen(claims));
  char *json = malloc(strlen(payload) + 128);
  snprintf(json, strlen(payload) + 128,
           "{\"xai\":{\"access\":\"h.%s.s\",\"expires\":1790684100000}}", payload);
  save_json(auth, json);
  doc = collect("grok", &box, "[{\"status\":200,\"body\":{\"currentPeriod\":{\"end\":\"reset\"}}}]");
  root = yyjson_doc_get_root(doc);
  request = yyjson_arr_get(jget(root, "requests"), 0);
  record = jget(root, "record");
  size_t n = 0;
  const char *headers = jstr(jget(request, "headers"), &n);
  expect(is_str(jget(record, "tierLabel"), "SuperGrok Heavy"), "grok tier");
  expect(headers && strstr(headers, "x-userid: user"), "userid");
  expect(is_num(jget(yyjson_arr_get(jget(record, "limits"), 0), "percent"), 0), "zero");
  yyjson_doc_free(doc);
  free(json);
  free(payload);
  free(auth);
  free_box(&box);
}
static void persist_refresh(void) {
  check_begin("OAuth refresh persists all profiles atomically at mode 0600 and rotates the bearer used for probing");
  Box box = make_box("");
  char *auth = check_join(box.agent, "auth.json");
  save_json(auth,
            "{\"openai-codex\":{\"type\":\"oauth\",\"access\":\"old\",\"refresh\":\"refresh with &\","
            "\"expires\":1,\"accountId\":\"acct\",\"extra\":42},\"other\":{\"keep\":\"unchanged\"}}");
  yyjson_doc *doc = collect(
      "codex", &box,
      "[{\"status\":200,\"body\":{\"access_token\":\"fixture-new\",\"refresh_token\":\"new-refresh\","
      "\"expires_in\":3600}},{\"status\":200,\"body\":{\"plan_type\":\"pro\",\"rate_limit\":{"
      "\"primary_window\":{\"used_percent\":1,\"limit_window_seconds\":604800,"
      "\"reset_after_seconds\":3600}}}}]");
  yyjson_val *requests = jget(yyjson_doc_get_root(doc), "requests");
  char *token = form_value(jstr(jget(yyjson_arr_get(requests, 0), "form"), NULL), "refresh_token");
  expect(is_str(jget(yyjson_arr_get(requests, 0), "url"), "https://auth.openai.com/oauth/token"),
         "token url");
  expect(token && !strcmp(token, "refresh with &"), "form token");
  expect(is_str(jget(yyjson_arr_get(requests, 1), "token"), "fixture-new"), "rotated");
  yyjson_doc_free(doc);
  free(token);
  size_t n = 0;
  unsigned char *raw = check_read(auth, &n);
  yyjson_doc *stored = parse_json(raw, n);
  yyjson_val *codex = jget(yyjson_doc_get_root(stored), "openai-codex");
  expect(is_str(jget(codex, "accountId"), "acct"), "account kept");
  expect(is_num(jget(codex, "extra"), 42), "extra");
  expect(is_str(jget(jget(yyjson_doc_get_root(stored), "other"), "keep"), "unchanged"), "other");
  expect(check_mode(auth) == 0600, "mode");
  expect(only_name(box.agent, "auth.json"), "atomic");
  yyjson_doc_free(stored);
  free(raw);
  free(auth);
  free_box(&box);
}
static void failed_save(void) {
  check_begin("failed credential publication reports the failure without probing with unsaved tokens");
  expect(getuid() != 0, "not root");
  Box box = make_box("");
  char *auth = check_join(box.agent, "auth.json");
  const char *original =
      "{\"openai-codex\":{\"access\":\"fixture-old\",\"refresh\":\"fixture-refresh\",\"expires\":1}}";
  save_json(auth, original);
  size_t before_len = 0;
  unsigned char *before = check_read(auth, &before_len);
  expect(chmod(box.agent, 0500) == 0, "chmod");
  yyjson_doc *doc = collect(
      "codex", &box,
      "[{\"status\":200,\"body\":{\"access_token\":\"fixture-new\",\"refresh_token\":\"fixture-rotated\","
      "\"expires_in\":3600}}]");
  yyjson_val *record = jget(yyjson_doc_get_root(doc), "record");
  char *encoded = yyjson_val_write(record, 0, NULL);
  size_t n = 0;
  const char *help = jstr(jget(record, "authHelpText"), &n);
  unsigned char *after = check_read(auth, &n);
  expect(yyjson_arr_size(jget(yyjson_doc_get_root(doc), "requests")) == 1, "no probe");
  expect(after && n == before_len && !memcmp(after, before, n), "unchanged");
  expect(is_str(jget(record, "usageStatusText"), "Couldn't save refreshed credentials"), "status");
  expect(help && strstr(help, "writable"), "writable");
  expect(encoded && !strstr(encoded, "fixture-old") && !strstr(encoded, "fixture-new") &&
             !strstr(encoded, "fixture-rotated") && !strstr(encoded, "fixture-refresh"),
         "secrets");
  expect(only_name(box.agent, "auth.json"), "no temp");
  free(encoded);
  free(after);
  yyjson_doc_free(doc);
  chmod(box.agent, 0700);
  free(before);
  free(auth);
  free_box(&box);
}
static void overflow(void) {
  check_begin("allocation size overflow exits with a value-free diagnostic");
  const char *ops[] = {"overflow_add", "overflow_multiply"};
  for (size_t i = 0; i < 2; i++) {
    const char *args[] = {fixture_bin, ops[i], NULL};
    const char *input = "{}\n";
    Proc proc = check_run(fixture_bin, args, input, 3, NULL, 10);
    expect(proc.status == 1 && proc.out_len == 0, ops[i]);
    expect(proc.err_len == strlen("agent-usage: allocation failed\n") &&
               !memcmp(proc.err, "agent-usage: allocation failed\n", proc.err_len),
           "diagnostic");
    proc_free(&proc);
  }
}
static void refresh_fallback(void) {
  check_begin("failed refresh falls back only to a still-valid token, and missing refresh stops expired probes");
  Box box = make_box("");
  char *auth = check_join(box.agent, "auth.json");
  save_json(auth,
            "{\"xai\":{\"access\":\"fixture\",\"refresh\":\"fixture-refresh\",\"expires\":1790683210000}}");
  yyjson_doc *doc =
      collect("grok", &box, "[{\"failed\":true},{\"status\":200,\"body\":{\"currentPeriod\":{\"end\":\"reset\"}}}]");
  expect(yyjson_arr_size(jget(yyjson_doc_get_root(doc), "requests")) == 2, "fallback requests");
  expect(is_str(jget(jget(yyjson_doc_get_root(doc), "record"), "usageStatusText"), ""), "fallback status");
  yyjson_doc_free(doc);
  save_json(auth, "{\"xai\":{\"access\":\"old\",\"refresh\":\"r\",\"expires\":1}}");
  doc = collect("grok", &box, "[{\"failed\":true}]");
  expect(yyjson_arr_size(jget(yyjson_doc_get_root(doc), "requests")) == 1, "expired requests");
  expect(is_str(jget(jget(yyjson_doc_get_root(doc), "record"), "usageStatusText"), "Sign-in expired"),
         "expired");
  yyjson_doc_free(doc);
  save_json(auth, "{\"xai\":{\"access\":\"old\",\"expires\":1}}");
  doc = collect("grok", &box, "[]");
  expect(yyjson_arr_size(jget(yyjson_doc_get_root(doc), "requests")) == 0, "no refresh");
  yyjson_doc_free(doc);
  free(auth);
  free_box(&box);
}
static void go_keys(void) {
  check_begin("Go key precedence is environment, models trusted command, then auth; diagnostics never enter records");
  Box box = make_box("");
  char *models = check_join(box.agent, "models.json");
  char *auth = check_join(box.agent, "auth.json");
  save_json(models,
            "{\"providers\":{\"opencode-go\":{\"apiKey\":\"!printf command-key; printf HIDDEN >&2\"}}}");
  save_json(auth, "{\"opencode-go\":{\"key\":\"auth-key\"}}");
  const char *replies = "[{\"status\":200,\"body\":{\"plan\":\"go plus\",\"rolling\":{\"percent\":0.4}}}]";
  yyjson_doc *doc = collect("opencode", &box, replies);
  yyjson_val *root = yyjson_doc_get_root(doc);
  char *encoded = yyjson_val_write(jget(root, "record"), 0, NULL);
  expect(is_str(jget(yyjson_arr_get(jget(root, "requests"), 0), "token"), "command-key"), "command");
  expect(is_str(jget(jget(root, "record"), "tierLabel"), "Go Plus"), "tier");
  expect(encoded && !strstr(encoded, "HIDDEN"), "hidden");
  free(encoded);
  yyjson_doc_free(doc);
  free(box.slots[4]);
  box.slots[4] = strdup("OPENCODE_GO_API_KEY=env-key");
  box.env[4] = box.slots[4];
  char *fields = malloc(strlen(replies) + 64);
  snprintf(fields, strlen(replies) + 64, "\"id\":\"opencode\",\"replies\":%s", replies);
  doc = call_op("collect", fields, box.env, 15);
  expect(is_str(jget(yyjson_arr_get(jget(yyjson_doc_get_root(doc), "requests"), 0), "token"), "env-key"),
         "env");
  yyjson_doc_free(doc);
  free(fields);
  save_json(models, "{}");
  free(box.slots[4]);
  box.slots[4] = strdup("OPENCODE_GO_API_KEY=");
  box.env[4] = box.slots[4];
  doc = collect("opencode", &box, replies);
  expect(is_str(jget(yyjson_arr_get(jget(yyjson_doc_get_root(doc), "requests"), 0), "token"), "auth-key"),
         "auth");
  yyjson_doc_free(doc);
  free(models);
  free(auth);
  free_box(&box);
}
static void http_errors(void) {
  check_begin("HTTP status, network errors and missing allowances retain useful local stats and retry semantics");
  Box box = make_box("");
  char *auth = check_join(box.agent, "auth.json");
  char *sessions = check_join(box.agent, "sessions");
  check_mkdir_p(sessions);
  char *session = check_join(sessions, "s.jsonl");
  char event[512];
  snprintf(event, sizeof event, event_json, "openai-codex", "gpt-6-astra");
  size_t n = strlen(event);
  char *line = malloc(n + 2);
  memcpy(line, event, n);
  line[n] = '\n';
  line[n + 1] = 0;
  check_write(session, line, n + 1);
  free(line);
  save_json(auth, "{\"openai-codex\":{\"access\":\"fixture\",\"expires\":1790684100000}}");
  yyjson_doc *doc = collect("codex", &box, "[{\"status\":401}]");
  yyjson_val *record = jget(yyjson_doc_get_root(doc), "record");
  expect(is_str(jget(record, "authHelpText"), "Codex usage returned status 401."), "401");
  expect(!jget(record, "retryAdvised"), "no retry");
  expect(is_num(jget(record, "totalPrompts"), 1), "prompts");
  yyjson_doc_free(doc);
  doc = collect("codex", &box, "[{\"failed\":true}]");
  record = jget(yyjson_doc_get_root(doc), "record");
  expect(yyjson_is_true(jget(record, "retryAdvised")), "retry");
  expect(is_num(jget(record, "totalPrompts"), 1), "failed prompts");
  yyjson_doc_free(doc);
  doc = collect("codex", &box, "[{\"status\":200,\"body\":{}}]");
  expect(is_str(jget(jget(yyjson_doc_get_root(doc), "record"), "authHelpText"),
                "Codex usage returned no allowance."),
         "allowance");
  yyjson_doc_free(doc);
  save_json(auth, "{\"opencode-go\":\"fixture-key\"}");
  doc = collect("opencode", &box, "[{\"status\":403}]");
  expect(is_str(jget(jget(yyjson_doc_get_root(doc), "record"), "authHelpText"),
                "This OpenCode key has no Go subscription."),
         "403");
  yyjson_doc_free(doc);
  free(session);
  free(sessions);
  free(auth);
  free_box(&box);
}
static void send_all(int fd, const void *data, size_t len) {
  const unsigned char *p = data;
  size_t at = 0;
  while (at < len) {
    ssize_t n = write(fd, p + at, len - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return;
    at += (size_t)n;
  }
}
static int read_headers(int fd, char *buf, size_t cap, size_t *used) {
  size_t n = 0;
  while (n + 1 < cap) {
    ssize_t got = read(fd, buf + n, cap - 1 - n);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      break;
    n += (size_t)got;
    buf[n] = 0;
    if (strstr(buf, "\r\n\r\n")) {
      *used = n;
      return 0;
    }
  }
  *used = n;
  return -1;
}
static const char *header_value(const char *headers, const char *name) {
  size_t n = strlen(name);
  for (const char *p = headers; *p; p++) {
    if ((p == headers || p[-1] == '\n') && !strncasecmp(p, name, n) && p[n] == ':') {
      p += n + 1;
      while (*p == ' ')
        p++;
      return p;
    }
  }
  return NULL;
}
static void log_seen(const char *path, const char *auth, const char *type) {
  char line[1024];
  int wrote = snprintf(line, sizeof line, "{\"path\":\"%s\",\"auth\":\"%s\",\"type\":\"%s\"}\n", path,
                       auth ? auth : "", type ? type : "");
  if (wrote < 0 || (size_t)wrote >= sizeof line || !http_log)
    return;
  int fd = open(http_log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0) {
    send_all(fd, line, (size_t)wrote);
    close(fd);
  }
}
static void copy_token(const char *value, char *out, size_t cap) {
  size_t n = 0;
  if (!value) {
    out[0] = 0;
    return;
  }
  while (*value && *value != '\r' && *value != '\n' && n + 1 < cap)
    out[n++] = *value++;
  out[n] = 0;
}
static void handle_http(int fd, int slow) {
  int nodelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
  char headers[8192];
  size_t used = 0;
  if (read_headers(fd, headers, sizeof headers, &used)) {
    close(fd);
    return;
  }
  char *start = headers;
  if (!strncmp(start, "GET ", 4) || !strncmp(start, "POST ", 5))
    start = strchr(start, ' ') + 1;
  char *end = strchr(start, ' ');
  char path[256] = "/";
  if (end && (size_t)(end - start) < sizeof path) {
    memcpy(path, start, (size_t)(end - start));
    path[end - start] = 0;
  }
  char auth[256], type[128];
  copy_token(header_value(headers, "authorization"), auth, sizeof auth);
  copy_token(header_value(headers, "content-type"), type, sizeof type);
  log_seen(path, auth, type);
  const char *length = header_value(headers, "content-length");
  long body_len = length ? strtol(length, NULL, 10) : 0;
  char *body = body_len > 0 && body_len < 1000000 ? malloc((size_t)body_len + 1) : NULL;
  size_t have = 0;
  char *split = strstr(headers, "\r\n\r\n");
  if (body && split) {
    size_t already = used - (size_t)(split + 4 - headers);
    if (already > (size_t)body_len)
      already = (size_t)body_len;
    memcpy(body, split + 4, already);
    have = already;
    while (have < (size_t)body_len) {
      ssize_t n = read(fd, body + have, (size_t)body_len - have);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      have += (size_t)n;
    }
    body[have] = 0;
  }
  if (slow) {
    const char *head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n";
    send_all(fd, head, strlen(head));
    sleep(9);
    send_all(fd, "{\"ok\":", 6);
    sleep(9);
    send_all(fd, "true}", 5);
  } else if (!strcmp(path, "/redirect")) {
    const char *resp = "HTTP/1.1 302 Found\r\nLocation: /capture\r\nContent-Length: 0\r\n"
                       "Connection: close\r\n\r\n";
    send_all(fd, resp, strlen(resp));
  } else if (!strcmp(path, "/error")) {
    const char *resp = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 100\r\n"
                       "Connection: close\r\n\r\n";
    send_all(fd, resp, strlen(resp));
  } else if (!strcmp(path, "/invalid")) {
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
                       "Connection: close\r\n\r\n[]";
    send_all(fd, resp, strlen(resp));
  } else if (!strcmp(path, "/nul")) {
    const char payload[] = "{\"ok\":true}\0trailing";
    char head[128];
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         sizeof payload - 1);
    send_all(fd, head, (size_t)wrote);
    send_all(fd, payload, sizeof payload - 1);
  } else if (!strcmp(path, "/large")) {
    const char *prefix = "{\"padding\":\"";
    const char *suffix = "\"}";
    size_t pad = 9 * 1024 * 1024;
    size_t total = strlen(prefix) + pad + strlen(suffix);
    char head[160];
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         total);
    send_all(fd, head, (size_t)wrote);
    send_all(fd, prefix, strlen(prefix));
    char block[4096];
    memset(block, 'x', sizeof block);
    size_t left = pad;
    while (left) {
      size_t n = left < sizeof block ? left : sizeof block;
      send_all(fd, block, n);
      left -= n;
    }
    send_all(fd, suffix, strlen(suffix));
  } else {
    const char *received = body ? body : "";
    char *payload = malloc(strlen(received) + 32);
    snprintf(payload, strlen(received) + 32, "{\"received\":\"%s\"}", received);
    char head[160];
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         strlen(payload));
    send_all(fd, head, (size_t)wrote);
    send_all(fd, payload, strlen(payload));
    free(payload);
  }
  free(body);
  close(fd);
}
static int start_server(int slow, int *port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 16))
    return -1;
  socklen_t len = sizeof addr;
  getsockname(fd, (struct sockaddr *)&addr, &len);
  *port = ntohs(addr.sin_port);
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (!pid) {
    setpgid(0, 0);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
      int client = accept(fd, NULL, NULL);
      if (client < 0)
        continue;
      pid_t worker = fork();
      if (!worker) {
        handle_http(client, slow);
        _exit(0);
      }
      close(client);
    }
  }
  setpgid(pid, pid);
  close(fd);
  return pid;
}
static yyjson_doc *probe(const char *json, const char *const *env) {
  const char *args[] = {fixture_bin, "probe", NULL};
  Proc proc = check_run(fixture_bin, args, json, strlen(json), env, 40);
  yyjson_doc *doc = proc.exited && proc.status == 0 ? parse_json(proc.out, proc.out_len) : NULL;
  if (!doc)
    check_fail("probe status %d: %s", proc.status, proc.err ? (char *)proc.err : "");
  proc_free(&proc);
  return doc ? doc : null_doc();
}
static int log_lines(void) {
  size_t n = 0;
  unsigned char *data = check_read(http_log, &n);
  int count = 0;
  if (data)
    for (size_t i = 0; i < n; i++)
      if (data[i] == '\n')
        count++;
  free(data);
  return count;
}
static void http_transport(void) {
  check_begin("real HTTP transport sends bearer/form data, rejects bad JSON, preserves large responses and never follows redirects");
  char *root = check_temp("usage-http-");
  http_log = check_join(root, "seen.log");
  check_write(http_log, "", 0);
  int port = 0;
  pid_t pid = start_server(0, &port);
  expect(pid > 0, "server");
  char json[256];
  snprintf(json, sizeof json,
           "{\"url\":\"http://127.0.0.1:%d\",\"token\":\"fixture-access\",\"form\":\"grant_type=refresh_token\"}\n",
           port);
  yyjson_doc *doc = probe(json, NULL);
  yyjson_val *root_ = yyjson_doc_get_root(doc);
  expect(yyjson_is_false(jget(root_, "failed")), "post failed");
  expect(is_str(jget(jget(root_, "body"), "received"), "grant_type=refresh_token"), "form");
  yyjson_doc_free(doc);
  size_t n = 0;
  unsigned char *seen = check_read(http_log, &n);
  yyjson_doc *first = NULL;
  if (seen) {
    char *nl = memchr(seen, '\n', n);
    if (nl)
      first = parse_json(seen, (size_t)(nl - (char *)seen));
  }
  expect(first && is_str(jget(yyjson_doc_get_root(first), "auth"), "Bearer fixture-access"), "auth");
  expect(first && is_str(jget(yyjson_doc_get_root(first), "type"), "application/x-www-form-urlencoded"),
         "type");
  yyjson_doc_free(first);
  free(seen);
  snprintf(json, sizeof json, "{\"url\":\"http://127.0.0.1:%d/redirect\",\"token\":\"fixture\"}\n", port);
  doc = probe(json, NULL);
  expect(is_num(jget(yyjson_doc_get_root(doc), "status"), 302), "redirect");
  yyjson_doc_free(doc);
  seen = check_read(http_log, &n);
  expect(seen && !contains_text(seen, n, "\"/capture\""), "not followed");
  free(seen);
  struct timespec start, stop;
  clock_gettime(CLOCK_MONOTONIC, &start);
  snprintf(json, sizeof json, "{\"url\":\"http://127.0.0.1:%d/error\"}\n", port);
  doc = probe(json, NULL);
  clock_gettime(CLOCK_MONOTONIC, &stop);
  double elapsed = (double)(stop.tv_sec - start.tv_sec) + (double)(stop.tv_nsec - start.tv_nsec) / 1e9;
  expect(is_num(jget(yyjson_doc_get_root(doc), "status"), 503), "503");
  expect(yyjson_is_false(jget(yyjson_doc_get_root(doc), "failed")), "503 failed");
  expect(elapsed < 2, "503 time");
  yyjson_doc_free(doc);
  snprintf(json, sizeof json, "{\"url\":\"http://127.0.0.1:%d/invalid\"}\n", port);
  doc = probe(json, NULL);
  expect(yyjson_is_true(jget(yyjson_doc_get_root(doc), "failed")), "invalid");
  yyjson_doc_free(doc);
  snprintf(json, sizeof json, "{\"url\":\"http://127.0.0.1:%d/nul\"}\n", port);
  doc = probe(json, NULL);
  expect(yyjson_is_true(jget(yyjson_doc_get_root(doc), "failed")), "nul");
  yyjson_doc_free(doc);
  snprintf(json, sizeof json, "{\"url\":\"http://127.0.0.1:%d/large\"}\n", port);
  doc = probe(json, NULL);
  size_t pad = 0;
  const char *padding = jstr(jget(jget(yyjson_doc_get_root(doc), "body"), "padding"), &pad);
  expect(padding && pad == 9 * 1024 * 1024, "large");
  yyjson_doc_free(doc);
  int count = log_lines();
  snprintf(json, sizeof json, "{\"url\":\"http://127.0.0.1:%d\",\"token\":\"bad\\r\\nInjected: value\"}\n",
           port);
  doc = probe(json, NULL);
  expect(yyjson_is_true(jget(yyjson_doc_get_root(doc), "failed")), "injected");
  expect(log_lines() == count, "no injected request");
  yyjson_doc_free(doc);
  kill(-pid, SIGKILL);
  waitpid(pid, NULL, 0);
  free(http_log);
  check_rm_rf(root);
  free(root);
}
static void slow_http(void) {
  check_begin("HTTP responses may exceed fifteen seconds while still making progress");
  char *root = check_temp("usage-slow-");
  http_log = check_join(root, "seen.log");
  check_write(http_log, "", 0);
  int port = 0;
  pid_t pid = start_server(1, &port);
  char json[128];
  snprintf(json, sizeof json, "{\"url\":\"http://127.0.0.1:%d\"}\n", port);
  yyjson_doc *doc = probe(json, NULL);
  expect(yyjson_is_true(jget(jget(yyjson_doc_get_root(doc), "body"), "ok")), "slow ok");
  yyjson_doc_free(doc);
  kill(-pid, SIGKILL);
  waitpid(pid, NULL, 0);
  free(http_log);
  check_rm_rf(root);
  free(root);
}
static int start_tls(const char *cert, const char *key, int *port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 8))
    return -1;
  socklen_t len = sizeof addr;
  getsockname(fd, (struct sockaddr *)&addr, &len);
  *port = ntohs(addr.sin_port);
  pid_t pid = fork();
  if (!pid) {
    setpgid(0, 0);
    signal(SIGPIPE, SIG_IGN);
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx || SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1)
      _exit(1);
    const char *body = "{\"ok\":true}";
    char response[160];
    int wrote = snprintf(response, sizeof response,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n%s",
                         strlen(body), body);
    for (;;) {
      int client = accept(fd, NULL, NULL);
      if (client < 0)
        continue;
      SSL *ssl = SSL_new(ctx);
      SSL_set_fd(ssl, client);
      if (SSL_accept(ssl) == 1) {
        char buf[2048];
        SSL_read(ssl, buf, sizeof buf);
        SSL_write(ssl, response, wrote);
      }
      SSL_shutdown(ssl);
      SSL_free(ssl);
      close(client);
    }
  }
  setpgid(pid, pid);
  close(fd);
  return (int)pid;
}
static void https_transport(void) {
  check_begin("real HTTPS transport validates both the CA bundle and hostname");
  expect(openssl_bin != NULL, "openssl");
  char *root = check_temp("usage-tls-");
  char *key = check_join(root, "key.pem");
  char *cert = check_join(root, "cert.pem");
  const char *args[] = {openssl_bin, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
                        key, "-out", cert, "-days", "1", "-subj", "/CN=fixture.invalid", "-addext",
                        "subjectAltName=IP:127.0.0.1", NULL};
  Proc made = check_run(openssl_bin, args, NULL, 0, NULL, 20);
  expect(made.status == 0, "cert");
  proc_free(&made);
  int port = 0;
  pid_t pid = start_tls(cert, key, &port);
  char json[160];
  snprintf(json, sizeof json, "{\"url\":\"https://127.0.0.1:%d\"}\n", port);
  yyjson_doc *doc = probe(json, NULL);
  expect(yyjson_is_true(jget(yyjson_doc_get_root(doc), "failed")), "untrusted");
  yyjson_doc_free(doc);
  char *ca = fmt("SSL_CERT_FILE=%s", cert);
  const char *env[] = {ca, NULL};
  doc = probe(json, env);
  expect(yyjson_is_true(jget(jget(yyjson_doc_get_root(doc), "body"), "ok")), "trusted ip");
  yyjson_doc_free(doc);
  snprintf(json, sizeof json, "{\"url\":\"https://localhost:%d\"}\n", port);
  doc = probe(json, env);
  expect(yyjson_is_true(jget(yyjson_doc_get_root(doc), "failed")), "hostname");
  yyjson_doc_free(doc);
  kill(-pid, SIGKILL);
  waitpid(pid, NULL, 0);
  free(ca);
  free(cert);
  free(key);
  check_rm_rf(root);
  free(root);
}
int main(void) {
  char *exe = check_exe_dir();
  char *fixture = check_join(exe, "usage-fixture");
  fixture_bin = fixture;
  bin_dir = exe;
  openssl_bin = which("openssl");
  scales();
  whitespace();
  timestamps();
  pi_scan();
  grok_scan();
  opencode_scan();
  billing();
  go_windows();
  codex_windows();
  waybar();
  legacy_credentials();
  oauth_merge();
  jwt();
  commands();
  installed();
  status_exe();
  fresh_oauth();
  persist_refresh();
  failed_save();
  overflow();
  refresh_fallback();
  go_keys();
  http_errors();
  http_transport();
  slow_http();
  https_transport();
  check_begin("usage-checks");
  return check_finish();
}
