#include "usage.h"
#include <ctype.h>
#include <dirent.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

Allowance parse_antigravity(Doc *d, Val *payload) {
  Val *user = get(payload, "userStatus");
  Allowance a = {.limits = yyjson_mut_arr(d), .status = "",
                 .tier = text(get(get(get(user, "planStatus"), "planInfo"), "planName")),
                 .note = "App-reported model quotas; not token totals or credit spend"};
  Val *models = get(get(user, "cascadeModelConfigData"), "clientModelConfigs");
  size_t i, n;
  Val *model;
  yyjson_mut_arr_foreach(models, i, n, model) {
    Val *quota = get(model, "quotaInfo"), *left = get(quota, "remainingFraction");
    // An absent balance is unknown, never an exhausted or unused allowance.
    if (!yyjson_mut_is_num(left))
      continue;
    double remaining = yyjson_mut_get_num(left);
    if (!isfinite(remaining) || remaining < 0 || remaining > 1)
      continue;
    const char *name = text(get(model, "label"));
    if (!*name)
      name = text(get(get(model, "modelOrAlias"), "model"));
    if (!*name)
      continue;
    Val *v = yyjson_mut_obj(d);
    strput(d, v, "label", name);
    strput(d, v, "title", name);
    put(d, v, "percent", yyjson_mut_real(d, 1 - remaining));
    strput(d, v, "resetsAt", iso_timestamp(d, get(quota, "resetTime")));
    yyjson_mut_arr_append(a.limits, v);
  }
  return a;
}

static char *flag(char *args, size_t length, const char *name) {
  size_t n = strlen(name);
  for (size_t i = 0; i < length;) {
    char *arg = args + i;
    size_t size = strlen(arg);
    i += size + 1;
    if (!strcmp(arg, name) && i < length)
      return args + i;
    if (size > n && !strncmp(arg, name, n) && arg[n] == '=')
      return arg + n + 1;
  }
  return "";
}

static bool process(const char *root, char **args, size_t *length) {
  struct stat st;
  if (stat(root, &st) || st.st_uid != getuid())
    return false;
  char *path = join(root, "exe"), exe[PATH_MAX];
  ssize_t n = readlink(path, exe, sizeof exe - 1);
  free(path);
  if (n < 0)
    return false;
  exe[n] = 0;
  const char *base = strrchr(exe, '/');
  if (!base || !strstr(base, "language_server") || !strstr(exe, "/antigravity/"))
    return false;
  path = join(root, "cmdline");
  gsize size = 0;
  bool ok = g_file_get_contents(path, args, &size, NULL);
  free(path);
  *length = size;
  return ok && size > 0;
}

static size_t ports(const char *root, unsigned out[16]) {
  unsigned long inodes[64];
  size_t count = 0, found = 0;
  char *path = join(root, "fd");
  DIR *dir = opendir(path);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < 64) {
      char *fd = join(path, entry->d_name), target[80];
      ssize_t n = readlink(fd, target, sizeof target - 1);
      free(fd);
      if (n < 0)
        continue;
      target[n] = 0;
      unsigned long inode;
      if (sscanf(target, "socket:[%lu]", &inode) == 1)
        inodes[count++] = inode;
    }
    closedir(dir);
  }
  free(path);
  const char *tables[] = {"net/tcp", "net/tcp6"};
  for (size_t t = 0; t < 2 && found < 16; t++) {
    path = join(root, tables[t]);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f)
      continue;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, f) >= 0 && found < 16) {
      char address[65];
      unsigned port, state;
      unsigned long inode;
      if (sscanf(line, " %*u: %64[0-9A-Fa-f]:%x %*s %x %*s %*s %*s %*u %*u %lu",
                 address, &port, &state, &inode) != 4 || state != 10 || !port || port > 65535)
        continue;
      bool owned = false, seen = false;
      for (size_t i = 0; i < count; i++)
        if (inodes[i] == inode)
          owned = true;
      for (size_t i = 0; i < found; i++)
        if (out[i] == port)
          seen = true;
      if (owned && !seen)
        out[found++] = port;
    }
    free(line);
    fclose(f);
  }
  return found;
}

Val *collect_antigravity_at(Doc *d, Context *ctx, const char *proc_root) {
  Allowance a = {.limits = yyjson_mut_arr(d), .tier = "",
                 .status = "Limits unavailable"};
  const char *help = "Open Antigravity and sign in with your Google AI account to read quotas.";
  bool fetched = false;
  DIR *dir = opendir(proc_root);
  if (dir) {
    struct dirent *entry;
    while (!fetched && (entry = readdir(dir))) {
      if (!*entry->d_name || strspn(entry->d_name, "0123456789") != strlen(entry->d_name))
        continue;
      char *root = join(proc_root, entry->d_name), *args = NULL;
      size_t length = 0;
      if (!process(root, &args, &length)) {
        free(root);
        g_free(args);
        continue;
      }
      const char *csrf = flag(args, length, "--csrf_token");
      if (!*csrf) {
        free(root);
        g_free(args);
        continue;
      }
      help = "Antigravity's local quota service is unavailable; open a project or try again.";
      unsigned listeners[16];
      size_t count = ports(root, listeners);
      free(root);
      for (size_t i = 0; i < count && !fetched; i++) {
        // HTTPS is the Connect service; older IDE builds also expose HTTP.
        for (size_t scheme = 0; scheme < 2 && !fetched; scheme++) {
          char url[160];
          snprintf(url, sizeof url, "%s://127.0.0.1:%u/exa.language_server_pb.LanguageServerService/GetUserStatus",
                   scheme ? "http" : "https", listeners[i]);
          const char *body = "{\"metadata\":{\"ideName\":\"antigravity\",\"extensionName\":\"antigravity\",\"locale\":\"en\"}}";
          size_t size = size_add(strlen(csrf), 80);
          char *headers = allocate(size);
          snprintf(headers, size, "Connect-Protocol-Version: 1\nX-Codeium-Csrf-Token: %s", csrf);
          Transport send = ctx->local_transport ? ctx->local_transport : antigravity_request;
          Reply reply = send(d, url, NULL, headers, body);
          free(headers);
          if (reply.failed || reply.status < 200 || reply.status >= 300)
            continue;
          Allowance parsed = parse_antigravity(d, reply.body);
          if (*parsed.tier)
            a.tier = parsed.tier;
          if (yyjson_mut_arr_size(parsed.limits)) {
            a = parsed;
            fetched = true;
            help = "";
          } else
            help = "Antigravity returned no measured quotas; check its sign-in and Settings page.";
        }
      }
      g_free(args);
    }
    closedir(dir);
  }
  double now = context_now(ctx);
  if (!fetched)
    a.note = help;
  Val *out = record(d, "antigravity", "Antigravity", empty_stats(d, now), a, help, false, now);
  put(d, out, "hasLocalStats", yyjson_mut_bool(d, false));
  put(d, out, "hasPromptStats", yyjson_mut_bool(d, false));
  put(d, out, "showWhenUnavailable", yyjson_mut_bool(d, true));
  if (fetched)
    intput(d, out, "limitsFetchedAt", (int64_t)(now * 1000));
  return out;
}
