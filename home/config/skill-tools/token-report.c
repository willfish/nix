#define _DEFAULT_SOURCE
#include "token-report.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *usage_keys[] = {"input", "output", "total", "cached",
                                   "reasoning"};
const char *token_turn_names[] = {"turn-1-skill-catalog", "turn-2-mcp-status",
                                  "turn-3-write-and-run"};
const char *token_turns[] = {
    "Work only in the current directory. Do not use team, subagent, or edit "
    "any files. Call skill_catalog once with query 'local development nix'. "
    "After you have the catalog result, reply with the matching skill name "
    "only and stop.",
    "Work only in the current directory. Do not use team or subagent. Call the "
    "mcp tool with no arguments so it lists MCP server status. Reply with "
    "connected server names only, then stop. Do not start other work.",
    "Work only in the current directory. Do not use team or subagent. Create "
    "add.py that prints the integer result of 2+3. Run it with python3 add.py. "
    "Stop when stdout is 5. Do not do anything else."};
const char *token_text(Val *v) {
  return yyjson_is_str(v) && strlen(yyjson_get_str(v)) == yyjson_get_len(v)
             ? yyjson_get_str(v)
             : NULL;
}
bool token_truth(Val *v) {
  if (!v || yyjson_is_null(v))
    return false;
  if (yyjson_is_bool(v))
    return yyjson_get_bool(v);
  if (yyjson_is_str(v))
    return yyjson_get_len(v) > 0;
  if (yyjson_is_arr(v))
    return yyjson_arr_size(v) > 0;
  if (yyjson_is_obj(v))
    return yyjson_obj_size(v) > 0;
  return yyjson_get_num(v) != 0;
}
static const char *text_or(Val *v, const char *fallback) {
  const char *s = token_text(v);
  return s && *s ? s : fallback;
}
char *token_home(const char *suffix) {
  const char *home = g_getenv("HOME");
  return g_build_filename(home ? home : g_get_home_dir(), suffix, NULL);
}
char *token_state(const char *suffix) {
  const char *state = g_getenv("XDG_STATE_HOME");
  char *fallback = state ? NULL : token_home(".local/state");
  char *out = g_build_filename(state ? state : fallback, "prompt-capture",
                               suffix, NULL);
  g_free(fallback);
  return out;
}
bool token_private_file(const char *path, const char *data, size_t length,
                        mode_t mode) {
  int fd =
      open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
  if (fd < 0)
    return false;
  bool ok = fchmod(fd, mode) == 0;
  for (size_t at = 0; ok && at < length;) {
    ssize_t n = write(fd, data + at, length - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      ok = false;
    else
      at += (size_t)n;
  }
  if (close(fd) != 0)
    ok = false;
  return ok;
}
static GPtrArray *strings(void) {
  return g_ptr_array_new_with_free_func(g_free);
}
static void append(GPtrArray *out, const char *s) {
  g_ptr_array_add(out, g_strdup(s));
}
static void append_unique(GPtrArray *out, const char *s) {
  for (size_t i = 0; i < out->len; i++)
    if (!strcmp(out->pdata[i], s))
      return;
  append(out, s);
}
static void sum(GHashTable *map, const char *key, int64_t n) {
  int64_t *count = g_hash_table_lookup(map, key);
  if (!count) {
    count = g_new0(int64_t, 1);
    g_hash_table_insert(map, g_strdup(key), count);
  }
  *count += n;
}
static int64_t chars(const char *s) { return (int64_t)g_utf8_strlen(s, -1); }
static int64_t json_chars(Val *v, bool compact) {
  char *s =
      v ? yyjson_val_write(v, YYJSON_WRITE_ALLOW_INF_AND_NAN, NULL) : NULL;
  if (!s)
    return 0;
  int64_t n = chars(s);
  if (!compact) {
    bool quoted = false;
    for (const char *p = s; *p; p++) {
      if (quoted && *p == '\\' && p[1])
        p++;
      else if (*p == '"')
        quoted = !quoted;
      else if (!quoted && (*p == ',' || *p == ':'))
        n++;
    }
  }
  free(s);
  return n;
}
static bool numeric_int(Val *v, int64_t *out) {
  if (!yyjson_is_num(v))
    return false;
  double value = yyjson_get_num(v);
  if (!isfinite(value) || value < (-9223372036854775807.0 - 1) ||
      value >= 9223372036854775808.0)
    return false;
  *out = (int64_t)value;
  return true;
}
static int64_t first(Val *v, const char *const *keys) {
  int64_t result;
  for (size_t i = 0; keys[i]; i++)
    if (numeric_int(field(v, keys[i]), &result))
      return result;
  return 0;
}
static void usage_merge(Usage *out, Val *usage) {
  const char *in[] = {"input_tokens", "prompt_tokens", "inputTokens",
                      "promptTokens", NULL},
             *output[] = {"output_tokens", "completion_tokens", "outputTokens",
                          "completionTokens", NULL},
             *total[] = {"total_tokens", "totalTokens", NULL},
             *cache[] = {"cached_tokens", "cachedTokens", NULL},
             *reason[] = {"reasoning_tokens", "reasoningTokens", NULL},
             *direct[] = {"cached_tokens", "cache_read_input_tokens", NULL};
  int64_t values[5] = {first(usage, in), first(usage, output),
                       first(usage, total), 0, 0};
  Val *details = field(usage, "input_tokens_details");
  if (!token_truth(details))
    details = field(usage, "prompt_tokens_details");
  values[3] = first(details, cache);
  int64_t top = first(usage, direct);
  if (top)
    values[3] = top;
  details = field(usage, "output_tokens_details");
  if (!token_truth(details))
    details = field(usage, "completion_tokens_details");
  values[4] = first(details, reason);
  if (!values[2] && (values[0] || values[1]))
    values[2] = values[0] + values[1];
  for (size_t i = 0; i < 5; i++)
    if (values[i]) {
      out->present |= 1u << i;
      out->value[i] = MAX(out->value[i], values[i]);
    }
}
static double input_cost(Usage usage, Rates rates) {
  double input = (double)usage.value[0],
         cached = fmin((double)usage.value[3], input);
  return (fmax(input - cached, 0) * rates.input + cached * rates.cached) /
         1000000;
}
static const char *tool_name(Val *tool) {
  const char *name = token_text(field(tool, "name"));
  if (name && *name)
    return name;
  name = token_text(field(tool, "tool"));
  if (name && *name)
    return name;
  Val *fn = field(tool, "function");
  if (!token_truth(fn))
    fn = field(tool, "definition");
  return token_text(field(fn, "name"));
}
static char *call(Request *r, Val *value) {
  if (!yyjson_is_obj(value))
    return NULL;
  Val *fn = field(value, "function");
  if (!yyjson_is_obj(fn))
    fn = value;
  Val *name = field(fn, "name");
  if (!token_truth(name))
    name = field(value, "name");
  if (!token_truth(name))
    name = field(value, "tool");
  const char *s = token_text(name);
  if (!s)
    return NULL;
  append(r->calls, s);
  Val *args = field(fn, "arguments");
  if (!token_truth(args))
    args = field(value, "arguments");
  if (!token_truth(args))
    args = field(value, "input");
  yyjson_doc *parsed = NULL;
  const char *raw = token_text(args);
  if (raw)
    parsed = yyjson_read(raw, strlen(raw), 0);
  Val *object = parsed ? yyjson_doc_get_root(parsed) : args;
  // Preserve the fixed probe label, never arbitrary query or MCP argument
  // values.
  if (!strcmp(s, "skill_catalog")) {
    Val *query = field(object, "query");
    if (token_truth(query) || (!parsed && raw && *raw))
      append(r->queries, text_is(query, "local development nix")
                             ? "local development nix"
                             : "[query omitted]");
  }
  if (!strcmp(s, "mcp"))
    append(r->mcp, !token_truth(args) ||
                           (yyjson_is_obj(object) && !yyjson_obj_size(object))
                       ? "{}"
                       : "[arguments omitted]");
  if (parsed)
    yyjson_doc_free(parsed);
  return g_strdup(s);
}
static void parse_payload(Request *r, const char *body) {
  yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
  if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
    if (doc)
      yyjson_doc_free(doc);
    return;
  }
  Val *payload = yyjson_doc_get_root(doc);
  const char *model = token_text(field(payload, "model"));
  if (model && *model) {
    g_free(r->model);
    r->model = g_strdup(model);
  }
  Val *tools = field(payload, "tools");
  if (!token_truth(tools))
    tools = field(payload, "functions");
  if (yyjson_is_arr(tools)) {
    r->schema_chars = json_chars(tools, true);
    Val *tool;
    size_t i, n;
    yyjson_arr_foreach(tools, i, n, tool) {
      const char *name = tool_name(tool);
      if (name && *name)
        append(r->advertised, name);
    }
  }
  Val *messages = field(payload, "messages");
  if (!token_truth(messages))
    messages = field(payload, "input");
  if (yyjson_is_arr(messages)) {
    r->messages = (int64_t)yyjson_arr_size(messages);
    Val *msg;
    size_t i, n;
    yyjson_arr_foreach(messages, i, n, msg) {
      if (text_is(field(msg, "type"), "function_call")) {
        char *name = call(r, msg);
        Val *id = field(msg, "call_id");
        if (!token_truth(id))
          id = field(msg, "id");
        const char *key = token_text(id);
        if (name && key)
          g_hash_table_replace(r->ids, g_strdup(key), g_strdup(name));
        g_free(name);
      }
      Val *calls = field(msg, "tool_calls");
      if (!token_truth(calls))
        calls = field(msg, "toolCalls");
      Val *c;
      size_t j, m;
      yyjson_arr_foreach(calls, j, m, c) {
        char *name = call(r, c);
        g_free(name);
      }
    }
    yyjson_arr_foreach(messages, i, n, msg) {
      if (!text_is(field(msg, "type"), "function_call_output"))
        continue;
      const char *id = token_text(field(msg, "call_id")),
                 *name = id ? g_hash_table_lookup(r->ids, id) : NULL;
      if (!name)
        continue;
      append(r->results, name);
      Val *output = field(msg, "output");
      int64_t n_chars =
          yyjson_is_str(output)
              ? (int64_t)g_utf8_strlen(yyjson_get_str(output),
                                       (gssize)yyjson_get_len(output))
          : !output || yyjson_is_null(output) ? 0
                                              : json_chars(output, false);
      sum(r->result_chars, name, n_chars);
    }
  }
  yyjson_doc_free(doc);
}
static void request_free(gpointer p) {
  Request *r = p;
  g_free(r->ts);
  g_free(r->url);
  g_free(r->model);
  g_ptr_array_free(r->advertised, TRUE);
  g_ptr_array_free(r->calls, TRUE);
  g_ptr_array_free(r->results, TRUE);
  g_ptr_array_free(r->queries, TRUE);
  g_ptr_array_free(r->mcp, TRUE);
  g_hash_table_destroy(r->ids);
  g_hash_table_destroy(r->result_chars);
  g_free(r);
}
typedef struct {
  Val *request, *response, *first_usage;
  Usage usage;
  bool has_usage;
} Flow;
static bool llm(const char *url, const char *body) {
  char *lower = g_ascii_strdown(url, -1);
  const char *parts[] = {"/chat/completions", "/responses", "/v1/messages",
                         "/v1/chat", "api.x.ai"};
  bool yes = false;
  for (size_t i = 0; i < G_N_ELEMENTS(parts); i++)
    yes = yes || strstr(lower, parts[i]);
  g_free(lower);
  if (yes)
    return true;
  yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
  if (doc) {
    Val *p = yyjson_doc_get_root(doc);
    yes = yyjson_is_obj(p) &&
          (field(p, "messages") || field(p, "input") || field(p, "tools") ||
           token_truth(field(p, "model")));
    yyjson_doc_free(doc);
  }
  return yes;
}
static char *endpoint(const char *url) {
  char *s = g_strdup(url), *end = strpbrk(s, "?#");
  if (end)
    *end = 0;
  char *scheme = strstr(s, "://");
  if (scheme) {
    char *slash = strchr(scheme + 3, '/'), *at = strchr(scheme + 3, '@');
    if (at && (!slash || at < slash))
      memmove(scheme + 3, at + 1, strlen(at + 1) + 1);
  }
  return s;
}
static GPtrArray *load_requests(const char *run, Rates rates,
                                GPtrArray *documents, size_t *record_count) {
  char *path = token_state("pi.jsonl");
  FILE *file = fopen(path, "r");
  g_free(path);
  GPtrArray *out = g_ptr_array_new_with_free_func(request_free);
  if (!file) {
    if (errno == ENOENT)
      return out;
    g_ptr_array_free(out, TRUE);
    return NULL;
  }
  GPtrArray *flows = g_ptr_array_new_with_free_func(g_free);
  GHashTable *index =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  char *line = NULL;
  size_t capacity = 0;
  ssize_t size;
  bool ok = true;
  while ((size = getline(&line, &capacity, file)) >= 0) {
    yyjson_doc *doc = yyjson_read(line, (size_t)size, 0);
    if (!doc)
      continue;
    Val *record = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(record)) {
      yyjson_doc_free(doc);
      ok = false;
      break;
    }
    if (!text_is(field(record, "run"), run)) {
      yyjson_doc_free(doc);
      continue;
    }
    (*record_count)++;
    g_ptr_array_add(documents, doc);
    Val *fid = field(record, "flow_id");
    if (!token_truth(fid))
      fid = field(record, "ts");
    char *key = fid ? yyjson_val_write(fid, 0, NULL) : strdup("null");
    Flow *flow = g_hash_table_lookup(index, key);
    if (!flow) {
      flow = g_new0(Flow, 1);
      g_ptr_array_add(flows, flow);
      g_hash_table_insert(index, g_strdup(key), flow);
    }
    free(key);
    if (text_is(field(record, "kind"), "usage")) {
      if (!flow->has_usage)
        flow->first_usage = record;
      flow->has_usage = true;
      usage_merge(&flow->usage, field(record, "usage"));
    } else if (text_is(field(record, "kind"), "request") && !flow->request)
      flow->request = record;
    else if (text_is(field(record, "kind"), "response"))
      flow->response = record;
  }
  if (ferror(file))
    ok = false;
  free(line);
  fclose(file);
  if (ok)
    for (size_t i = 0; i < flows->len; i++) {
      Flow *flow = flows->pdata[i];
      const char *url = text_or(field(flow->request, "url"), ""),
                 *body = text_or(field(flow->request, "request_body"), "");
      if ((flow->request && !llm(url, body)) ||
          (!flow->request && !flow->has_usage))
        continue;
      Request *r = g_new0(Request, 1);
      r->index = out->len + 1;
      r->ts = g_strdup(text_or(field(flow->request, "ts"),
                               text_or(field(flow->first_usage, "ts"), "")));
      r->url = endpoint(url);
      r->model = g_strdup(text_or(field(flow->request, "model"), "grok-4.6"));
      r->status = field(flow->response, "status");
      if (!numeric_int(field(flow->request, "request_chars"), &r->chars) ||
          !r->chars)
        r->chars = chars(body);
      r->advertised = strings();
      r->calls = strings();
      r->results = strings();
      r->queries = strings();
      r->mcp = strings();
      r->ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
      r->result_chars =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
      r->usage = flow->usage;
      r->cost = input_cost(r->usage, rates) +
                (double)r->usage.value[1] * rates.output / 1000000;
      parse_payload(r, *body ? body : "{}");
      g_ptr_array_add(out, r);
    }
  g_hash_table_destroy(index);
  g_ptr_array_free(flows, TRUE);
  if (!ok) {
    g_ptr_array_free(out, TRUE);
    return NULL;
  }
  return out;
}
static Tool *tool(Attribution *attr, const char *name) {
  for (size_t i = 0; i < attr->tools->len; i++) {
    Tool *t = attr->tools->pdata[i];
    if (!strcmp(t->name, name))
      return t;
  }
  Tool *t = g_new0(Tool, 1);
  t->name = g_strdup(name);
  g_ptr_array_add(attr->tools, t);
  return t;
}
static void tool_free(gpointer p) {
  Tool *t = p;
  g_free(t->name);
  g_free(t);
}
static Attribution attribution(GPtrArray *requests, Rates rates) {
  Attribution a = {g_ptr_array_new_with_free_func(tool_free), strings(),
                   strings(), 0, 0};
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (size_t i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    double share = (double)r->schema_chars / fmax((double)r->chars, 1),
           cost = input_cost(r->usage, rates);
    a.schema_chars += r->schema_chars;
    a.schema_cost += cost * share;
    for (size_t j = 0; j < r->advertised->len; j++)
      tool(&a, r->advertised->pdata[j])->advertised++;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, r->ids);
    while (g_hash_table_iter_next(&it, &key, &value))
      if (!g_hash_table_contains(seen, key)) {
        g_hash_table_add(seen, g_strdup(key));
        tool(&a, value)->unique++;
      }
    for (size_t j = 0; j < r->calls->len; j++)
      tool(&a, r->calls->pdata[j])->wire++;
    for (size_t j = 0; j < r->results->len; j++)
      tool(&a, r->results->pdata[j])->results++;
    double total = 0;
    g_hash_table_iter_init(&it, r->result_chars);
    while (g_hash_table_iter_next(&it, &key, &value)) {
      int64_t n = *(int64_t *)value;
      tool(&a, key)->chars += n;
      total += (double)n;
    }
    double rest = fmax(cost * (1 - share), 0);
    if (rest && total) {
      g_hash_table_iter_init(&it, r->result_chars);
      while (g_hash_table_iter_next(&it, &key, &value))
        tool(&a, key)->cost += rest * ((double)*(int64_t *)value / total);
    } else if (rest && r->calls->len) {
      GPtrArray *names = strings();
      for (size_t j = 0; j < r->calls->len; j++)
        append_unique(names, r->calls->pdata[j]);
      for (size_t j = 0; j < names->len; j++)
        tool(&a, names->pdata[j])->cost += rest / names->len;
      g_ptr_array_free(names, TRUE);
    }
    for (size_t j = 0; j < r->queries->len; j++)
      append_unique(a.queries, r->queries->pdata[j]);
    for (size_t j = 0; j < r->mcp->len; j++)
      append_unique(a.mcp, r->mcp->pdata[j]);
  }
  g_hash_table_destroy(seen);
  return a;
}
static Mut *string_array(Doc *d, GPtrArray *array) {
  Mut *out = yyjson_mut_arr(d);
  for (size_t i = 0; i < array->len; i++)
    yyjson_mut_arr_add_strcpy(d, out, array->pdata[i]);
  return out;
}
static void str(Doc *d, Mut *o, const char *key, const char *value) {
  yyjson_mut_obj_add_strcpy(d, o, key, value ? value : "");
}
static Mut *usage_json(Doc *d, Usage usage) {
  Mut *out = yyjson_mut_obj(d);
  for (size_t i = 0; i < 5; i++)
    if (usage.present & (1u << i))
      yyjson_mut_obj_add_sint(d, out, usage_keys[i], usage.value[i]);
  return out;
}
static char *archive(Meta *meta, GPtrArray *requests, Attribution *a,
                     Rates rates, size_t records) {
  Doc *d = yyjson_mut_doc_new(NULL);
  Mut *root = yyjson_mut_obj(d), *m = yyjson_mut_obj(d),
      *rs = yyjson_mut_obj(d), *list = yyjson_mut_arr(d),
      *attr = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, root);
  str(d, m, "run_id", meta->run);
  str(d, m, "captured_at", meta->captured_at);
  str(d, m, "model", "grok-4.6");
  str(d, m, "provider", "xai");
  str(d, m, "task_result", meta->task_result);
  if (meta->captured)
    yyjson_mut_obj_add_sint(d, m, "exit_code", meta->exit_code);
  yyjson_mut_obj_add_uint(d, m, "request_count", requests->len);
  yyjson_mut_obj_add_uint(d, m, "record_count", records);
  yyjson_mut_obj_add_val(d, root, "meta", m);
  yyjson_mut_obj_add_real(d, rs, "input", rates.input);
  yyjson_mut_obj_add_real(d, rs, "output", rates.output);
  yyjson_mut_obj_add_real(d, rs, "cacheRead", rates.cached);
  yyjson_mut_obj_add_real(d, rs, "cacheWrite", rates.write);
  yyjson_mut_obj_add_val(d, root, "rates", rs);
  for (size_t i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    Mut *row = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, row, "index", r->index);
    str(d, row, "ts", r->ts);
    str(d, row, "url", r->url);
    yyjson_mut_obj_add_val(d, row, "status",
                           r->status ? yyjson_val_mut_copy(d, r->status)
                                     : yyjson_mut_null(d));
    str(d, row, "model", r->model);
    yyjson_mut_obj_add_sint(d, row, "request_chars", r->chars);
    yyjson_mut_obj_add_sint(d, row, "message_count", r->messages);
    yyjson_mut_obj_add_uint(d, row, "tool_count", r->advertised->len);
    yyjson_mut_obj_add_sint(d, row, "tool_schema_chars", r->schema_chars);
    const char *names[] = {"tools_advertised", "tool_calls", "tool_results",
                           "skill_queries", "mcp_calls"};
    GPtrArray *arrays[] = {r->advertised, r->calls, r->results, r->queries,
                           r->mcp};
    for (size_t j = 0; j < 5; j++)
      yyjson_mut_obj_add_val(d, row, names[j], string_array(d, arrays[j]));
    Mut *ids = yyjson_mut_obj(d), *sizes = yyjson_mut_obj(d);
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, r->ids);
    while (g_hash_table_iter_next(&it, &key, &value))
      str(d, ids, key, value);
    g_hash_table_iter_init(&it, r->result_chars);
    while (g_hash_table_iter_next(&it, &key, &value))
      yyjson_mut_obj_add_sint(d, sizes, key, *(int64_t *)value);
    yyjson_mut_obj_add_val(d, row, "call_ids", ids);
    yyjson_mut_obj_add_val(d, row, "result_chars", sizes);
    yyjson_mut_obj_add_val(d, row, "usage", usage_json(d, r->usage));
    yyjson_mut_obj_add_real(d, row, "cost_usd", r->cost);
    yyjson_mut_arr_add_val(list, row);
  }
  yyjson_mut_obj_add_val(d, root, "requests", list);
  Mut *tools = yyjson_mut_obj(d), *skills = yyjson_mut_obj(d),
      *mcp = yyjson_mut_obj(d), *advertised = yyjson_mut_obj(d);
  for (size_t i = 0; i < a->tools->len; i++) {
    Tool *t = a->tools->pdata[i];
    if (t->advertised)
      yyjson_mut_obj_add_sint(d, advertised, t->name, t->advertised);
    if (t->unique || t->wire || t->results || t->chars || t->cost) {
      Mut *row = yyjson_mut_obj(d);
      yyjson_mut_obj_add_sint(d, row, "unique_calls", t->unique);
      yyjson_mut_obj_add_sint(d, row, "wire_appearances", t->wire);
      yyjson_mut_obj_add_sint(d, row, "result_appearances", t->results);
      yyjson_mut_obj_add_sint(d, row, "result_chars", t->chars);
      yyjson_mut_obj_add_real(d, row, "cost_usd", t->cost);
      yyjson_mut_obj_add_val(d, tools, t->name, row);
      if (!strcmp(t->name, "skill_catalog") || !strcmp(t->name, "mcp")) {
        bool skill = !strcmp(t->name, "skill_catalog");
        Mut *detail = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, detail, skill ? "queries" : "notes",
                               string_array(d, skill ? a->queries : a->mcp));
        yyjson_mut_obj_add_sint(d, detail, "unique_calls", t->unique);
        yyjson_mut_obj_add_sint(d, detail, "wire_appearances", t->wire);
        yyjson_mut_obj_add_real(d, detail, "cost_usd", t->cost);
        yyjson_mut_obj_add_val(d, skill ? skills : mcp, t->name, detail);
      }
    }
  }
  yyjson_mut_obj_add_val(d, attr, "tools", tools);
  yyjson_mut_obj_add_val(d, attr, "skills", skills);
  yyjson_mut_obj_add_val(d, attr, "mcp", mcp);
  yyjson_mut_obj_add_val(d, attr, "advertised", advertised);
  yyjson_mut_obj_add_sint(d, attr, "schema_chars_total", a->schema_chars);
  yyjson_mut_obj_add_real(d, attr, "schema_cost_usd", a->schema_cost);
  yyjson_mut_obj_add_val(d, root, "attribution", attr);
  char *out = yyjson_mut_write(d, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL);
  yyjson_mut_doc_free(d);
  return out;
}
static bool rates_load(Rates *rates) {
  *rates = (Rates){2, 6, 0.5, 0};
  char *path = token_home(".pi/agent/models-store.json");
  yyjson_doc *doc = load_json(path);
  g_free(path);
  if (!doc)
    return true;
  Val *models = field(field(yyjson_doc_get_root(doc), "xai"), "models"), *item;
  size_t i, n;
  bool ok = true;
  yyjson_arr_foreach(models, i, n,
                     item) if (text_is(field(item, "id"), "grok-4.6")) {
    Val *cost = field(item, "cost");
    const char *keys[] = {"input", "output", "cacheRead", "cacheWrite"};
    double *values[] = {&rates->input, &rates->output, &rates->cached,
                        &rates->write};
    for (size_t j = 0; j < 4; j++) {
      Val *v = field(cost, keys[j]);
      if (!v)
        continue;
      if (yyjson_is_num(v))
        *values[j] = yyjson_get_num(v);
      else if (yyjson_is_bool(v))
        *values[j] = yyjson_get_bool(v) ? 1 : 0;
      else if (!number_arg(token_text(v), values[j]))
        ok = false;
      if (!isfinite(*values[j]))
        ok = false;
    }
    break;
  }
  yyjson_doc_free(doc);
  return ok;
}
int main(int argc, char **argv) {
  bool capture = argc == 1 || (argc > 1 && !strcmp(argv[1], "--capture"));
  const char *run =
      !capture && argc >= 3 && !strcmp(argv[1], "--from-run") ? argv[2] : NULL;
  if (!capture && !run) {
    fputs("usage: pi-token-report [--capture] | --from-run RUN_ID\n", stderr);
    return 2;
  }
  Rates rates;
  if (!rates_load(&rates)) {
    fputs("Invalid model rates.\n", stderr);
    return 1;
  }
  Meta meta = {0};
  meta.captured = capture;
  if (capture) {
    char *error = token_capture(&meta);
    if (error) {
      fprintf(stderr, "%s\n", error);
      g_free(error);
      return 1;
    }
  } else {
    meta.run = g_strdup(run);
    meta.task_result = g_strdup("rebuilt from capture");
  }
  if (!meta.run || !g_regex_match_simple("\\A[A-Za-z0-9][A-Za-z0-9_.-]*\\z",
                                         meta.run, 0, 0)) {
    fputs("Invalid capture run identifier.\n", stderr);
    g_free(meta.run);
    g_free(meta.task_result);
    return 1;
  }
  GDateTime *now = g_date_time_new_now_utc();
  meta.captured_at = g_date_time_format(now, "%Y-%m-%d %H:%M UTC");
  g_date_time_unref(now);
  GPtrArray *documents =
      g_ptr_array_new_with_free_func((GDestroyNotify)yyjson_doc_free);
  size_t records = 0;
  GPtrArray *requests = load_requests(meta.run, rates, documents, &records);
  int status = 1;
  if (!requests)
    goto done;
  Attribution attr = attribution(requests, rates);
  char *html = token_html(&meta, requests, &attr, rates),
       *json = archive(&meta, requests, &attr, rates, records),
       *lower = html ? g_ascii_strdown(html, -1) : NULL;
  char *out = token_state("reports"),
       *html_name = g_strconcat(meta.run, ".html", NULL),
       *json_name = g_strconcat(meta.run, ".json", NULL),
       *html_path = g_build_filename(out, html_name, NULL),
       *json_path = g_build_filename(out, json_name, NULL),
       *latest = g_build_filename(out, "latest.html", NULL);
  bool ok = html && json && !strstr(lower, "request_body") &&
            !strstr(lower, "authorization") && !strstr(lower, "api-key");
  if (ok)
    ok = g_mkdir_with_parents(out, 0777) == 0 && chmod(out, 0700) == 0 &&
         token_private_file(html_path, html, strlen(html), 0600) &&
         token_private_file(json_path, json, strlen(json), 0600);
  if (ok) {
    struct stat st;
    if (lstat(latest, &st) == 0 && unlink(latest) != 0)
      ok = false;
    if (ok && symlink(html_name, latest) != 0)
      ok = false;
  }
  if (ok) {
    puts(html_path);
    printf("run_id=%s requests=%u", meta.run, requests->len);
    if (capture)
      printf(" exit=%d", meta.exit_code);
    putchar('\n');
    if (capture)
      puts(meta.task_result);
    status = requests->len ? 0 : 2;
  }
  g_free(out);
  g_free(html_name);
  g_free(json_name);
  g_free(html_path);
  g_free(json_path);
  g_free(latest);
  g_free(html);
  free(json);
  g_free(lower);
  g_ptr_array_free(attr.tools, TRUE);
  g_ptr_array_free(attr.queries, TRUE);
  g_ptr_array_free(attr.mcp, TRUE);
  g_ptr_array_free(requests, TRUE);
done:
  g_ptr_array_free(documents, TRUE);
  g_free(meta.run);
  g_free(meta.captured_at);
  g_free(meta.task_result);
  if (status == 1)
    fputs("Token report failed; capture contents have not been logged.\n",
          stderr);
  return status;
}
