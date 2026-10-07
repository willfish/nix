#define _DEFAULT_SOURCE
#include "agenda.h"
#include <sys/resource.h>
#include <unistd.h>
static void restrict_child(void *unused) {
  (void)unused;
  struct rlimit zero = {0, 0}, cpu = {30, 30},
                memory = {1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024},
                file = {MAX_BYTES, MAX_BYTES};
  if (setrlimit(RLIMIT_CORE, &zero) < 0 || setrlimit(RLIMIT_CPU, &cpu) < 0 ||
      setrlimit(RLIMIT_AS, &memory) < 0 || setrlimit(RLIMIT_FSIZE, &file) < 0)
    _exit(126);
#ifdef RLIMIT_DATA
  setrlimit(RLIMIT_DATA, &memory);
#endif
}
typedef struct {
  GMainLoop *loop;
  GCancellable *cancel;
  GSubprocess *child;
  GBytes *output;
  bool ok, timeout;
} Parse;
static void completed(GObject *source, GAsyncResult *result, void *data) {
  Parse *p = data;
  GError *error = NULL;
  p->ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result,
                                          &p->output, NULL, &error);
  g_clear_error(&error);
  g_main_loop_quit(p->loop);
}
static gboolean expired(void *data) {
  Parse *p = data;
  p->timeout = true;
  g_subprocess_force_exit(p->child);
  g_cancellable_cancel(p->cancel);
  return G_SOURCE_REMOVE;
}
yyjson_mut_doc *expand(GBytes *raw, const char *program, GDateTime *start,
                       GDateTime *end, const char *zone_name, bool range) {
  char *first = iso_day(start), *last = iso_day(end);
  const char *args[6] = {program,
                         range ? "--parse-range" : "--parse-feed",
                         first,
                         range ? last : zone_name,
                         range ? zone_name : NULL,
                         NULL};
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
      G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  const char *path = g_getenv("PATH");
  char *env[] = {g_strconcat("PATH=", path ? path : "", NULL),
                 g_strconcat("TZ=", zone_name, NULL), g_strdup("LANG=C.UTF-8"),
                 g_strdup("LC_ALL=C.UTF-8"), NULL};
  g_subprocess_launcher_set_environ(launcher, env);
  g_subprocess_launcher_set_child_setup(launcher, restrict_child, NULL, NULL);
  GError *error = NULL;
  GSubprocess *child = g_subprocess_launcher_spawnv(launcher, args, &error);
  g_clear_error(&error);
  g_object_unref(launcher);
  for (size_t i = 0; env[i]; i++)
    g_free(env[i]);
  g_free(first);
  g_free(last);
  if (!child) {
    bad("invalid calendar");
    return NULL;
  }
  Parse p = {.loop = g_main_loop_new(NULL, false),
             .cancel = g_cancellable_new(),
             .child = child};
  guint timer = g_timeout_add(PARSE_TIMEOUT * 1000, expired, &p);
  g_subprocess_communicate_async(child, raw, p.cancel, completed, &p);
  g_main_loop_run(p.loop);
  if (!p.timeout)
    g_source_remove(timer);
  g_main_loop_unref(p.loop);
  g_object_unref(p.cancel);
  bool ok = p.ok && !p.timeout && g_subprocess_get_if_exited(child) &&
            g_subprocess_get_exit_status(child) == 0 && p.output &&
            g_bytes_get_size(p.output) <= MAX_BYTES;
  if (p.timeout)
    g_subprocess_wait(child, NULL, NULL);
  g_object_unref(child);
  yyjson_mut_doc *out = NULL;
  if (ok) {
    gsize len;
    const char *s = g_bytes_get_data(p.output, &len);
    yyjson_doc *doc = yyjson_read(s, len, YYJSON_READ_ALLOW_INF_AND_NAN);
    if (doc) {
      yyjson_val *root = yyjson_doc_get_root(doc);
      ok = yyjson_is_arr(root) &&
           yyjson_arr_size(root) <= (range ? BAR_MAX_EVENTS : MAX_OCCURRENCES);
      if (ok)
        out = yyjson_doc_mut_copy(doc, NULL);
      yyjson_doc_free(doc);
    }
  }
  if (p.output)
    g_bytes_unref(p.output);
  if (!out)
    bad(p.timeout ? "calendar parse timed out" : "invalid calendar");
  return out;
}
