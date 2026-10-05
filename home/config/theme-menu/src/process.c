#include "theme.h"
#include <string.h>
typedef struct {
  Command *c;
  GSubprocess *child;
  GCancellable *cancel;
  GMainLoop *loop;
  guint timer;
} Wait;
static gboolean expired(gpointer raw) {
  Wait *w = raw;
  w->timer = 0;
  w->c->timed_out = true;
  g_subprocess_force_exit(w->child);
  g_cancellable_cancel(w->cancel);
  return G_SOURCE_REMOVE;
}
static char *text(GBytes *bytes, bool *valid) {
  gsize len = 0;
  const char *s = bytes ? g_bytes_get_data(bytes, &len) : NULL;
  if (!s)
    return g_strdup("");
  if (!g_utf8_validate(s, (gssize)len, NULL)) {
    *valid = false;
    return g_strdup("");
  }
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\r') {
      g_string_append_c(out, '\n');
      if (i + 1 < len && s[i + 1] == '\n')
        i++;
    } else
      g_string_append_c(out, s[i]);
  }
  return g_string_free(out, false);
}
static void finished(GObject *object, GAsyncResult *result, gpointer raw) {
  Wait *w = raw;
  GBytes *out = NULL, *err = NULL;
  GError *e = NULL;
  bool communicated = g_subprocess_communicate_finish(G_SUBPROCESS(object),
                                                      result, &out, &err, &e);
  if (w->timer)
    g_source_remove(w->timer);
  w->timer = 0;
  g_subprocess_wait(w->child, NULL, NULL);
  bool valid = true;
  w->c->out = text(out, &valid);
  w->c->err = text(err, &valid);
  if (out)
    g_bytes_unref(out);
  if (err)
    g_bytes_unref(err);
  if (w->c->timed_out)
    w->c->failure = g_strdup("Command timed out");
  else if (!communicated)
    w->c->failure =
        g_strdup(e ? e->message : "Could not communicate with command");
  else if (!valid)
    w->c->failure = g_strdup("Invalid UTF-8 from command");
  if (g_subprocess_get_if_exited(w->child))
    w->c->status = g_subprocess_get_exit_status(w->child);
  else
    w->c->status = -g_subprocess_get_term_sig(w->child);
  w->c->ok =
      communicated && !w->c->timed_out && !w->c->failure && w->c->status == 0;
  g_clear_error(&e);
  g_main_loop_quit(w->loop);
}
Command *command(const char *const *argv, const char *input, guint timeout_ms,
                 bool inherit) {
  Command *c = g_new0(Command, 1);
  c->status = -1;
  GSubprocessFlags flags =
      inherit
          ? G_SUBPROCESS_FLAGS_NONE
          : (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  if (input)
    flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
  GError *e = NULL;
  GSubprocess *child = g_subprocess_newv(argv, flags, &e);
  if (!child) {
    c->failure = g_strdup(e->message);
    g_error_free(e);
    c->out = g_strdup("");
    c->err = g_strdup("");
    return c;
  }
  Wait w = {.c = c,
            .child = child,
            .cancel = g_cancellable_new(),
            .loop = g_main_loop_new(NULL, false)};
  if (timeout_ms)
    w.timer = g_timeout_add(timeout_ms, expired, &w);
  GBytes *bytes = input ? g_bytes_new(input, strlen(input)) : NULL;
  g_subprocess_communicate_async(child, bytes, w.cancel, finished, &w);
  if (bytes)
    g_bytes_unref(bytes);
  g_main_loop_run(w.loop);
  g_main_loop_unref(w.loop);
  g_object_unref(w.cancel);
  g_object_unref(child);
  return c;
}
void command_free(Command *c) {
  g_free(c->out);
  g_free(c->err);
  g_free(c->failure);
  g_free(c);
}
bool checked(Themes *t, Command *c) {
  if (c->ok)
    return true;
  return fail(t, "%s",
              c->failure ? c->failure
                         : "Command returned a non-zero exit status");
}
