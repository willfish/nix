#include "run.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef BASELINE
#define BASELINE "home/config/llm/skills/pi-token-mitm/scripts/baseline-2026-09-12.json"
#endif
static char *root, *binary, *stub;
static int serial;
static void expect(int cond, const char *what) {
  if (!cond)
    check_fail("%s", what);
}
typedef struct {
  char *home, *state, *env_home, *env_state;
} Fixture;
static Fixture fixture(void) {
  char leaf[32];
  snprintf(leaf, sizeof leaf, "%d", serial++);
  Fixture f = {0};
  f.home = check_join(root, leaf);
  f.state = check_join(f.home, "state");
  char *dir = check_join(f.state, "prompt-capture");
  check_mkdir_p(dir);
  free(dir);
  f.env_home = malloc(strlen(f.home) + 6);
  f.env_state = malloc(strlen(f.state) + 16);
  snprintf(f.env_home, strlen(f.home) + 6, "HOME=%s", f.home);
  snprintf(f.env_state, strlen(f.state) + 16, "XDG_STATE_HOME=%s", f.state);
  return f;
}
static void fixture_free(Fixture *f) {
  free(f->home);
  free(f->state);
  free(f->env_home);
  free(f->env_state);
}
static const char *first_body =
    "{\"model\":\"grok-4.6\",\"tools\":[{\"type\":\"function\",\"name\":\"skill_catalog\","
    "\"description\":\"find skills\",\"parameters\":{\"type\":\"object\"}},{\"type\":\"function\","
    "\"name\":\"mcp\",\"description\":\"mcp status\",\"parameters\":{\"type\":\"object\"}}],"
    "\"input\":[{\"role\":\"developer\",\"content\":\"PRIVATE_INSTRUCTIONS\"},{\"role\":\"user\","
    "\"content\":\"turn 1\"}]}";
static const char *second_body =
    "{\"model\":\"grok-4.6\",\"tools\":[{\"type\":\"function\",\"name\":\"skill_catalog\","
    "\"description\":\"find skills\",\"parameters\":{\"type\":\"object\"}},{\"type\":\"function\","
    "\"name\":\"mcp\",\"description\":\"mcp status\",\"parameters\":{\"type\":\"object\"}}],"
    "\"input\":[{\"role\":\"developer\",\"content\":\"PRIVATE_INSTRUCTIONS\"},{\"role\":\"user\","
    "\"content\":\"turn 1\"},{\"type\":\"function_call\",\"name\":\"skill_catalog\",\"call_id\":"
    "\"call-1\",\"arguments\":\"{\\\"query\\\":\\\"local development nix\\\"}\"},{\"type\":"
    "\"function_call_output\",\"call_id\":\"call-1\",\"output\":\"PRIVATE_TOOL_RESULT\"}]}";
