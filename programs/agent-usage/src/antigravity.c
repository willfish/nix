#include "usage.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Val *field(Val *v, const char *camel, const char *snake) {
  Val *out = get(v, camel);
  return out ? out : get(v, snake);
}

Allowance parse_antigravity(Doc *d, Val *payload) {
  Allowance a = {.limits = yyjson_mut_arr(d), .tier = "", .status = "",
                 .note = "Quota used"};
  Val *command = get(payload, "command");
  if (strcmp(text(get(payload, "status")), "SUCCESS") ||
      strcmp(text(get(command, "name")), "usage"))
    return a;
  size_t i, n;
  Val *group;
  yyjson_mut_arr_foreach(get(get(command, "data"), "groups"), i, n, group) {
    const char *name = text(field(group, "displayName", "display_name"));
    if (!*name)
      name = text(get(group, "name"));
    if (!*name)
      continue;
    if (!strcmp(name, "Gemini Models"))
      name = "Gemini";
    else if (!strcmp(name, "Claude and GPT models"))
      name = "Claude + GPT";
    size_t j, count;
    Val *bucket;
    yyjson_mut_arr_foreach(get(group, "buckets"), j, count, bucket) {
      if (truth(get(bucket, "disabled")))
        continue;
      Val *left = field(bucket, "remainingFraction", "remaining_fraction");
      Val *balance = get(bucket, "remaining");
      if (!left)
        left = field(balance, "remainingFraction", "remaining_fraction");
      if (!left && !strcmp(text(get(balance, "case")), "remainingFraction"))
        left = get(balance, "value");
      if (!yyjson_mut_is_num(left))
        continue;
      double remaining = yyjson_mut_get_num(left);
      if (!isfinite(remaining) || remaining < 0 || remaining > 1)
        continue;
      const char *window = text(field(bucket, "displayName", "display_name"));
      if (!*window)
        window = text(get(bucket, "name"));
      if (!*window)
        window = text(field(bucket, "bucketId", "bucket_id"));
      if (!*window)
        window = text(get(bucket, "id"));
      // The panel's percent is USED, while native names say "Limit Remaining".
      const char *period = text(get(bucket, "window"));
      if (!strcmp(period, "weekly"))
        window = "Weekly";
      else if (!strcmp(period, "5h"))
        window = "5-hour";
      if (!*window)
        continue;
      size_t size = size_add(size_add(strlen(name), strlen(window)), 2);
      char *title = allocate(size);
      snprintf(title, size, "%s %s", name, window);
      Val *v = yyjson_mut_obj(d);
      strput(d, v, "label", title);
      strput(d, v, "title", title);
      put(d, v, "percent", yyjson_mut_real(d, 1 - remaining));
      strput(d, v, "resetsAt", iso_timestamp(d, field(bucket, "resetTime", "reset_time")));
      yyjson_mut_arr_append(a.limits, v);
      free(title);
    }
  }
  return a;
}

Val *antigravity_record(Doc *d, Context *ctx, Val *payload) {
  Allowance a = parse_antigravity(d, payload);
  bool fetched = yyjson_mut_arr_size(a.limits) > 0;
  const char *help = "";
  if (!fetched) {
    a.status = "Limits unavailable";
    help = "Run agy in a terminal and sign in. Quota checks use its system-keyring login.";
    a.note = help;
  }
  double now = context_now(ctx);
  Val *out = record(d, "antigravity", "Antigravity CLI", empty_stats(d, now), a, help, false, now);
  put(d, out, "hasLocalStats", yyjson_mut_bool(d, false));
  put(d, out, "hasPromptStats", yyjson_mut_bool(d, false));
  put(d, out, "showWhenUnavailable", yyjson_mut_bool(d, true));
  if (fetched)
    intput(d, out, "limitsFetchedAt", (int64_t)(now * 1000));
  return out;
}

Val *collect_antigravity(Doc *d, Context *ctx) {
  char *report = command_key("!omarchy-antigravity-usage", 40000);
  Val *out = antigravity_record(d, ctx, parse(d, report));
  free(report);
  return out;
}
