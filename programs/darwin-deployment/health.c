#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "tools.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *match(const char *pattern, Result *r) {
  GRegex *re = g_regex_new(pattern, G_REGEX_MULTILINE, 0, NULL);
  GMatchInfo *info = NULL;
  g_regex_match_full(re, r->out->str, (gssize)r->out->len, 0, 0, &info, NULL);
  char *out = g_match_info_matches(info) ? g_match_info_fetch(info, 1) : NULL;
  g_match_info_free(info);
  g_regex_unref(re);
  return out;
}
static Mut *digits(Doc *d, const char *text) {
  bool negative = *text == '-';
  const char *p = text + negative;
  GString *s = g_string_new(NULL);
  for (; *p; p = g_utf8_next_char(p)) {
    int n = g_unichar_digit_value(g_utf8_get_char(p));
    if (n < 0 || n > 9) {
      g_string_free(s, TRUE);
      return yyjson_mut_null(d);
    }
    g_string_append_c(s, (char)('0' + n));
  }
  while (s->len > 1 && s->str[0] == '0')
    g_string_erase(s, 0, 1);
  if (negative && strcmp(s->str, "0"))
    g_string_prepend_c(s, '-');
  Mut *v = yyjson_mut_rawcpy(d, s->str);
  g_string_free(s, TRUE);
  return v;
}
Doc *health_snapshot(Val *config, Run run, void *context) {
  if (!yyjson_is_arr(field(config, "services")))
    return NULL;
  char **readiness = strings(field(config, "readiness"));
  if (!readiness || !readiness[0]) {
    g_strfreev(readiness);
    return NULL;
  }
  Doc *d = yyjson_mut_doc_new(NULL);
  Mut *root = yyjson_mut_obj(d), *services = yyjson_mut_obj(d),
      *memory = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, root);
  bool healthy = true;
  Val *service;
  size_t i, n;
  yyjson_arr_foreach(field(config, "services"), i, n, service) {
    const char *label = text(field(service, "label")),
               *kind = text(field(service, "kind"));
    if (!label || !kind) {
      g_strfreev(readiness);
      yyjson_mut_doc_free(d);
      return NULL;
    }
    char *name = g_strconcat("system/", label, NULL);
    const char *argv[] = {"/bin/launchctl", "print", name, NULL};
    Result r = run(context, argv, 5000, true);
    g_free(name);
    char *state = match(
             "(*UCP)^[\\s\\x1c-\\x1f]*state = ([a-z ]+)[\\s\\x1c-\\x1f]*$", &r),
         *exit = match("(*UCP)^[\\s\\x1c-\\x1f]*last exit code = (-?\\d+)", &r);
    if (state)
      g_strstrip(state);
    else
      state = g_strdup("unknown");
    Mut *last = exit ? digits(d, exit) : yyjson_mut_null(d);
    char *last_text = yyjson_mut_val_write(last, 0, NULL);
    bool zero = last_text && !strcmp(last_text, "0");
    free(last_text);
    bool ok = r.code == 0 &&
              (!strcmp(kind, "socket") ||
               (!strcmp(kind, "running") ? !strcmp(state, "running")
                                         : strcmp(state, "running") && zero));
    Mut *record = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, record, "healthy", ok);
    yyjson_mut_obj_add_strcpy(d, record, "state", state);
    yyjson_mut_obj_add_val(d, record, "lastExit", last);
    yyjson_mut_obj_put(services, yyjson_mut_strcpy(d, label), record);
    g_free(state);
    g_free(exit);
    result_free(&r);
  }
  Mut *service_key, *service_value;
  yyjson_mut_obj_foreach(services, i, n, service_key, service_value) {
    healthy = healthy &&
              yyjson_mut_get_bool(yyjson_mut_obj_get(service_value, "healthy"));
  }
  Result ready = run(context, (const char *const *)readiness, 5000, true);
  g_strfreev(readiness);
  bool available = ready.code == 0;
  result_free(&ready);
  const char *vm_argv[] = {"/usr/bin/vm_stat", NULL};
  Result vm = run(context, vm_argv, 5000, true);
  GRegex *re = g_regex_new("(*UCP)^(Pages (?:free|active|wired down|occupied "
                           "by compressor)):[\\s\\x1c-\\x1f]*(\\d+)",
                           G_REGEX_MULTILINE, 0, NULL);
  GMatchInfo *info = NULL;
  g_regex_match_full(re, vm.out->str, (gssize)vm.out->len, 0, 0, &info, NULL);
  while (g_match_info_matches(info)) {
    char *key = g_match_info_fetch(info, 1),
         *count = g_match_info_fetch(info, 2);
    yyjson_mut_obj_put(memory, yyjson_mut_strcpy(d, key), digits(d, count));
    g_free(key);
    g_free(count);
    g_match_info_next(info, NULL);
  }
  g_match_info_free(info);
  g_regex_unref(re);
  const char *swap_argv[] = {"/usr/sbin/sysctl", "-n", "vm.swapusage", NULL};
  Result swap = run(context, swap_argv, 5000, true);
  char *used = match("(*UCP)used = ([\\d.]+[KMGT])", &swap);
  GDateTime *now = g_date_time_new_now_utc();
  char *time = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S.%f+00:00");
  g_date_time_unref(now);
  yyjson_mut_obj_add_strcpy(d, root, "time", time);
  yyjson_mut_obj_add_bool(d, root, "healthy", available && healthy);
  yyjson_mut_obj_add_bool(d, root, "ready", available);
  yyjson_mut_obj_add_val(d, root, "services", services);
  yyjson_mut_obj_add_val(d, root, "memoryPages", memory);
  yyjson_mut_obj_add_val(d, root, "swapUsed",
                         used ? yyjson_mut_strcpy(d, used)
                              : yyjson_mut_null(d));
  yyjson_mut_obj_add_bool(d, root, "metricsAvailable",
                          vm.code == 0 && swap.code == 0);
  g_free(time);
  g_free(used);
  result_free(&vm);
  result_free(&swap);
  return d;
}
int rotate_log(const char *path, size_t limit) {
  int fd = open(path, O_RDWR | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return errno == ENOENT ? 0 : -1;
  struct stat st;
  int result = -1;
  char *tail = NULL, *previous = NULL, *older = NULL;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    goto done;
  if (st.st_size < 0 || (guint64)st.st_size <= limit) {
    result = 0;
    goto done;
  }
  tail = g_malloc(limit ? limit : 1);
  ssize_t n = pread(fd, tail, limit, st.st_size - (off_t)limit);
  if (n < 0)
    goto done;
  previous = g_strconcat(path, ".1", NULL);
  older = g_strconcat(path, ".2", NULL);
  if (exists(previous) && rename(previous, older) != 0)
    goto done;
  if (!atomic_file(previous, tail, (size_t)n, ".log."))
    goto done;
  if (ftruncate(fd, 0) != 0)
    goto done;
  result = 1;
done:
  close(fd);
  g_free(tail);
  g_free(previous);
  g_free(older);
  return result;
}
bool health_record(Val *config, Doc *snapshot) {
  Val *limit = field(config, "logLimitBytes");
  const char *path = text(field(config, "statusFile"));
  char **logs = strings(field(config, "logs"));
  if (!logs || !path || !yyjson_is_uint(limit) ||
      yyjson_get_uint(limit) > G_MAXSSIZE) {
    g_strfreev(logs);
    return false;
  }
  size_t rotated = 0;
  bool ok = true;
  for (size_t i = 0; logs[i]; i++) {
    int count = rotate_log(logs[i], (size_t)yyjson_get_uint(limit));
    if (count < 0) {
      ok = false;
      break;
    }
    rotated += (size_t)count;
  }
  g_strfreev(logs);
  if (!ok)
    return false;
  yyjson_mut_obj_add_uint(snapshot, yyjson_mut_doc_get_root(snapshot),
                          "rotated", rotated);
  char *s = json(snapshot, false);
  ok = s && atomic_file(path, s, strlen(s), ".health.");
  free(s);
  return ok;
}
