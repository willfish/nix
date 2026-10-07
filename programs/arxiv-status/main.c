#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

static char *join(const char *base, const char *tail) {
  size_t a = strlen(base), b = strlen(tail);
  if (a > SIZE_MAX - b - 2)
    return NULL;
  char *path = malloc(a + b + 2);
  if (path)
    snprintf(path, a + b + 2, "%s%s%s", base, a ? "/" : "", tail);
  return path;
}

static yyjson_doc *load(const char *base, const char *tail) {
  char *path = join(base, tail);
  if (!path)
    return NULL;
  yyjson_doc *doc =
      yyjson_read_file(path, YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL);
  free(path);
  if (doc && !yyjson_is_obj(yyjson_doc_get_root(doc))) {
    yyjson_doc_free(doc);
    return NULL;
  }
  return doc;
}

// JSON objects retain the last value for a repeated key, like the old adapter.
static yyjson_val *field(yyjson_doc *doc, const char *name) {
  yyjson_val *found = NULL, *key, *value;
  size_t i, n, len = strlen(name);
  yyjson_obj_foreach(yyjson_doc_get_root(doc), i, n, key, value) {
    if (yyjson_get_len(key) == len && !memcmp(yyjson_get_str(key), name, len))
      found = value;
  }
  return found;
}

static size_t array_size(yyjson_val *v) {
  return yyjson_is_arr(v) ? yyjson_arr_size(v) : 0;
}
static size_t text_size(yyjson_val *v) {
  return yyjson_is_str(v) ? yyjson_get_len(v) : 0;
}

int main(void) {
  const char *home = getenv("HOME"), *state_home = getenv("XDG_STATE_HOME"),
             *config_home = getenv("XDG_CONFIG_HOME");
  if (!home)
    home = "";
  char *default_state = join(home, ".local/state"),
       *default_config = join(home, ".config");
  if (!default_state || !default_config) {
    free(default_state);
    free(default_config);
    return 1;
  }
  // Explicitly empty XDG variables select relative paths, not HOME defaults.
  if (!state_home)
    state_home = default_state;
  if (!config_home)
    config_home = default_config;
  yyjson_doc *state = load(state_home, "omarchy-arxiv-scanner/state.json");
  yyjson_doc *viewed =
      load(state_home, "omarchy-arxiv-scanner/last_viewed.json");
  yyjson_doc *config = load(config_home, "omarchy-arxiv-scanner/config.json");
  size_t total = array_size(field(state, "area_matches")) +
                 array_size(field(state, "watched_matches"));
  yyjson_val *updated = field(state, "updated_at"),
             *seen = field(viewed, "viewed_at"),
             *category = field(config, "category");
  size_t updated_len = text_size(updated), seen_len = text_size(seen),
         category_len = text_size(category);
  int unseen = updated_len && (updated_len != seen_len ||
                               memcmp(yyjson_get_str(updated),
                                      yyjson_get_str(seen), updated_len));
  const char *label = category_len ? yyjson_get_str(category) : "arXiv";
  if (!category_len)
    category_len = strlen(label);
  int unconfigured = !array_size(field(config, "interestAreas")) &&
                     !array_size(field(config, "watchedAuthors"));
  char suffix[200], text[32];
  snprintf(suffix, sizeof(suffix),
           ": %zu match(es)%s%s · right-click scans now", total,
           unseen ? " · not opened since the last scan" : "",
           unconfigured ? " · set interests in the panel" : "");
  if (total)
    snprintf(text, sizeof(text), "%s%zu", unseen ? "!" : "", total);
  else
    snprintf(text, sizeof text, "%s", "\uf0c3");
  size_t suffix_len = strlen(suffix);
  char *tooltip = category_len > SIZE_MAX - suffix_len - 1
                      ? NULL
                      : malloc(category_len + suffix_len + 1);
  yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
  if (!tooltip || !out) {
    free(tooltip);
    yyjson_mut_doc_free(out);
    yyjson_doc_free(state);
    yyjson_doc_free(viewed);
    yyjson_doc_free(config);
    free(default_state);
    free(default_config);
    return 1;
  }
  memcpy(tooltip, label, category_len);
  memcpy(tooltip + category_len, suffix, suffix_len + 1);
  yyjson_mut_val *root = yyjson_mut_obj(out);
  yyjson_mut_doc_set_root(out, root);
  yyjson_mut_obj_add_str(out, root, "text", text);
  yyjson_mut_obj_add_val(
      out, root, "tooltip",
      yyjson_mut_strncpy(out, tooltip, category_len + suffix_len));
  yyjson_mut_obj_add_str(out, root, "class", unseen ? "unseen" : "idle");
  char *json = yyjson_mut_write(out, YYJSON_WRITE_ESCAPE_UNICODE, NULL);
  int result = json && puts(json) >= 0 ? 0 : 1;
  free(json);
  free(tooltip);
  yyjson_mut_doc_free(out);
  yyjson_doc_free(state);
  yyjson_doc_free(viewed);
  yyjson_doc_free(config);
  free(default_state);
  free(default_config);
  return result;
}
