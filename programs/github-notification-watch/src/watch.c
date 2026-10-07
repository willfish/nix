#include "watch.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

yyjson_val *field(yyjson_val *object, const char *name) {
  yyjson_val *found = NULL, *key, *value;
  size_t i, n, len = strlen(name);
  yyjson_obj_foreach(object, i, n, key, value) {
    if (yyjson_get_len(key) == len && !memcmp(yyjson_get_str(key), name, len))
      found = value;
  }
  return found;
}
bool truth(yyjson_val *v) {
  if (!v || yyjson_is_null(v))
    return false;
  if (yyjson_is_bool(v))
    return yyjson_get_bool(v);
  if (yyjson_is_num(v))
    return yyjson_get_num(v) != 0;
  if (yyjson_is_str(v))
    return yyjson_get_len(v) != 0;
  return yyjson_get_len(v) != 0;
}
char *html_url(const char *url) {
  if (!url)
    return g_strdup("");
  GString *clean = g_string_new(NULL);
  for (const char *s = url; *s; s++)
    if (*s != '\t' && *s != '\r' && *s != '\n')
      g_string_append_c(clean, *s);
  const char *s = clean->str;
  while (*s && (unsigned char)*s <= 32)
    s++;
  char *result = NULL;
  if (g_ascii_strncasecmp(s, "https://", 8))
    goto finish;
  const char *host = s + 8, *path = host + strcspn(host, "/?#");
  bool api = path - host == 14 && !memcmp(host, "api.github.com", 14);
  bool web = path - host == 10 && !memcmp(host, "github.com", 10);
  if (!api && !web)
    goto finish;
  char *route = g_strndup(path, strcspn(path, "?#"));
  if (web) {
    if (*route && strcmp(route, "/"))
      result = g_strconcat("https://github.com", route, NULL);
    g_free(route);
    goto finish;
  }
  char **split = g_strsplit(route, "/", -1);
  GPtrArray *parts = g_ptr_array_new();
  for (size_t i = 0; split[i]; i++)
    if (*split[i])
      g_ptr_array_add(parts, split[i]);
  if (parts->len >= 4 && !strcmp(parts->pdata[0], "repos")) {
    const char *kind = parts->pdata[3];
    size_t rest = parts->len - 4;
    if (rest && !strcmp(parts->pdata[4], "comments"))
      goto release;
    if (!strcmp(kind, "pulls") && rest)
      kind = "pull";
    else if (!strcmp(kind, "commits") && rest)
      kind = "commit";
    else if (!strcmp(kind, "issues") && rest >= 3 &&
             !strcmp(parts->pdata[5], "comments"))
      rest = 1;
    else if (strcmp(kind, "issues") && strcmp(kind, "pull") &&
             strcmp(kind, "commit") && strcmp(kind, "discussions"))
      goto release;
    if (!rest)
      goto release;
    GString *page = g_string_new("https://github.com/");
    g_string_append_printf(page, "%s/%s/%s", (char *)parts->pdata[1],
                           (char *)parts->pdata[2], kind);
    for (size_t i = 0; i < rest; i++)
      g_string_append_printf(page, "/%s", (char *)parts->pdata[4 + i]);
    result = g_string_free(page, false);
  }
release:
  g_ptr_array_free(parts, true);
  g_strfreev(split);
  g_free(route);
finish:
  g_string_free(clean, true);
  return result ? result : g_strdup("");
}
static const char *reason_label(yyjson_val *v) {
  const char *reason = yyjson_get_str(v);
  const char *pairs[][2] = {{"assign", "Assigned"},
                            {"author", "Update"},
                            {"comment", "Comment"},
                            {"ci_activity", "CI"},
                            {"invitation", "Invitation"},
                            {"manual", "Subscribed"},
                            {"mention", "Mention"},
                            {"review_requested", "Review requested"},
                            {"security_alert", "Security alert"},
                            {"state_change", "State change"},
                            {"subscribed", "Subscribed"},
                            {"team_mention", "Team mention"}};
  for (size_t i = 0; reason && i < G_N_ELEMENTS(pairs); i++)
    if (!strcmp(reason, pairs[i][0]))
      return pairs[i][1];
  return "Notification";
}
static GBytes *identity(yyjson_val *id) {
  if (!truth(id))
    return g_bytes_new_static("", 0);
  if (yyjson_is_str(id))
    return g_bytes_new(yyjson_get_str(id), yyjson_get_len(id));
  if (yyjson_is_bool(id))
    return g_bytes_new_static("True", 4);
  char *text = yyjson_val_write(id, YYJSON_WRITE_ALLOW_INF_AND_NAN, NULL);
  GBytes *bytes = g_bytes_new(text ? text : "", text ? strlen(text) : 0);
  free(text);
  return bytes;
}
static gint compare_bytes(gconstpointer a, gconstpointer b) {
  return g_bytes_compare(a, b);
}
static yyjson_mut_val *seen_array(yyjson_mut_doc *doc, GHashTable *seen) {
  yyjson_mut_val *array = yyjson_mut_arr(doc);
  GList *keys = g_list_sort(g_hash_table_get_keys(seen), compare_bytes);
  for (GList *it = keys; it; it = it->next) {
    gsize len;
    const char *text = g_bytes_get_data(it->data, &len);
    yyjson_mut_arr_append(array, yyjson_mut_strncpy(doc, text, len));
  }
  g_list_free(keys);
  return array;
}
static void add_text(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                     const char *text) {
  yyjson_mut_obj_add_strcpy(doc, obj, key, text);
}
static void add_notice(yyjson_mut_doc *doc, yyjson_mut_val *out,
                       const char *body) {
  yyjson_mut_val *item = yyjson_mut_obj(doc);
  add_text(doc, item, "summary", "GitHub");
  add_text(doc, item, "body", body);
  yyjson_mut_arr_append(out, item);
}
yyjson_mut_doc *plan(yyjson_val *items, yyjson_val *state) {
  if (!yyjson_is_arr(items) || (state && !yyjson_is_obj(state)))
    return NULL;
  GHashTable *seen = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                           (GDestroyNotify)g_bytes_unref, NULL);
  yyjson_val *old_seen = field(state, "seen"), *v;
  size_t i, n;
  if (truth(old_seen) && !yyjson_is_arr(old_seen))
    goto invalid;
  yyjson_arr_foreach(old_seen, i, n, v) {
    if (!yyjson_is_str(v))
      goto invalid;
    g_hash_table_add(seen, g_bytes_new(yyjson_get_str(v), yyjson_get_len(v)));
  }
  bool seeded = truth(field(state, "seeded"));
  if (!seeded)
    g_hash_table_remove_all(seen);
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc), *out = yyjson_mut_arr(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_val(doc, root, "announcements", out);
  GPtrArray *current =
      g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
  size_t fresh = 0;
  yyjson_arr_foreach(items, i, n, v) {
    if (!yyjson_is_obj(v)) {
      g_ptr_array_free(current, true);
      yyjson_mut_doc_free(doc);
      goto invalid;
    }
    GBytes *id = identity(field(v, "id"));
    if (!g_bytes_get_size(id)) {
      g_bytes_unref(id);
      continue;
    }
    g_ptr_array_add(current, id);
    if (!seeded || g_hash_table_contains(seen, id))
      continue;
    fresh++;
    if (fresh > 8)
      continue;
    yyjson_val *subject = field(v, "subject"),
               *repository = field(v, "repository");
    yyjson_val *repo = field(repository, "full_name"),
               *title = field(subject, "title");
    if ((truth(repo) && (!yyjson_is_str(repo) || strlen(yyjson_get_str(repo)) !=
                                                     yyjson_get_len(repo))) ||
        (truth(title) &&
         (!yyjson_is_str(title) ||
          strlen(yyjson_get_str(title)) != yyjson_get_len(title)))) {
      g_ptr_array_free(current, true);
      yyjson_mut_doc_free(doc);
      goto invalid;
    }
    const char *repo_text = truth(repo) ? yyjson_get_str(repo) : "GitHub";
    const char *title_text =
        truth(title) ? yyjson_get_str(title) : "GitHub notification";
    char *url = html_url(yyjson_get_str(field(subject, "url")));
    if (!*url) {
      g_free(url);
      url = html_url(yyjson_get_str(field(subject, "latest_comment_url")));
    }
    char *body = g_strdup_printf("%s: %s%s%s", reason_label(field(v, "reason")),
                                 title_text, *url ? "\n" : "", url);
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    add_text(doc, item, "summary", repo_text);
    add_text(doc, item, "body", body);
    add_text(doc, item, "url", url);
    yyjson_mut_arr_append(out, item);
    g_free(body);
    g_free(url);
  }
  if (!seeded && current->len) {
    char *body = g_strdup_printf("%u unread notifications already waiting",
                                 current->len);
    add_notice(doc, out, body);
    g_free(body);
  } else if (fresh > 8) {
    char *body = g_strdup_printf("%zu more new notifications", fresh - 8);
    add_notice(doc, out, body);
    g_free(body);
  }
  // Do not deduplicate within a single response: the old planner preserves
  // response order and counts repeated fresh threads in its burst summary.
  for (size_t index = 0; index < current->len; index++)
    g_hash_table_add(seen, g_bytes_ref(current->pdata[index]));
  g_ptr_array_free(current, true);
  yyjson_mut_obj_add_val(doc, root, "seen", seen_array(doc, seen));
  yyjson_mut_obj_add_bool(doc, root, "seeded", true);
  g_hash_table_destroy(seen);
  return doc;
invalid:
  g_hash_table_destroy(seen);
  return NULL;
}
char **notify_command(yyjson_val *item) {
  const char *summary = yyjson_get_str(field(item, "summary")),
             *body = yyjson_get_str(field(item, "body")),
             *url = yyjson_get_str(field(item, "url"));
  if (!summary || !body ||
      strlen(summary) != yyjson_get_len(field(item, "summary")) ||
      strlen(body) != yyjson_get_len(field(item, "body")))
    return NULL;
  const char *base[] = {"notify-send",
                        "-a",
                        "GitHub",
                        "-i",
                        "github",
                        "-u",
                        "normal",
                        "-t",
                        "30000",
                        "-h",
                        "string:desktop-entry:github-notifications"};
  GPtrArray *args = g_ptr_array_new();
  for (size_t i = 0; i < G_N_ELEMENTS(base); i++)
    g_ptr_array_add(args, g_strdup(base[i]));
  if (url && *url) {
    g_ptr_array_add(args, g_strdup("-A"));
    g_ptr_array_add(args, g_strdup("open=Open"));
  }
  g_ptr_array_add(args, g_strdup("--"));
  g_ptr_array_add(args, g_strdup(summary));
  g_ptr_array_add(args, g_strdup(body));
  g_ptr_array_add(args, NULL);
  return (char **)g_ptr_array_free(args, false);
}
bool save_state(const char *path, yyjson_mut_val *seen) {
  char *parent = g_path_get_dirname(path);
  bool ok = g_mkdir_with_parents(parent, 0777) == 0;
  g_free(parent);
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_bool(doc, root, "seeded", true);
  yyjson_mut_obj_add_val(doc, root, "seen", yyjson_mut_val_mut_copy(doc, seen));
  char *json = yyjson_mut_write(doc, YYJSON_WRITE_ESCAPE_UNICODE, NULL);
  char *payload = json ? g_strconcat(json, "\n", NULL) : NULL;
  char *temporary =
      g_strdup_printf("%.*s.tmp", (int)(strlen(path) - strlen(".json")), path);
  // Write the same-directory temporary first, then atomically replace only
  // after every notification process has been started.
  if (ok && payload) {
    FILE *file = fopen(temporary, "w");
    if (!file)
      ok = false;
    else {
      bool wrote = fwrite(payload, 1, strlen(payload), file) == strlen(payload);
      bool closed = fclose(file) == 0;
      ok = wrote && closed && g_rename(temporary, path) == 0;
    }
  } else
    ok = false;
  g_free(temporary);
  g_free(payload);
  free(json);
  yyjson_mut_doc_free(doc);
  return ok;
}
