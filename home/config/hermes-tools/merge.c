#define _POSIX_C_SOURCE 200809L
#include "managed.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static bool text_is(Value *v, const char *s) {
  return v && v->kind == TEXT && v->length == strlen(s) &&
         !memcmp(v->text, s, v->length);
}
static bool is_true(Value *v) { return v && v->kind == BOOL && v->boolean; }
Value *merge_profile(Value *original, Value *overlay) {
  if (!map(original) || !map(overlay)) {
    failed = true;
    return NULL;
  }
  Value *out = copy(original);
  for (size_t i = 0; i < overlay->keys->len && !failed; i++) {
    Value *k = overlay->keys->pdata[i], *v = overlay->values->pdata[i];
    const char *key = cstring(k);
    if (!strcmp(key, "custom_providers")) {
      Value *providers = get(out, key);
      if (!providers) {
        providers = value(SEQUENCE);
        put(out, key, providers);
      }
      if (!array(providers) || !array(v)) {
        failed = true;
        break;
      }
      for (size_t n = 0; n < v->values->len; n++) {
        Value *p = v->values->pdata[n], *name = get(p, "name");
        if (!map(p) || !name) {
          failed = true;
          break;
        }
        size_t at = 0;
        for (; at < providers->values->len; at++) {
          Value *existing = providers->values->pdata[at];
          if (!map(existing)) {
            failed = true;
            break;
          }
          if (equal(get(existing, "name"), name)) {
            providers->values->pdata[at] = merge_profile(existing, p);
            break;
          }
        }
        if (at == providers->values->len)
          append(providers, copy(p));
      }
    } else if (map(v) && map(get(out, key)))
      put(out, key, merge_profile(get(out, key), v));
    else
      put(out, key, copy(v));
  }
  return out;
}
static Value *wanted(Value *house) {
  if (!map(house))
    return NULL;
  Value *topics = get(house, "topics"), *group = get(house, "group_id");
  if (truth(topics) && !map(topics)) {
    failed = true;
    return NULL;
  }
  if (!truth(topics) || !get(topics, "Qwen") || !group)
    return NULL;
  Value *v = value(MAPPING);
  put(v, "name", string("telegram-qwen"));
  put(v, "platform", string("telegram"));
  char *chat = as_text(group), *thread = as_text(get(topics, "Qwen"));
  put(v, "chat_id", string(chat));
  put(v, "thread_id", string(thread));
  g_free(chat);
  g_free(thread);
  put(v, "profile", string("qwen"));
  put(v, "enabled", boolean(true));
  return v;
}
static bool same_route(Value *route, Value *want) {
  if (!map(route))
    return false;
  const char *names[] = {"name", "platform", "profile"};
  for (size_t i = 0; i < 3; i++)
    if (!equal(get(route, names[i]), get(want, names[i])))
      return false;
  for (size_t i = 0; i < 2; i++) {
    const char *key = i ? "thread_id" : "chat_id";
    Value *v = get(route, key);
    char *s = truth(v) ? as_text(v) : g_strdup("");
    bool same = text_is(get(want, key), s);
    g_free(s);
    if (!same)
      return false;
  }
  Value *enabled = get(route, "enabled");
  return !enabled || truth(enabled);
}
bool merge_routes(Value *config, Value *house) {
  if (!map(config)) {
    failed = true;
    return false;
  }
  Value *gateway = get(config, "gateway");
  if (gateway && gateway->kind != NIL && !map(gateway)) {
    failed = true;
    return false;
  }
  if (gateway && gateway->kind == NIL)
    gateway = NULL;
  Value *want = wanted(house), *routes = get(config, "profile_routes");
  if (!truth(routes))
    routes = value(SEQUENCE);
  if (!array(routes)) {
    failed = true;
    return false;
  }
  if (want && is_true(get(config, "multiplex_profiles")) &&
      is_true(get(gateway, "multiplex_profiles"))) {
    for (size_t i = 0; i < routes->values->len; i++)
      if (same_route(routes->values->pdata[i], want))
        return false;
  }
  Value *kept = value(SEQUENCE);
  for (size_t i = 0; i < routes->values->len; i++) {
    Value *r = routes->values->pdata[i];
    if (!text_is(get(r, "name"), "telegram-qwen"))
      append(kept, r);
  }
  if (!want && kept->values->len == routes->values->len)
    return false;
  if (!gateway) {
    gateway = value(MAPPING);
    put(config, "gateway", gateway);
  }
  if (want) {
    put(config, "multiplex_profiles", boolean(true));
    put(gateway, "multiplex_profiles", boolean(true));
    append(kept, want);
  }
  put(config, "profile_routes", kept);
  Value *mirrored = value(SEQUENCE);
  for (size_t i = 0; i < kept->values->len; i++) {
    Value *r = kept->values->pdata[i];
    if (!map(r)) {
      failed = true;
      return false;
    }
    append(mirrored, copy(r));
  }
  put(gateway, "profile_routes", mirrored);
  return true;
}
bool inject_key(Value *profile, Value *key) {
  Value *model = get(profile, "model");
  if (!model) {
    model = value(MAPPING);
    put(profile, "model", model);
  }
  if (!map(model)) {
    failed = true;
    return false;
  }
  put(model, "api_key", copy(key));
  Value *providers = get(profile, "custom_providers");
  if (providers) {
    if (!array(providers)) {
      failed = true;
      return false;
    }
    for (size_t i = 0; i < providers->values->len; i++) {
      Value *p = providers->values->pdata[i];
      if (!map(p)) {
        failed = true;
        return false;
      }
      if (text_is(get(p, "name"), "qwen-local"))
        put(p, "api_key", copy(key));
    }
  }
  return !failed;
}
bool apply_profile(const char *profile, const char *overlay,
                   const char *key_file) {
  if (symlinked(profile)) {
    failed = true;
    return false;
  }
  char *original_text = exists(profile) ? read_text(profile) : g_strdup("");
  if (!original_text)
    return false;
  Value *original = parse_yaml(original_text, strlen(original_text));
  if (!truth(original))
    original = value(MAPPING);
  Value *updated = merge_profile(original, read_json(overlay));
  if (!failed && key_file && regular(key_file)) {
    char *key = strip(read_text(key_file));
    if (!key || !*key)
      failed = true;
    else
      inject_key(updated, string(key));
    g_free(key);
  }
  if (failed) {
    g_free(original_text);
    return false;
  }
  if (equal(original, updated) || failed) {
    g_free(original_text);
    return false;
  }
  if (!parents(profile, 0700)) {
    g_free(original_text);
    return false;
  }
  if (*original_text) {
    GDateTime *now = g_date_time_new_now_utc();
    char *stamp = g_date_time_format(now, "%Y%m%dT%H%M%S%fZ"),
         *backup = g_strconcat(profile, ".before-local-llm-", stamp, NULL);
    g_date_time_unref(now);
    g_free(stamp);
    int fd = open(backup, O_WRONLY | O_CREAT | O_EXCL, 0600);
    g_free(backup);
    bool ok = fd >= 0;
    for (size_t at = 0, n = strlen(original_text); ok && at < n;) {
      ssize_t count = write(fd, original_text + at, n - at);
      if (count <= 0)
        ok = false;
      else
        at += (size_t)count;
    }
    if (fd >= 0 && close(fd) != 0)
      ok = false;
    if (!ok)
      failed = true;
  }
  g_free(original_text);
  GBytes *data = failed ? NULL : yaml_bytes(updated);
  bool changed =
      !failed && atomic_write(profile, data, 0600, ".local-llm-", false);
  if (data)
    g_bytes_unref(data);
  return changed;
}
bool apply_routes(const char *path, const char *house_path) {
  if (!regular(path))
    return false;
  GBytes *data = read_bytes(path);
  if (!data)
    return false;
  gsize n;
  const char *s = g_bytes_get_data(data, &n);
  Value *config = parse_yaml(s, n);
  g_bytes_unref(data);
  if (config && config->kind == NIL)
    config = value(MAPPING);
  Value *house = house_path ? read_json(house_path) : NULL;
  if (failed)
    return false;
  bool changed = merge_routes(config, house);
  if (changed && !failed) {
    data = yaml_bytes(config);
    changed = atomic_write(path, data, 0600, ".hermes-managed-", true);
    if (data)
      g_bytes_unref(data);
  }
  return changed;
}
