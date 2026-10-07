#define _DEFAULT_SOURCE
#include <errno.h>
#include <gio/gio.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void on_interrupt(int signum) {
  (void)signum;
  interrupted = 1;
}
static int failure(void) {
  fputs("Daily action could not start. Check installed commands and user "
        "services.\n",
        stderr);
  return 1;
}

/* Path.home() normalizes separators and '.', but preserves '..' and '//'. */
static char *normal_path(const char *value) {
  bool rooted = value[0] == '/',
       two = rooted && value[1] == '/' && value[2] != '/';
  GString *out = g_string_new(rooted ? (two ? "//" : "/") : "");
  char **parts = g_strsplit(value, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    if (out->len && out->str[out->len - 1] != '/')
      g_string_append_c(out, '/');
    g_string_append(out, parts[i]);
  }
  if (!out->len)
    g_string_append_c(out, '.');
  g_strfreev(parts);
  return g_string_free(out, false);
}
static char *home_path(void) {
  const char *home = g_getenv("HOME");
  if (home)
    return normal_path(*home ? home : "/");
  struct passwd *pw = getpwuid(getuid());
  return pw ? normal_path(pw->pw_dir) : NULL;
}
static void detached(void *data) {
  (void)data;
  if (setsid() < 0)
    _exit(127);
}
static bool launch(const char *action) {
  char *home = home_path();
  if (!home)
    return false;
  char *working = g_strconcat("--working-directory=", home, NULL);
  const char *argv[20] = {"systemd-run", "--user", "--scope", "--collect",
                          "--quiet",     "--",     "ghostty"};
  size_t n = 7;
  if (!strcmp(action, "agenda") || !strcmp(action, "notes")) {
    bool agenda = !strcmp(action, "agenda");
    argv[n++] = agenda ? "--title=Today" : "--title=Today's notes";
    argv[n++] = agenda ? "--window-width=84" : "--window-width=100";
    argv[n++] = agenda ? "--window-height=24" : "--window-height=36";
  }
  argv[n++] = working;
  argv[n++] = "-e";
  if (!strcmp(action, "workspace") || !strcmp(action, "notes")) {
    argv[n++] = "fish";
    argv[n++] = "-ic";
    argv[n++] = !strcmp(action, "workspace") ? "mux start dot" : "today";
  } else {
    argv[n++] = "daily-workflow";
    argv[n++] = !strcmp(action, "agenda") ? "_agenda" : "_cleanup";
  }
  argv[n] = NULL;
  char **env = g_get_environ();
  /* Work from a separate snapshot because removal can move the vector. */
  char **snapshot = g_strdupv(env);
  for (size_t i = 0; snapshot[i]; i++)
    if (g_str_has_prefix(snapshot[i], "HERDR_") && strchr(snapshot[i], '=')) {
      char *key = g_strndup(snapshot[i],
                            (size_t)(strchr(snapshot[i], '=') - snapshot[i]));
      env = g_environ_unsetenv(env, key);
      g_free(key);
    }
  g_strfreev(snapshot);
  env = g_environ_setenv(env, "MUX_BACKEND", "herdr", true);
  env = g_environ_unsetenv(env, "MUX_HERDR_ATTACH");
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_environ(launcher, env);
  g_subprocess_launcher_set_child_setup(launcher, detached, NULL, NULL);
  GError *error = NULL;
  GSubprocess *child = g_subprocess_launcher_spawnv(launcher, argv, &error);
  g_clear_error(&error);
  bool ok = child != NULL;
  if (child)
    g_object_unref(child);
  g_object_unref(launcher);
  g_strfreev(env);
  g_free(home);
  g_free(working);
  return ok;
}

typedef struct {
  GMainLoop *loop;
  GSubprocess *child;
  bool ok;
  gint64 interrupt_at;
} Wait;
static void waited(GObject *source, GAsyncResult *result, void *data) {
  Wait *wait = data;
  GError *error = NULL;
  wait->ok = g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
  g_clear_error(&error);
  g_main_loop_quit(wait->loop);
}
static gboolean interrupt_child(void *data) {
  Wait *wait = data;
  if (interrupted) {
    gint64 now = g_get_monotonic_time();
    if (!wait->interrupt_at)
      wait->interrupt_at = now;
    if (now - wait->interrupt_at >= 250000)
      g_subprocess_force_exit(wait->child);
  }
  return G_SOURCE_CONTINUE;
}
static bool run_child(const char *const *argv, int *status) {
  GError *error = NULL;
  GSubprocess *child =
      g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDIN_INHERIT, &error);
  g_clear_error(&error);
  if (!child)
    return false;
  Wait wait = {.loop = g_main_loop_new(NULL, false), .child = child};
  guint timer = g_timeout_add(25, interrupt_child, &wait);
  g_subprocess_wait_async(child, NULL, waited, &wait);
  g_main_loop_run(wait.loop);
  g_source_remove(timer);
  g_main_loop_unref(wait.loop);
  if (wait.ok)
    *status = g_subprocess_get_if_signaled(child)
                  ? -g_subprocess_get_term_sig(child)
                  : g_subprocess_get_exit_status(child);
  g_object_unref(child);
  return wait.ok;
}

