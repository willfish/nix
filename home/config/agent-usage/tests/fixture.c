#include "usage.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Val *responses, *requests;
static size_t response_index;
static Reply fake(Doc *d, const char *url, const char *token,
                  const char *headers, const char *form) {
  Val *request = yyjson_mut_obj(d);
  strput(d, request, "url", url);
  strput(d, request, "token", token);
  strput(d, request, "headers", headers);
  strput(d, request, "form", form);
  yyjson_mut_arr_append(requests, request);
  Val *v = yyjson_mut_arr_get(responses, response_index++);
  return (Reply){.status = number(get(v, "status")),
                 .failed = truth(get(v, "failed")),
                 .body = get(v, "body")};
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  char *line = NULL;
  size_t capacity = 0;
  if (getline(&line, &capacity, stdin) < 0)
    return 2;
  Doc *d = yyjson_mut_doc_new(NULL);
  Val *args = parse(d, line), *v = get(args, "value"), *out = NULL;
  double now = yyjson_mut_get_num(get(args, "now"));
  const char *op = argv[1];
  if (!strcmp(op, "number"))
    out = yyjson_mut_sint(d, number(v));
  else if (!strcmp(op, "fraction"))
    out = yyjson_mut_real(d, fraction(v, truth(get(args, "percent"))));
  else if (!strcmp(op, "day")) {
    char day[11];
    local_day(v, now, day);
    out = yyjson_mut_strcpy(d, day);
  } else if (!strcmp(op, "timestamp"))
    out = yyjson_mut_strcpy(d, iso_timestamp(d, v));
  else if (!strcmp(op, "empty"))
    out = empty_stats(d, now);
  else if (!strcmp(op, "pi"))
    out = scan_pi(d, text(get(args, "path")), text(get(args, "provider")), now);
  else if (!strcmp(op, "grok"))
    out = scan_grok(d, text(get(args, "path")), now);
  else if (!strcmp(op, "opencode"))
    out = scan_opencode(d, text(get(args, "path")), now);
  else if (!strcmp(op, "billing"))
    out = parse_grok(d, v);
  else if (!strcmp(op, "codex") || !strcmp(op, "go")) {
    Allowance a =
        !strcmp(op, "codex") ? parse_codex(d, v, now) : parse_opencode(d, v);
    out = yyjson_mut_obj(d);
    put(d, out, "limits", a.limits);
    strput(d, out, "plan", a.tier);
    strput(d, out, "status", a.status);
  } else if (!strcmp(op, "tier"))
    out = yyjson_mut_strcpy(d, grok_tier(d, v));
  else if (!strcmp(op, "login")) {
    const char *key = "";
    Val *entry = grok_login(v, now, &key);
    out = yyjson_mut_obj(d);
    strput(d, out, "key", key);
    put(d, out, "entry", entry ? copy(d, entry) : yyjson_mut_null(d));
  } else if (!strcmp(op, "merge_login"))
    out = merge_login(d, get(args, "entry"), get(args, "payload"), now);
  else if (!strcmp(op, "merge"))
    out = merge_oauth(d, get(args, "entry"), get(args, "payload"),
                      number(get(args, "now_ms")));
  else if (!strcmp(op, "current"))
    out = yyjson_mut_bool(d, token_current(v, number(get(args, "now_ms")),
                                           get(args, "skew_ms")
                                               ? number(get(args, "skew_ms"))
                                               : 300000));
  else if (!strcmp(op, "key"))
    out = yyjson_mut_strcpy(d, extract_opencode_key(v));
  else if (!strcmp(op, "command")) {
    char *key = command_key(text(v), (int)number(get(args, "timeout_ms")));
    out = yyjson_mut_strcpy(d, key);
    free(key);
  } else if (!strcmp(op, "waybar"))
    out = waybar(d, v);
  else if (!strcmp(op, "jwt"))
    out = jwt_claims(d, text(v));
  else if (!strcmp(op, "collect")) {
    requests = yyjson_mut_arr(d);
    responses = get(args, "replies");
    Context ctx = {.now = now, .transport = fake};
    out = yyjson_mut_obj(d);
    put(d, out, "record", collect(d, &ctx, text(get(args, "id"))));
    put(d, out, "requests", requests);
  } else if (!strcmp(op, "probe")) {
    Reply reply = http_request(
        d, text(get(args, "url")),
        truth(get(args, "token")) ? text(get(args, "token")) : NULL,
        text(get(args, "headers")),
        truth(get(args, "form")) ? text(get(args, "form")) : NULL);
    out = yyjson_mut_obj(d);
    put(d, out, "body", reply.body ? reply.body : yyjson_mut_null(d));
    intput(d, out, "status", reply.status);
    put(d, out, "failed", yyjson_mut_bool(d, reply.failed));
  }
  char *result = out ? encode(out) : NULL;
  bool ok = result != NULL;
  if (result)
    puts(result);
  free(result);
  free(line);
  yyjson_mut_doc_free(d);
  curl_global_cleanup();
  return ok ? 0 : 1;
}