static void capture(Fixture *f, const char *jsonl) {
  char *path = check_join(f->state, "prompt-capture/pi.jsonl");
  size_t n = strlen(jsonl);
  char *all = malloc(n + 32);
  memcpy(all, jsonl, n);
  memcpy(all + n, "malformed ignored line\n", 23);
  check_write(path, all, n + 23);
  free(all);
  free(path);
}
static char *rows(const char *run) {
  char *out = NULL;
  size_t len = 0;
  FILE *mem = open_memstream(&out, &len);
  if (!mem)
    return NULL;
  fprintf(mem,
          "{\"run\":\"%s\",\"kind\":\"request\",\"flow_id\":\"f1\",\"ts\":\"t1\","
          "\"url\":\"api.x.ai/v1/responses\",\"request_body\":",
          run);
  fputc('"', mem);
  for (const char *p = first_body; *p; p++) {
    if (*p == '"' || *p == '\\')
      fputc('\\', mem);
    fputc(*p, mem);
  }
  fprintf(mem, "\",\"request_chars\":%zu}\n", strlen(first_body));
  fprintf(mem,
          "{\"run\":\"%s\",\"kind\":\"usage\",\"flow_id\":\"f1\",\"usage\":{\"input_tokens\":1000,"
          "\"output_tokens\":20,\"total_tokens\":1020}}\n",
          run);
  fprintf(mem, "{\"run\":\"%s\",\"kind\":\"response\",\"flow_id\":\"f1\",\"status\":200}\n", run);
  fprintf(mem,
          "{\"run\":\"%s\",\"kind\":\"request\",\"flow_id\":\"f2\",\"ts\":\"t2\","
          "\"url\":\"api.x.ai/v1/responses\",\"request_body\":",
          run);
  fputc('"', mem);
  for (const char *p = second_body; *p; p++) {
    if (*p == '"' || *p == '\\')
      fputc('\\', mem);
    fputc(*p, mem);
  }
  fprintf(mem, "\",\"request_chars\":%zu}\n", strlen(second_body));
  fprintf(mem,
          "{\"run\":\"%s\",\"kind\":\"usage\",\"flow_id\":\"f2\",\"usage\":{\"input_tokens\":1100,"
          "\"output_tokens\":10,\"cached_tokens\":800,\"total_tokens\":1110,"
          "\"output_tokens_details\":{\"reasoning_tokens\":4}}}\n",
          run);
  fprintf(mem, "{\"run\":\"%s\",\"kind\":\"response\",\"flow_id\":\"f2\",\"status\":200}\n", run);
  fclose(mem);
  return out;
}
static yyjson_doc *report(Fixture *f, const char *run, char **html, char **html_path) {
  const char *args[] = {binary, "--from-run", run, NULL};
  const char *env[] = {f->env_home, f->env_state, NULL};
  Proc result = check_run(binary, args, NULL, 0, env, 10);
  expect(result.status == 0, (const char *)result.err);
  proc_free(&result);
  char *dir = check_join(f->state, "prompt-capture/reports");
  char leaf[128];
  snprintf(leaf, sizeof leaf, "%s.json", run);
  char *json_path = check_join(dir, leaf);
  snprintf(leaf, sizeof leaf, "%s.html", run);
  *html_path = check_join(dir, leaf);
  size_t json_len = 0, html_len = 0;
  unsigned char *json = check_read(json_path, &json_len);
  unsigned char *page = check_read(*html_path, &html_len);
  *html = (char *)page;
  yyjson_doc *doc = parse_json(json, json_len);
  if (!doc)
    check_fail("report json");
  free(json);
  free(json_path);
  free(dir);
  return doc;
}
static void responses_report(void) {
  check_begin("Responses requests, cache-aware costs, distinct calls and private report publication");
  Fixture f = fixture();
  char *jsonl = rows("fixture");
  capture(&f, jsonl);
  free(jsonl);
  char *html = NULL, *html_path = NULL;
  yyjson_doc *doc = report(&f, "fixture", &html, &html_path);
  yyjson_val *data = yyjson_doc_get_root(doc);
  yyjson_val *requests = jget(data, "requests");
  expect(yyjson_get_num(jget(jget(data, "meta"), "request_count")) == 2, "requests");
  expect(yyjson_get_num(jget(jget(data, "meta"), "record_count")) == 6, "records");
  expect(yyjson_arr_size(jget(yyjson_arr_get(requests, 0), "tool_calls")) == 0, "no calls");
  yyjson_val *queries = jget(yyjson_arr_get(requests, 1), "skill_queries");
  size_t len = 0;
  const char *query = jstr(yyjson_arr_get(queries, 0), &len);
  expect(query && !strcmp(query, "local development nix"), "query");
  expect(yyjson_get_num(jget(jget(yyjson_arr_get(requests, 1), "result_chars"),
                            "skill_catalog")) == strlen("PRIVATE_TOOL_RESULT"),
         "result chars");
  expect(yyjson_get_num(jget(jget(yyjson_arr_get(requests, 1), "usage"), "reasoning")) == 4,
         "reasoning");
  yyjson_val *skill = jget(jget(jget(data, "attribution"), "tools"), "skill_catalog");
  expect(yyjson_get_num(jget(skill, "unique_calls")) == 1, "unique");
  expect(yyjson_get_num(jget(skill, "wire_appearances")) == 1, "wire");
  expect(fabs(yyjson_get_num(jget(yyjson_arr_get(requests, 1), "cost_usd")) - 0.00106) < 1e-12,
         "cost");
  expect(html && strstr(html, "aria-label=\"Tokens per LLM request\""), "aria");
  const char *secrets[] = {"PRIVATE_INSTRUCTIONS", "PRIVATE_TOOL_RESULT", "request_body"};
  char *encoded = yyjson_val_write(data, 0, NULL);
  for (size_t i = 0; i < 3; i++) {
    expect(!strstr(html, secrets[i]) && encoded && !strstr(encoded, secrets[i]), secrets[i]);
  }
  free(encoded);
  expect(check_mode(html_path) == 0600, "html mode");
  char *json_path = strdup(html_path);
  char *dot = strrchr(json_path, '.');
  memcpy(dot, ".json", 6);
  expect(check_mode(json_path) == 0600, "json mode");
  char *dir = strdup(html_path);
  *strrchr(dir, '/') = 0;
  expect(check_mode(dir) == 0700, "dir mode");
  char *latest = check_join(dir, "latest.html");
  char link[64];
  ssize_t n = readlink(latest, link, sizeof link - 1);
  if (n < 0)
    check_fail("latest link");
  else {
    link[n] = 0;
    expect(!strcmp(link, "fixture.html"), "latest target");
  }
  if (getenv("KEEP_TOKEN_FIXTURE"))
    printf("TOKEN_HTML=%s\n", html_path);
  free(latest);
  free(dir);
  free(json_path);
  free(html);
  free(html_path);
  yyjson_doc_free(doc);
  fixture_free(&f);
}
static void streaming_usage(void) {
  check_begin("streaming usage takes maxima, aliases and Chat calls retain their counting rules");
  Fixture f = fixture();
  const char *input =
      "{\"model\":\"grok-4.6\",\"functions\":[{\"function\":{\"name\":\"bash\"}}],\"messages\":"
      "[{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"chat-1\",\"function\":{\"name\":\"bash\","
      "\"arguments\":\"{\\\"command\\\":\\\"private\\\"}\"}}]}]}";
  char *jsonl = NULL;
  size_t len = 0;
  FILE *mem = open_memstream(&jsonl, &len);
  fputs("{\"run\":\"fixture\",\"kind\":\"request\",\"flow_id\":\"f\",\"url\":\"https://service/chat/completions\",\"request_body\":",
        mem);
  fputc('"', mem);
  for (const char *p = input; *p; p++) {
    if (*p == '"' || *p == '\\')
      fputc('\\', mem);
    fputc(*p, mem);
  }
  fputs("\"}\n", mem);
  fputs("{\"run\":\"fixture\",\"kind\":\"usage\",\"flow_id\":\"f\",\"usage\":{\"input_tokens\":100,\"output_tokens\":1}}\n",
        mem);
  fputs("{\"run\":\"fixture\",\"kind\":\"usage\",\"flow_id\":\"f\",\"usage\":{\"promptTokens\":100,\"completionTokens\":8,\"prompt_tokens_details\":{\"cachedTokens\":40}}}\n",
        mem);
  fputs("{\"run\":\"fixture\",\"kind\":\"request\",\"flow_id\":\"asset\",\"url\":\"https://example/asset\",\"request_body\":\"{}\"}\n",
        mem);
  fputs("{\"run\":\"other\",\"kind\":\"usage\",\"flow_id\":\"other\",\"usage\":{\"input_tokens\":999}}\n",
        mem);
  fclose(mem);
  capture(&f, jsonl);
  free(jsonl);
  char *html = NULL, *html_path = NULL;
  yyjson_doc *doc = report(&f, "fixture", &html, &html_path);
  yyjson_val *requests = jget(yyjson_doc_get_root(doc), "requests");
  yyjson_val *usage = jget(yyjson_arr_get(requests, 0), "usage");
  expect(yyjson_arr_size(requests) == 1, "one request");
  expect(yyjson_get_num(jget(usage, "input")) == 100, "input");
  expect(yyjson_get_num(jget(usage, "output")) == 8, "output");
  expect(yyjson_get_num(jget(usage, "total")) == 108, "total");
  expect(yyjson_get_num(jget(usage, "cached")) == 40, "cached");
  expect(fabs(yyjson_get_num(jget(yyjson_arr_get(requests, 0), "cost_usd")) - 0.000188) < 1e-12,
         "chat cost");
  yyjson_val *bash = jget(jget(jget(yyjson_doc_get_root(doc), "attribution"), "tools"), "bash");
  expect(yyjson_get_num(jget(bash, "unique_calls")) == 0, "chat unique");
  expect(yyjson_get_num(jget(bash, "wire_appearances")) == 1, "chat wire");
  free(html);
  free(html_path);
  yyjson_doc_free(doc);
  fixture_free(&f);
}
static void privacy_guard(void) {
  check_begin("custom prices and privacy guard keep raw arguments, URL credentials and bodies out of both artifacts");
  Fixture f = fixture();
  char *agent = check_join(f.home, ".pi/agent");
  check_mkdir_p(agent);
  char *models = check_join(agent, "models-store.json");
  const char *store =
      "{\"xai\":{\"models\":[{\"id\":\"grok-4.6\",\"cost\":{\"input\":3,\"output\":9,\"cacheRead\":1}}]}}";
  check_write(models, store, strlen(store));
  const char *payload =
      "{\"tools\":[{\"name\":\"mcp\"}],\"input\":[{\"type\":\"function_call\",\"name\":\"mcp\","
      "\"call_id\":\"mcp-1\",\"arguments\":\"{\\\"tool\\\":\\\"send\\\",\\\"args\\\":{\\\"password\\\":"
      "\\\"SECRET_ARGUMENT\\\"}}\"},{\"type\":\"function_call\",\"name\":\"skill_catalog\",\"call_id\":"
      "\"skill-1\",\"arguments\":\"{\\\"query\\\":\\\"SECRET_QUERY\\\"}\"},{\"type\":"
      "\"function_call_output\",\"call_id\":\"mcp-1\",\"output\":\"SECRET_RESULT\"}]}";
  char *jsonl = NULL;
  size_t len = 0;
  FILE *mem = open_memstream(&jsonl, &len);
  fputs("{\"run\":\"fixture\",\"kind\":\"request\",\"flow_id\":\"f\",\"url\":\"https://user:SECRET_PASSWORD@api.x.ai/v1/responses?key=SECRET_URL\",\"request_body\":",
        mem);
  fputc('"', mem);
  for (const char *p = payload; *p; p++) {
    if (*p == '"' || *p == '\\')
      fputc('\\', mem);
    fputc(*p, mem);
  }
  fputs("\"}\n{\"run\":\"fixture\",\"kind\":\"usage\",\"flow_id\":\"f\",\"usage\":{\"input_tokens\":100,\"output_tokens\":2}}\n",
        mem);
  fclose(mem);
  capture(&f, jsonl);
  free(jsonl);
  char *html = NULL, *html_path = NULL;
  yyjson_doc *doc = report(&f, "fixture", &html, &html_path);
  yyjson_val *data = yyjson_doc_get_root(doc);
  expect(yyjson_get_num(jget(jget(data, "rates"), "input")) == 3, "custom rate");
  size_t url_len = 0;
  const char *url = jstr(jget(yyjson_arr_get(jget(data, "requests"), 0), "url"), &url_len);
  expect(url && !strcmp(url, "https://api.x.ai/v1/responses"), "redacted url");
  char *encoded = yyjson_val_write(data, 0, NULL);
  expect(html && !strstr(html, "SECRET_") && encoded && !strstr(encoded, "SECRET_"), "secrets");
  yyjson_val *request = yyjson_arr_get(jget(data, "requests"), 0);
  const char *mcp = jstr(yyjson_arr_get(jget(request, "mcp_calls"), 0), &url_len);
  const char *skill = jstr(yyjson_arr_get(jget(request, "skill_queries"), 0), &url_len);
  expect(mcp && !strcmp(mcp, "[arguments omitted]"), "mcp omitted");
  expect(skill && !strcmp(skill, "[query omitted]"), "query omitted");
  free(encoded);
  free(html);
  free(html_path);
  free(models);
  free(agent);
  yyjson_doc_free(doc);
  fixture_free(&f);
}
static void stub_capture(void) {
  check_begin("stub capture isolates the parent, keeps the fixed workload and redacts failed probe output");
  Fixture f = fixture();
  char *local = check_join(f.home, ".local/bin");
  check_mkdir_p(local);
  char *pi = check_join(local, "pi");
  const char *wrapper = "exec /nix/store/first/bin/pi\nexec /nix/store/last/bin/pi\n";
  check_write(pi, wrapper, strlen(wrapper));
  char *capture_bin = check_join(local, "prompt-capture");
  if (symlink(stub, capture_bin))
    check_fail("capture stub");
  char *receipt = check_join(f.home, "receipt.json");
  char *jsonl = rows("20260912T104853-77612");
  capture(&f, jsonl);
  free(jsonl);
  char *receipt_env = malloc(strlen(receipt) + 9);
  snprintf(receipt_env, strlen(receipt) + 9, "RECEIPT=%s", receipt);
  const char *env[] = {f.env_home,
                       f.env_state,
                       receipt_env,
                       "PI_SESSION_FILE=parent",
                       "PI_SESSION_ID=parent",
                       "CAPTURE_PROMPTS=1",
                       "HTTP_PROXY=parent",
                       "HTTPS_PROXY=parent",
                       "http_proxy=parent",
                       "https_proxy=parent",
                       "SSL_CERT_FILE=parent",
                       "REQUESTS_CA_BUNDLE=parent",
                       "NODE_EXTRA_CA_CERTS=parent",
                       NULL};
  const char *args[] = {binary, "--capture", NULL};
  Proc result = check_run(binary, args, NULL, 0, env, 20);
  expect(result.status == 0, (const char *)result.err);
  expect(!contains_text(result.out, result.out_len, "SENSITIVE_PROBE_VALUE") &&
             !contains_text(result.err, result.err_len, "SENSITIVE_PROBE_VALUE"),
         "redacted probe");
  proc_free(&result);
  size_t got_len = 0;
  unsigned char *got_raw = check_read(receipt, &got_len);
  yyjson_doc *got = parse_json(got_raw, got_len);
  yyjson_val *root_ = yyjson_doc_get_root(got);
  expect(yyjson_is_true(jget(root_, "unset")), "unset");
  size_t len = 0;
  const char *telemetry = jstr(jget(root_, "telemetry"), &len);
  expect(telemetry && !strcmp(telemetry, "0"), "telemetry");
  yyjson_val *got_args = jget(root_, "args");
  const char *a0 = jstr(yyjson_arr_get(got_args, 0), &len);
  const char *a1 = jstr(yyjson_arr_get(got_args, 1), &len);
  expect(a0 && a1 && !strcmp(a0, "pi") && !strcmp(a1, "--"), "args");
  const char *source = jstr(jget(root_, "source"), &len);
  expect(source && strstr(source, "/nix/store/last/bin/pi"), "last pi");
  int continues = 0;
  if (source)
    for (const char *p = source; (p = strstr(p, "--continue")); p += 10)
      continues++;
  expect(continues == 2, "continue");
  expect(source && strstr(source, "python3 add.py"), "python workload");
  expect(source && strstr(source, "unset PI_SESSION_FILE PI_SESSION_ID CAPTURE_PROMPTS"),
         "unset line");
  char *archive = check_join(f.state, "prompt-capture/reports/20260912T104853-77612.json");
  size_t archive_len = 0;
  unsigned char *archive_raw = check_read(archive, &archive_len);
  yyjson_doc *archived = parse_json(archive_raw, archive_len);
  yyjson_val *meta = jget(yyjson_doc_get_root(archived), "meta");
  expect(yyjson_get_num(jget(meta, "exit_code")) == 17, "exit");
  const char *task = jstr(jget(meta, "task_result"), &len);
  expect(task && !strcmp(task, "add.py=yes output=error"), "task");
  char *log = check_join(f.state, "prompt-capture/reports/nested-pi.log");
  expect(check_mode(log) == 0600, "log mode");
  free(log);
  free(archive_raw);
  yyjson_doc_free(archived);
  free(archive);
  free(got_raw);
  yyjson_doc_free(got);
  free(receipt_env);
  free(receipt);
  free(capture_bin);
  free(pi);
  free(local);
  fixture_free(&f);
}
static void baseline_and_escape(void) {
  check_begin("baseline stays counts-only, empty captures return two and run identifiers cannot escape reports");
  size_t len = 0;
  unsigned char *raw = check_read(BASELINE, &len);
  if (!raw)
    check_fail("baseline missing");
  yyjson_doc *doc = parse_json(raw, len);
  yyjson_val *root_ = yyjson_doc_get_root(doc);
  expect(yyjson_get_num(jget(root_, "request_count")) == 7, "baseline count");
  size_t id_len = 0;
  const char *run = jstr(jget(root_, "run_id"), &id_len);
  expect(run && !strcmp(run, "20260912T104853-77612"), "baseline run");
  expect(!contains_text(raw, len, "request_body") && !contains_text(raw, len, "Authorization"),
         "baseline privacy");
  yyjson_doc_free(doc);
  free(raw);
  Fixture f = fixture();
  const char *env[] = {f.env_home, f.env_state, NULL};
  const char *empty[] = {binary, "--from-run", "empty", NULL};
  Proc result = check_run(binary, empty, NULL, 0, env, 10);
  expect(result.status == 2, "empty status");
  proc_free(&result);
  char *page = check_join(f.state, "prompt-capture/reports/empty.html");
  size_t page_len = 0;
  unsigned char *html = check_read(page, &page_len);
  expect(html && contains_text(html, page_len, "No LLM requests captured."), "empty page");
  free(html);
  free(page);
  const char *escape[] = {binary, "--from-run", "../escape", NULL};
  result = check_run(binary, escape, NULL, 0, env, 10);
  expect(result.status == 1, "escape status");
  proc_free(&result);
  char *escaped = check_join(f.state, "prompt-capture/escape.html");
  expect(access(escaped, F_OK) != 0, "no escape file");
  free(escaped);
  const char *unknown[] = {binary, "--unknown", NULL};
  result = check_run(binary, unknown, NULL, 0, env, 10);
  expect(result.status == 2, "unknown");
  proc_free(&result);
  fixture_free(&f);
}
void run_token_tests(void) {
  root = check_temp("token-report-");
  char *bin_dir = getenv("SKILL_TOOLS_BIN") ? strdup(getenv("SKILL_TOOLS_BIN"))
                                            : check_exe_dir();
  binary = getenv("TOKEN_REPORT_BIN") ? strdup(getenv("TOKEN_REPORT_BIN"))
                                      : check_join(bin_dir, "pi-token-report");
  char *exe = check_exe_dir();
  stub = check_join(exe, "capture-stub");
  free(exe);
  free(bin_dir);
  responses_report();
  streaming_usage();
  privacy_guard();
  stub_capture();
  baseline_and_escape();
  if (!getenv("KEEP_TOKEN_FIXTURE"))
    check_rm_rf(root);
  free(root);
  free(binary);
  free(stub);
}
