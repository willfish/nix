#include "focus.h"
#include <stdio.h>
#include <string.h>
static bool option(const char *arg, const char *name) {
  size_t n = strlen(arg);
  return n >= 3 && n <= strlen(name) && !strncmp(arg, name, n);
}
static void usage(FILE *out) {
  fputs("usage: herdr-notification-focus [-h] {remember,open} ...\n", out);
}
int focus_main(int argc, char **argv) {
  focus_error = false;
  if (argc > 1 && (!strcmp(argv[1], "-h") || option(argv[1], "--help"))) {
    usage(stdout);
    return 0;
  }
  if (argc < 2 || (strcmp(argv[1], "remember") && strcmp(argv[1], "open"))) {
    usage(stderr);
    return 2;
  }
  const char *summary = "", *body = "", *ts = "", *pid_text = "0";
  for (int i = 2; i < argc; i++) {
    const char *arg = argv[i];
    if (!strcmp(arg, "-h") || option(arg, "--help")) {
      usage(stdout);
      return 0;
    }
    char *key = g_strdup(arg), *at = strchr(key, '=');
    const char *value = NULL;
    if (at) {
      *at = 0;
      value = arg + (at - key) + 1;
    }
    const char **dest = option(key, "--summary") ? &summary
                        : option(key, "--body")  ? &body
                        : option(key, "--ts")    ? &ts
                        : option(key, "--pid")   ? &pid_text
                                                 : NULL;
    g_free(key);
    if (!dest || (!value && i + 1 >= argc)) {
      usage(stderr);
      return 2;
    }
    if (!value) {
      value = argv[++i];
      if (value[0] == '-' && value[1] &&
          !g_regex_match_simple("\\A(?:-\\d+|-\\d*\\.\\d+)\\z", value, 0, 0)) {
        usage(stderr);
        return 2;
      }
    }
    if (!g_utf8_validate(value, -1, NULL)) {
      focus_error = true;
      break;
    }
    *dest = value;
  }
  GPtrArray *sockets = socket_paths();
  char *cache = cache_path();
  char *pid = g_strdup("0");
  yyjson_doc *clients = NULL;
  if (!strcmp(argv[1], "open")) {
    if (digit_text(pid_text)) {
      g_free(pid);
      pid = integer_text(pid_text);
    }
    if (!focus_error)
      clients = hypr_clients();
  }
  Doc *toast = parse_toast(summary, body, "");
  yyjson_doc *parsed = toast ? yyjson_mut_doc_imut_copy(toast, NULL) : NULL;
  Val *target_toast = parsed ? yyjson_doc_get_root(parsed) : NULL;
  Doc *target = NULL;
  char *sock = NULL;
  yyjson_doc *response = NULL, *target_raw = NULL;
  if (!strcmp(argv[1], "remember")) {
    for (size_t i = 0; toast && i < sockets->len && !focus_error; i++) {
      response = herdr_call(sockets->pdata[i], "session.snapshot", NULL);
      target = resolve_target(snapshot_from(response), target_toast);
      if (response) {
        yyjson_doc_free(response);
        response = NULL;
      }
      if (target) {
        yyjson_mut_obj_add_strcpy(target, yyjson_mut_doc_get_root(target),
                                  "socket", sockets->pdata[i]);
        target_raw = yyjson_mut_doc_imut_copy(target, NULL);
        remember(summary, body, ts, yyjson_doc_get_root(target_raw), cache);
        break;
      }
    }
    goto done;
  }
  if (toast) {
    Doc *stored = recall(summary, body, ts, cache);
    yyjson_doc *old = stored ? yyjson_mut_doc_imut_copy(stored, NULL) : NULL;
    Val *saved = old ? yyjson_doc_get_root(old) : NULL;
    if (truth(field(saved, "socket")) && !focus_error) {
      sock = string(field(saved, "socket"));
      response = herdr_call(sock, "session.snapshot", NULL);
      Val *snapshot = snapshot_from(response);
      if (target_alive(snapshot, saved))
        target = yyjson_doc_mut_copy(old, NULL);
      else
        target = resolve_target(snapshot, target_toast);
      if (response) {
        yyjson_doc_free(response);
        response = NULL;
      }
    }
    if (old)
      yyjson_doc_free(old);
    if (stored)
      yyjson_mut_doc_free(stored);
    if (!target) {
      for (size_t i = 0; i < sockets->len && !focus_error; i++) {
        response = herdr_call(sockets->pdata[i], "session.snapshot", NULL);
        target = resolve_target(snapshot_from(response), target_toast);
        if (response) {
          yyjson_doc_free(response);
          response = NULL;
        }
        if (target) {
          g_free(sock);
          sock = g_strdup(sockets->pdata[i]);
          break;
        }
      }
    }
  }
  if (sock && target && !focus_error) {
    target_raw = yyjson_mut_doc_imut_copy(target, NULL);
    Doc *payload = focus_payload(yyjson_doc_get_root(target_raw));
    if (payload) {
      yyjson_doc *raw = yyjson_mut_doc_imut_copy(payload, NULL);
      Val *root = yyjson_doc_get_root(raw);
      char *method = string(field(root, "method"));
      response = herdr_call(sock, method, field(root, "params"));
      g_free(method);
      if (response) {
        yyjson_doc_free(response);
        response = NULL;
      }
      yyjson_doc_free(raw);
      yyjson_mut_doc_free(payload);
    }
  }
  if (!focus_error) {
    GPtrArray *ancestors = g_ptr_array_new_with_free_func(g_free);
    if (sock) {
      GPtrArray *holders = attached_pids(sock, "/proc");
      for (size_t i = 0; i < holders->len; i++) {
        GPtrArray *chain = ancestor_pids(holders->pdata[i], "/proc");
        for (size_t j = 0; j < chain->len; j++)
          g_ptr_array_add(ancestors, g_strdup(chain->pdata[j]));
        g_ptr_array_free(chain, true);
      }
      g_ptr_array_free(holders, true);
    }
    char *label = target_toast ? string(field(target_toast, "workspace_label"))
                               : g_strdup("");
    Val *window = choose_window(clients ? yyjson_doc_get_root(clients) : NULL,
                                pid ? pid : "0", ancestors, label);
    if (window && truth(field(window, "address")) && !focus_error)
      focus_address(field(window, "address"));
    g_free(label);
    g_ptr_array_free(ancestors, true);
  }
done:
  g_free(pid);
  if (clients)
    yyjson_doc_free(clients);
  if (response)
    yyjson_doc_free(response);
  if (target_raw)
    yyjson_doc_free(target_raw);
  if (target)
    yyjson_mut_doc_free(target);
  if (parsed)
    yyjson_doc_free(parsed);
  if (toast)
    yyjson_mut_doc_free(toast);
  g_free(sock);
  g_free(cache);
  g_ptr_array_free(sockets, true);
  if (focus_error) {
    fputs("Notification focus failed; check the desktop and Herdr session.\n",
          stderr);
    return 1;
  }
  return 0;
}
