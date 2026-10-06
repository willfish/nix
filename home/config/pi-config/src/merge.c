#include "config.h"
#include <string.h>
static Json *get(Json *object, const char *key) {
  return json_get(object, key, strlen(key));
}
static void set(Json *object, const char *key, Json *value) {
  json_set(object, key, strlen(key), value);
}
static bool is_string(Json *value, const char *text) {
  return value && value->kind == J_STRING &&
         value->string->len == strlen(text) &&
         !memcmp(value->string->str, text, value->string->len);
}
static bool boolean(Json *value, bool expected) {
  return value && value->kind == J_BOOL && value->boolean == expected;
}
static bool unhashable(Json *value) {
  return value && (value->kind == J_OBJECT || value->kind == J_ARRAY);
}
static bool old_pair(Json *provider, Json *model) {
  return (is_string(provider, "xai") &&
          (is_string(model, "grok-4.6") || is_string(model, "grok-4.7"))) ||
         (is_string(provider, "opencode-go") &&
          (is_string(model, "deepseek-v4.1-flash") ||
           is_string(model, "glm-5.3") ||
           is_string(model, "space-bunny-free")));
}
int config_settings(const char *defaults_path, const char *settings_path) {
  Json *defaults = config_load(defaults_path, false), *settings = NULL;
  int status = 1;
  if (!defaults)
    goto done;
  settings = config_load(settings_path, true);
  if (!settings)
    goto done;
  bool changed = false;
  for (size_t i = 0; i < defaults->keys->len; i++) {
    GString *key = defaults->keys->pdata[i];
    if (!json_get(settings, key->str, key->len)) {
      json_set(settings, key->str, key->len,
               json_ref(defaults->values->pdata[i]));
      changed = true;
    }
  }
  Json *provider = get(settings, "defaultProvider"),
       *model = get(settings, "defaultModel");
  if (unhashable(provider) || unhashable(model))
    goto done;
  if (old_pair(provider, model)) {
    const char *names[] = {"defaultProvider", "defaultModel"};
    for (size_t i = 0; i < 2; i++) {
      Json *value = get(defaults, names[i]);
      set(settings, names[i], value ? json_ref(value) : json_node(J_NULL));
    }
    changed = true;
  }
  bool marker = false;
  Json *declared = get(defaults, "observational-memory-jev");
  if (declared && declared->kind == J_OBJECT &&
      boolean(get(declared, "enabledByDefault"), false)) {
    char *path = config_sibling(settings_path, ".om-default-off-migrated");
    if (!path)
      goto done;
    int exists = config_exists(path);
    g_free(path);
    if (exists < 0)
      goto done;
    marker = !exists;
    if (marker) {
      Json *current = get(settings, "observational-memory-jev");
      if (current && current->kind == J_OBJECT &&
          boolean(get(current, "enabledByDefault"), true)) {
        set(current, "enabledByDefault", json_node(J_BOOL));
        changed = true;
      }
    }
  }
  if (changed && !config_save(settings_path, settings))
    goto done;
  if (marker && !config_marker(settings_path))
    goto done;
  status = 0;
done:
  json_free(defaults);
  json_free(settings);
  return status;
}
int config_auth(const char *path, GPtrArray *drop, GString *provider,
                const char *refresh_path, const char *account_path) {
  GString *refresh = NULL, *account = NULL;
  Json *auth = NULL;
  int status = 1;
  int have_files = 0;
  if (refresh_path && account_path) {
    have_files = config_exists(refresh_path);
    if (have_files < 0)
      goto done;
    if (have_files)
      have_files = config_exists(account_path);
    if (have_files < 0)
      goto done;
  }
  if (have_files) {
    refresh = config_read(refresh_path);
    if (!refresh)
      goto done;
    refresh = config_strip(refresh);
    account = config_read(account_path);
    if (!account)
      goto done;
    account = config_strip(account);
  }
  auth = config_load(path, true);
  if (!auth)
    goto done;
  bool changed = false;
  for (size_t i = 0; i < drop->len; i++) {
    GString *key = drop->pdata[i];
    changed = json_remove(auth, key->str, key->len) || changed;
  }
  if (provider && provider->len && refresh && refresh->len && account &&
      account->len) {
    Json *current = json_get(auth, provider->str, provider->len);
    if (!current || current->kind != J_OBJECT ||
        !is_string(get(current, "type"), "oauth")) {
      Json *entry = json_node(J_OBJECT);
      set(entry, "type", json_text("oauth", 5));
      set(entry, "refresh", json_text(refresh->str, refresh->len));
      set(entry, "accountId", json_text(account->str, account->len));
      set(entry, "access", json_text("", 0));
      set(entry, "expires", json_parse("0"));
      json_set(auth, provider->str, provider->len, entry);
      changed = true;
    }
  }
  if (changed && !config_save(path, auth))
    goto done;
  status = 0;
done:
  json_free(auth);
  if (refresh)
    g_string_free(refresh, true);
  if (account)
    g_string_free(account, true);
  return status;
}
