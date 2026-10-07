#include "watch.h"
#include <string.h>

static gboolean expired(gpointer raw) {
  Capture *c = raw;
  c->timer = 0;
  c->timed_out = true;
  g_subprocess_force_exit(c->process);
  g_cancellable_cancel(c->cancel);
  return G_SOURCE_REMOVE;
}
static void completed(GObject *object, GAsyncResult *result, gpointer raw) {
  Capture *c = raw;
  GError *error = NULL;
  bool communicated = g_subprocess_communicate_utf8_finish(
      G_SUBPROCESS(object), result, &c->output, NULL, &error);
  if (c->timer) {
    g_source_remove(c->timer);
    c->timer = 0;
  }
  g_clear_error(&error);
  // Cancellation must not leave a live direct child behind or require EOF
  // from a descendant that inherited its stdout pipe.
  g_subprocess_wait(c->process, NULL, NULL);
  c->ok =
      communicated && !c->timed_out && g_subprocess_get_successful(c->process);
  if (c->loop)
    g_main_loop_quit(c->loop);
  if (c->done)
    c->done(c, c->data);
}
static Capture *start(const char *const *argv, guint timeout_ms,
                      CaptureDone done, void *data, GSubprocessFlags flags) {
  GError *error = NULL;
  GSubprocess *process = g_subprocess_newv(argv, flags, &error);
  g_clear_error(&error);
  if (!process)
    return NULL;
  Capture *c = g_new0(Capture, 1);
  c->process = process;
  c->cancel = g_cancellable_new();
  c->done = done;
  c->data = data;
  c->timer = g_timeout_add(timeout_ms, expired, c);
  g_subprocess_communicate_utf8_async(process, NULL, c->cancel, completed, c);
  return c;
}
Capture *capture_start(const char *const *argv, guint timeout_ms,
                       CaptureDone done, void *data) {
  return start(argv, timeout_ms, done, data,
               G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                   G_SUBPROCESS_FLAGS_STDERR_SILENCE);
}
static Capture *wait_flags(const char *const *argv, guint timeout_ms,
                           GSubprocessFlags flags) {
  Capture *c = start(argv, timeout_ms, NULL, NULL, flags);
  if (!c)
    return NULL;
  c->loop = g_main_loop_new(NULL, false);
  g_main_loop_run(c->loop);
  g_main_loop_unref(c->loop);
  c->loop = NULL;
  return c;
}
Capture *capture_wait(const char *const *argv, guint timeout_ms) {
  return wait_flags(argv, timeout_ms,
                    G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                        G_SUBPROCESS_FLAGS_STDERR_SILENCE);
}
void capture_free(Capture *c) {
  if (!c)
    return;
  g_free(c->output);
  g_object_unref(c->cancel);
  g_object_unref(c->process);
  g_free(c);
}
static bool space(gunichar c) {
  return g_unichar_isspace(c) || c == '\v' || c == 0x85 ||
         (c >= 0x1c && c <= 0x1f);
}
bool open_chosen(const char *output, const char *url) {
  char *action = g_strdup(output ? output : "");
  // Python's strip includes Unicode whitespace.
  char *begin = action;
  while (*begin && space(g_utf8_get_char(begin)))
    begin = g_utf8_next_char(begin);
  char *end = action + strlen(action);
  while (end > begin) {
    char *prev = g_utf8_find_prev_char(begin, end);
    if (!prev || !space(g_utf8_get_char(prev)))
      break;
    end = prev;
  }
  *end = 0;
  bool chosen = !strcmp(begin, "open") && url && *url;
  g_free(action);
  if (chosen) {
    const char *argv[] = {"xdg-open", url, NULL};
    Capture *c = wait_flags(argv, 10000, G_SUBPROCESS_FLAGS_NONE);
    capture_free(c);
  }
  return chosen;
}