/* Linux input() removes LF only. Never trim confirmation or ignore NUL. */
static bool valid_text(const GString *line) {
  const char *p = line->str;
  size_t left = line->len;
  while (left) {
    size_t len = strnlen(p, left);
    if (!g_utf8_validate(p, (gssize)len, NULL))
      return false;
    size_t used = len + (len < left);
    p += used;
    left -= used;
  }
  return true;
}
static GString *input_line(const char *prompt, bool *failed) {
  *failed = false;
  clearerr(stdin);
  fputs(prompt, stdout);
  fflush(stdout);
  GString *line = g_string_new(NULL);
  for (;;) {
    errno = 0;
    int c = fgetc(stdin);
    if (interrupted || (c == EOF && errno == EINTR)) {
      interrupted = 0;
      clearerr(stdin);
      g_string_free(line, true);
      return NULL;
    }
    if (c == EOF) {
      *failed = ferror(stdin) != 0;
      if (*failed || !line->len) {
        g_string_free(line, true);
        return NULL;
      }
      break;
    }
    if (c == '\n')
      break;
    g_string_append_c(line, (char)c);
  }
  if (!valid_text(line)) {
    *failed = true;
    g_string_free(line, true);
    return NULL;
  }
  return line;
}
static bool cleanup(int *status) {
  puts("Delete old Nix generations and collect garbage for your user AND the "
       "system.");
  puts("This removes rollback options. Sudo may request your password.");
  bool failed;
  GString *answer =
      input_line("Type DELETE to run gcall, or Enter to cancel: ", &failed);
  if (failed)
    return false;
  if (!answer) {
    *status = 0;
    return true;
  }
  bool approved = answer->len == 6 && !memcmp(answer->str, "DELETE", 6);
  g_string_free(answer, true);
  if (!approved) {
    puts("Cancelled. Nothing deleted.");
    *status = 0;
    return true;
  }
  char *home = home_path();
  if (!home)
    return false;
  char *joined = g_build_filename(home, ".bin", "gcall", NULL),
       *program = normal_path(joined);
  const char *argv[] = {program, NULL};
  bool ok = run_child(argv, status);
  g_free(home);
  g_free(joined);
  g_free(program);
  return ok;
}
static bool close_terminal(bool agenda) {
  if (!agenda || !isatty(STDIN_FILENO)) {
    bool failed;
    GString *line = input_line("\nPress Enter to close.", &failed);
    if (line)
      g_string_free(line, true);
    return !failed;
  }
  struct termios previous, raw;
  if (tcgetattr(STDIN_FILENO, &previous) < 0)
    return false;
  raw = previous;
  raw.c_iflag &= ~(IGNBRK | BRKINT | IGNPAR | PARMRK | INPCK | ISTRIP | INLCR |
                   IGNCR | ICRNL | IXON | IXANY | IXOFF);
  raw.c_oflag &= ~OPOST;
  raw.c_cflag &= ~(PARENB | CSIZE);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL | ICANON | IEXTEN | ISIG |
                   NOFLSH | TOSTOP);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
    return false;
  char key;
  ssize_t count = read(STDIN_FILENO, &key, 1);
  int saved_error = errno;
  bool restored = tcsetattr(STDIN_FILENO, TCSADRAIN, &previous) == 0;
  if (interrupted) {
    interrupted = 0;
    return restored;
  }
  return restored && (count >= 0 || saved_error == EINTR);
}
int main(int argc, char **argv) {
  if (argc != 2 ||
      (strcmp(argv[1], "workspace") && strcmp(argv[1], "notes") &&
       strcmp(argv[1], "agenda") && strcmp(argv[1], "cleanup") &&
       strcmp(argv[1], "_agenda") && strcmp(argv[1], "_cleanup"))) {
    fputs("Usage: daily-workflow workspace|notes|agenda|cleanup\n", stderr);
    return 64;
  }
  struct sigaction old, handler = {.sa_handler = on_interrupt};
  sigemptyset(&handler.sa_mask);
  if (sigaction(SIGINT, NULL, &old) < 0 ||
      (old.sa_handler != SIG_IGN && sigaction(SIGINT, &handler, NULL) < 0))
    return failure();
  if (!strcmp(argv[1], "_agenda") || !strcmp(argv[1], "_cleanup")) {
    bool agenda = !strcmp(argv[1], "_agenda");
    int status = 0;
    const char *command[] = {"daily-agenda", NULL};
    bool ok = agenda ? run_child(command, &status) : cleanup(&status);
    if (interrupted) {
      struct sigaction reset = {.sa_handler = SIG_DFL};
      sigemptyset(&reset.sa_mask);
      sigaction(SIGINT, &reset, NULL);
      raise(SIGINT);
      return 130;
    }
    if (!ok || !close_terminal(agenda))
      return failure();
    return status;
  }
  return launch(argv[1]) ? 0 : failure();
}
