#define _DEFAULT_SOURCE
#include "token-report.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef PROBE_PYTHON
#define PROBE_PYTHON "python3"
#endif

typedef struct {
  GMainLoop *loop;
  GSubprocess *process;
  GCancellable *cancel;
  char *output;
  bool ok, timed_out;
} Capture;
static void communicated(GObject *object, GAsyncResult *result, gpointer data) {
  Capture *c = data;
  c->ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(object), result,
                                               &c->output, NULL, NULL);
  g_main_loop_quit(c->loop);
}
static gboolean expired(gpointer data) {
  Capture *c = data;
  c->timed_out = true;
  g_subprocess_force_exit(c->process);
  g_cancellable_cancel(c->cancel);
  return G_SOURCE_REMOVE;
}
static char *execute(GSubprocessLauncher *launcher, const char *const *args,
                     unsigned seconds, int *code) {
  GSubprocess *child = g_subprocess_launcher_spawnv(launcher, args, NULL);
  if (!child)
    return NULL;
  Capture c = {g_main_loop_new(NULL, FALSE),
               child,
               g_cancellable_new(),
               NULL,
               false,
               false};
  guint timer = g_timeout_add_seconds(seconds, expired, &c);
  g_subprocess_communicate_utf8_async(child, NULL, c.cancel, communicated, &c);
  g_main_loop_run(c.loop);
  if (!c.timed_out)
    g_source_remove(timer);
  else
    g_subprocess_wait(child, NULL, NULL);
  *code =
      g_subprocess_get_if_exited(child) ? g_subprocess_get_exit_status(child)
      : g_subprocess_get_if_signaled(child) ? -g_subprocess_get_term_sig(child)
                                            : 1;
  g_object_unref(c.cancel);
  g_main_loop_unref(c.loop);
  g_object_unref(child);
  if (!c.ok || c.timed_out) {
    g_free(c.output);
    return NULL;
  }
  char *normal = replace_all(c.output ? c.output : "", "\r\n", "\n"),
       *out = replace_all(normal, "\r", "\n");
  g_free(normal);
  g_free(c.output);
  return out;
}
static char *pi_binary(void) {
  char *wrapper = token_home(".local/bin/pi");
  GString *contents = read_text(wrapper);
  g_free(wrapper);
  if (!contents)
    return NULL;
  GRegex *regex = g_regex_new("/nix/store/[^ \\n]+/bin/pi", 0, 0, NULL);
  GMatchInfo *matches = NULL;
  g_regex_match(regex, contents->str, 0, &matches);
  char *pi = NULL;
  while (g_match_info_matches(matches)) {
    g_free(pi);
    pi = g_match_info_fetch(matches, 0);
    g_match_info_next(matches, NULL);
  }
  g_match_info_free(matches);
  g_regex_unref(regex);
  g_string_free(contents, TRUE);
  return pi;
}
char *token_capture(Meta *meta) {
  char *pi = pi_binary(), *out = token_state("reports"),
       *capture = token_home(".local/bin/prompt-capture"), *task = NULL,
       *session = NULL, *runner = NULL, *script = NULL, *stdout_ = NULL,
       *log = NULL, *probe = NULL;
  char *error =
      g_strdup("Nested capture failed; inspect the private capture log.");
  if (!pi) {
    g_free(error);
    error = g_strdup("Could not find the store Pi binary in ~/.local/bin/pi.");
    goto done;
  }
  if (g_mkdir_with_parents(out, 0777) || chmod(out, 0700))
    goto done;
  task = g_build_filename(out, "pi-mitm-task-XXXXXX", NULL);
  session = g_build_filename(out, "pi-mitm-session-XXXXXX", NULL);
  if (!g_mkdtemp(task) || !g_mkdtemp(session))
    goto done;
  runner = g_build_filename(task, "run-turns.sh", NULL);
  char *quoted_task = g_shell_quote(task), *quoted_pi = g_shell_quote(pi),
       *quoted_session = g_shell_quote(session);
  GString *source = g_string_new("#!/usr/bin/env bash\nset -euo pipefail\n");
  g_string_append_printf(
      source,
      "cd %s\nunset PI_SESSION_FILE PI_SESSION_ID CAPTURE_PROMPTS\nexport "
      "PI_TELEMETRY=0\nexport PI_PROVIDER=xai\nexport "
      "PI_MODEL=grok-4.6\nPI=%s\nSESSION=%s\nCOMMON=(--print --provider xai "
      "--model grok-4.6 --thinking low --session-dir \"$SESSION\" --name "
      "mitm-token-probe)\n",
      quoted_task, quoted_pi, quoted_session);
  g_free(quoted_task);
  g_free(quoted_pi);
  g_free(quoted_session);
  for (size_t i = 0; i < 3; i++) {
    char *prompt = g_shell_quote(token_turns[i]);
    g_string_append_printf(source, "\"$PI\" \"${COMMON[@]}\" %s%s\n",
                           i ? "--continue " : "", prompt);
    g_free(prompt);
  }
  script = g_string_free(source, FALSE);
  if (!token_private_file(runner, script, strlen(script), 0700))
    goto done;
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE |
      G_SUBPROCESS_FLAGS_STDIN_INHERIT);
  const char *unset[] = {"PI_SESSION_FILE",    "PI_SESSION_ID",
                         "CAPTURE_PROMPTS",    "HTTP_PROXY",
                         "HTTPS_PROXY",        "http_proxy",
                         "https_proxy",        "SSL_CERT_FILE",
                         "REQUESTS_CA_BUNDLE", "NODE_EXTRA_CA_CERTS"};
  for (size_t i = 0; i < G_N_ELEMENTS(unset); i++)
    g_subprocess_launcher_unsetenv(launcher, unset[i]);
  g_subprocess_launcher_setenv(launcher, "PI_TELEMETRY", "0", TRUE);
  const char *args[] = {capture, "pi", "--", runner, NULL};
  stdout_ = execute(launcher, args, 900, &meta->exit_code);
  g_object_unref(launcher);
  if (!stdout_)
    goto done;
  log = g_build_filename(out, "nested-pi.log", NULL);
  if (!token_private_file(log, stdout_, strlen(stdout_), 0600))
    goto done;
  GRegex *regex = g_regex_new("run ([0-9T]+-[0-9]+)", 0, 0, NULL);
  GMatchInfo *match = NULL;
  g_regex_match(regex, stdout_, 0, &match);
  if (g_match_info_matches(match))
    meta->run = g_match_info_fetch(match, 1);
  g_match_info_free(match);
  g_regex_unref(regex);
  if (!meta->run)
    goto done;
  probe = g_build_filename(task, "add.py", NULL);
  bool present = g_file_test(probe, G_FILE_TEST_EXISTS);
  char *result = NULL;
  if (present) {
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_SILENCE |
                                         G_SUBPROCESS_FLAGS_STDIN_INHERIT);
    const char *check[] = {PROBE_PYTHON, probe, NULL};
    int code = 1;
    char *raw = execute(launcher, check, 10, &code);
    g_object_unref(launcher);
    if (raw) {
      char *trim = strip(raw);
      result = g_strdup(code                 ? "error"
                        : !strcmp(trim, "5") ? "5"
                        : !*trim             ? "n/a"
                                             : "unexpected output");
      g_free(trim);
      g_free(raw);
    } else
      result = g_strdup("error");
  }
  meta->task_result = g_strdup_printf(
      "add.py=%s output=%s", present ? "yes" : "no", result ? result : "n/a");
  g_free(result);
  g_clear_pointer(&error, g_free);
done:
  g_free(pi);
  g_free(out);
  g_free(capture);
  g_free(task);
  g_free(session);
  g_free(runner);
  g_free(script);
  g_free(stdout_);
  g_free(log);
  g_free(probe);
  return error;
}
