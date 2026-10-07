#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "rows.h"

static const char *current_test;
static int failures;
static char *binary, *pty_bin, *fake_bin;
static const char *usage =
    "Usage: daily-workflow workspace|notes|agenda|cleanup\n";
static const char *error =
    "Daily action could not start. Check installed commands and user services.\n";
static const char *banner =
    "Delete old Nix generations and collect garbage for your user AND the system.\n"
    "This removes rollback options. Sudo may request your password.\n"
    "Type DELETE to run gcall, or Enter to cancel: ";
static const char *close_prompt = "\nPress Enter to close.";
static const char *prefix[] = {"--user", "--scope", "--collect", "--quiet", "--",
                               "ghostty"};

static void failf(const char *file, int line, const char *fmt, ...) {
  fprintf(stderr, "FAIL %s (%s:%d): ", current_test, file, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  failures++;
}
#define FAIL(...) failf(__FILE__, __LINE__, __VA_ARGS__)
#define EXPECT(cond)                                                           \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL("expected true: %s", #cond);                                        \
  } while (0)
static void begin(const char *name) {
  current_test = name;
  printf("RUN %s\n", name);
  fflush(stdout);
}
static char *sibling(const char *env, const char *name) {
  const char *v = env ? getenv(env) : NULL;
  if (v && *v)
    return g_strdup(v);
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n < 0)
    return NULL;
  exe[n] = 0;
  char *dir = g_path_get_dirname(exe);
  char *path = g_build_filename(dir, name, NULL);
  g_free(dir);
  return path;
}
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
typedef struct {
  int status;
  char *out, *err;
  size_t out_len, err_len;
  bool error;
  int signal;
} Proc;
static bool grow(char **buf, size_t *len, size_t *cap, const void *add, size_t n) {
  if (*len > SIZE_MAX - n - 1)
    return false;
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1) {
      if (next > SIZE_MAX / 2)
        return false;
      next *= 2;
    }
    char *p = realloc(*buf, next);
    if (!p)
      return false;
    *buf = p;
    *cap = next;
  }
  if (n)
    memcpy(*buf + *len, add, n);
  *len += n;
  (*buf)[*len] = 0;
  return true;
}
static Proc run_proc(const char *cmd, char *const *argv, const char *cwd,
                     const void *input, size_t input_len, const char *const *env_set,
                     const char *const *env_unset, int timeout_ms) {
  Proc p = {.status = -1};
  int outp[2] = {-1, -1}, errp[2] = {-1, -1}, inp[2] = {-1, -1};
  if (pipe(outp) || pipe(errp) || pipe(inp)) {
    p.error = true;
    return p;
  }
  pid_t pid = fork();
  if (pid < 0) {
    p.error = true;
    return p;
  }
  if (!pid) {
    dup2(inp[0], 0);
    dup2(outp[1], 1);
    dup2(errp[1], 2);
    close(inp[0]);
    close(inp[1]);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    if (cwd && chdir(cwd) < 0)
      _exit(127);
    if (env_unset)
      for (size_t i = 0; env_unset[i]; i++)
        unsetenv(env_unset[i]);
    if (env_set)
      for (size_t i = 0; env_set[i]; i++)
        putenv((char *)env_set[i]);
    execv(cmd, argv);
    _exit(127);
  }
  close(inp[0]);
  close(outp[1]);
  close(errp[1]);
  size_t in_off = 0, ocap = 0, ecap = 0;
  int64_t deadline = now_ms() + timeout_ms;
  while (outp[0] >= 0 || errp[0] >= 0 || inp[1] >= 0) {
    if (now_ms() > deadline) {
      kill(pid, SIGKILL);
      p.error = true;
      break;
    }
    struct pollfd fds[3];
    nfds_t nfd = 0;
    int ii = -1, oi = -1, ei = -1;
    if (inp[1] >= 0) {
      ii = (int)nfd;
      fds[nfd++] = (struct pollfd){inp[1], POLLOUT, 0};
    }
    if (outp[0] >= 0) {
      oi = (int)nfd;
      fds[nfd++] = (struct pollfd){outp[0], POLLIN, 0};
    }
    if (errp[0] >= 0) {
      ei = (int)nfd;
      fds[nfd++] = (struct pollfd){errp[0], POLLIN, 0};
    }
    poll(fds, nfd, 50);
    if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
      if (input && in_off < input_len) {
        ssize_t w = write(inp[1], (const char *)input + in_off, input_len - in_off);
        if (w > 0)
          in_off += (size_t)w;
        else
          in_off = input_len;
      } else {
        close(inp[1]);
        inp[1] = -1;
      }
    }
    char tmp[8192];
    if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t r = read(outp[0], tmp, sizeof tmp);
      if (r > 0) {
        if (!grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r))
          p.error = true;
      } else {
        close(outp[0]);
        outp[0] = -1;
      }
    }
    if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t r = read(errp[0], tmp, sizeof tmp);
      if (r > 0) {
        if (!grow(&p.err, &p.err_len, &ecap, tmp, (size_t)r))
          p.error = true;
      } else {
        close(errp[0]);
        errp[0] = -1;
      }
    }
  }
  if (inp[1] >= 0)
    close(inp[1]);
  if (outp[0] >= 0)
    close(outp[0]);
  if (errp[0] >= 0)
    close(errp[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    p.error = true;
  else if (WIFEXITED(status))
    p.status = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) {
    p.signal = WTERMSIG(status);
    p.status = 128 + p.signal;
  } else
    p.error = true;
  return p;
}
static void proc_clear(Proc *p) {
  free(p->out);
  free(p->err);
  memset(p, 0, sizeof *p);
}
static bool eq_mem(const char *got, size_t n, const char *expect) {
  size_t m = strlen(expect);
  return n == m && (n == 0 || (got && !memcmp(got, expect, m)));
}
typedef struct {
  char *root, *home, *bin, *log;
  char **env;
  char **programs;
  size_t nprograms;
} Sandbox;
static char *find_on_path(const char *name) {
  const char *path = g_getenv("PATH");
  if (!path)
    return NULL;
  char **parts = g_strsplit(path, ":", -1);
  char *found = NULL;
  for (size_t i = 0; parts[i]; i++) {
    char *candidate = g_build_filename(parts[i], name, NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
      found = candidate;
      break;
    }
    g_free(candidate);
  }
  g_strfreev(parts);
  return found;
}
static Sandbox *setup(void) {
  Sandbox *s = g_new0(Sandbox, 1);
  s->root = g_dir_make_tmp("daily-workflow-XXXXXX", NULL);
  s->home = g_build_filename(s->root, "home", NULL);
  s->bin = g_build_filename(s->root, "bin", NULL);
  s->log = g_build_filename(s->root, "log.jsonl", NULL);
  g_mkdir_with_parents(g_build_filename(s->home, ".bin", NULL), 0755);
  g_mkdir_with_parents(s->bin, 0755);
  if (!g_file_set_contents(s->log, "", 0, NULL))
    exit(1);
  const char *names[] = {"systemd-run", "daily-agenda"};
  for (size_t i = 0; i < 2; i++) {
    char *path = g_build_filename(s->bin, names[i], NULL);
    if (symlink(fake_bin, path) < 0)
      exit(1);
    g_free(path);
  }
  char *gcall = g_build_filename(s->home, ".bin", "gcall", NULL);
  if (symlink(fake_bin, gcall) < 0)
    exit(1);
  g_free(gcall);
  char *path = g_strdup_printf("PATH=%s", s->bin);
  char *home = g_strdup_printf("HOME=%s", s->home);
  char *log = g_strdup_printf("DW_LOG=%s", s->log);
  s->env = g_new0(char *, 16);
  s->env[0] = home;
  s->env[1] = path;
  s->env[2] = log;
  s->env[3] = g_strdup("DW_MARKER=test-marker");
  s->env[4] = g_strdup("HERDR_TEST=remove-me");
  s->env[5] = g_strdup("HERDR_ANOTHER=remove-me-too");
  s->env[6] = g_strdup("HERDR_=also-remove");
  s->env[7] = g_strdup("HERDR=keep-me");
  s->env[8] = g_strdup("NOT_HERDR_TEST=keep-other");
  s->env[9] = g_strdup("MUX_BACKEND=tmux");
  s->env[10] = g_strdup("MUX_HERDR_ATTACH=attach-value");
  s->programs = g_new0(char *, 3);
  s->programs[0] = g_strdup(binary);
  s->nprograms = 1;
  const char *legacy = getenv("DAILY_WORKFLOW_LEGACY");
  if (legacy) {
    char *python = getenv("DAILY_WORKFLOW_PYTHON") ? g_strdup(getenv("DAILY_WORKFLOW_PYTHON"))
                                                   : find_on_path("python3");
    char *bash = find_on_path("bash");
    EXPECT(python && bash);
    char *script = g_build_filename(s->root, "legacy", NULL);
    char *body = g_strdup_printf("#!%s\nexec %s %s \"$@\"\n", bash, python, legacy);
    g_file_set_contents(script, body, -1, NULL);
    chmod(script, 0755);
    s->programs[s->nprograms++] = script;
    g_free(body);
    g_free(python);
    g_free(bash);
  }
  return s;
}
static void sandbox_free(Sandbox *s) {
  char *argv[] = {"rm", "-rf", "--", s->root, NULL};
  pid_t pid = fork();
  if (!pid)
    execvp(argv[0], argv);
  if (pid > 0)
    waitpid(pid, NULL, 0);
  g_strfreev(s->env);
  g_strfreev(s->programs);
  g_free(s->root);
  g_free(s->home);
  g_free(s->bin);
  g_free(s->log);
  g_free(s);
}
static Proc run_s(Sandbox *s, const char *program, const char **args, const void *input,
                  size_t input_len, const char *const *extra_env) {
  GPtrArray *argv = g_ptr_array_new();
  g_ptr_array_add(argv, (gpointer)program);
  if (args)
    for (size_t i = 0; args[i]; i++)
      g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  GPtrArray *env = g_ptr_array_new();
  for (size_t i = 0; s->env[i]; i++)
    g_ptr_array_add(env, s->env[i]);
  if (extra_env)
    for (size_t i = 0; extra_env[i]; i++)
      g_ptr_array_add(env, (gpointer)extra_env[i]);
  g_ptr_array_add(env, NULL);
  Proc p = run_proc(program, (char *const *)argv->pdata, s->root, input, input_len,
                    (const char *const *)env->pdata, NULL, 8000);
  g_ptr_array_free(argv, true);
  g_ptr_array_free(env, true);
  return p;
}
static size_t row_count(Sandbox *s) {
  Rows rows = load_rows(s->log);
  size_t n = rows.len;
  rows_free(rows);
  return n;
}
static bool wait_rows(Sandbox *s, size_t more_than) {
  for (int i = 0; i < 150; i++) {
    Rows rows = load_rows(s->log);
    size_t n = rows.len;
    rows_free(rows);
    if (n > more_than)
      return true;
    g_usleep(20000);
  }
  FAIL("expected command did not appear");
  return false;
}
static Row *nth(Rows rows, size_t i) { return i < rows.len ? &rows.items[i] : NULL; }
static bool pipe_stdio(const Row *row) {
  for (int i = 0; i < 3; i++) {
    const char *s = row->stdio[i];
    if (!s || (strncmp(s, "pipe:", 5) && strncmp(s, "socket:", 7)))
      return false;
  }
  return true;
}
static bool env_exact(const Row *row, const char **keys, const char **vals, int n) {
  if (row->env_n != n)
    return false;
  for (int i = 0; i < n; i++) {
    const char *got = env_get(row, keys[i]);
    if (!got || strcmp(got, vals[i]))
      return false;
  }
  return true;
}
static int json_int(const char *s, const char *key) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(s, pat);
  return p ? atoi(p + strlen(pat)) : -999;
}
static bool json_bool(const char *s, const char *key) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(s, pat);
  return p && !strncmp(p + strlen(pat), "true", 4);
}
static char *json_str(const char *s, const char *key) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = strstr(s, pat);
  if (!p)
    return NULL;
  p += strlen(pat);
  const char *end = strchr(p, '"');
  return end ? g_strndup(p, (gsize)(end - p)) : NULL;
}
static char **with_env(Sandbox *s, const char *home, GPtrArray **owned) {
  GPtrArray *env = g_ptr_array_new();
  for (size_t i = 0; s->env[i]; i++)
    if (!g_str_has_prefix(s->env[i], "HOME="))
      g_ptr_array_add(env, s->env[i]);
  g_ptr_array_add(env, (gpointer)home);
  g_ptr_array_add(env, NULL);
  *owned = env;
  return (char **)env->pdata;
}
int main(void) {
  binary = sibling("DAILY_WORKFLOW_BIN", "daily-workflow");
  pty_bin = sibling("DAILY_WORKFLOW_PTY", "daily-pty");
  fake_bin = sibling(NULL, "daily-fake");
  if (!binary || !pty_bin || !fake_bin) {
    fprintf(stderr, "Set DAILY_WORKFLOW_BIN and DAILY_WORKFLOW_PTY\n");
    return 1;
  }
  begin("invalid actions, options, injection and argument counts do not spawn");
  {
    Sandbox *s = setup();
    const char *cases[][3] = {{NULL}, {"--help", NULL}, {"notes", "extra", NULL},
                              {"_cleanup", "extra", NULL}, {"agenda;touch marker", NULL},
                              {"", NULL}, {"_agenda ", NULL}, {"NOTES", NULL}};
    for (size_t p = 0; p < s->nprograms; p++)
      for (size_t i = 0; i < 8; i++) {
        Proc r = run_s(s, s->programs[p], cases[i][0] ? cases[i] : NULL, "", 0, NULL);
        EXPECT(!r.error && r.status == 64);
        EXPECT(eq_mem(r.err, r.err_len, usage));
        EXPECT(r.out_len == 0);
        proc_clear(&r);
      }
    EXPECT(row_count(s) == 0);
    sandbox_free(s);
  }
  begin("all public actions use fixed scoped argv, original cwd and silenced streams");
  {
    Sandbox *s = setup();
    const char *actions[] = {"workspace", "notes", "agenda", "cleanup"};
    for (size_t p = 0; p < s->nprograms; p++)
      for (size_t a = 0; a < 4; a++) {
        size_t before = row_count(s);
        const char *args[] = {actions[a], NULL};
        Proc r = run_s(s, s->programs[p], args, "", 0, NULL);
        EXPECT(!r.error && r.status == 0 && r.out_len == 0 && r.err_len == 0);
        proc_clear(&r);
        EXPECT(wait_rows(s, before));
        Rows rows = load_rows(s->log);
        Row *e = nth(rows, before);
        EXPECT(e);
        GPtrArray *expect = g_ptr_array_new();
        for (size_t i = 0; i < 6; i++)
          g_ptr_array_add(expect, (gpointer)prefix[i]);
        if (!strcmp(actions[a], "agenda")) {
          g_ptr_array_add(expect, (gpointer) "--title=Today");
          g_ptr_array_add(expect, (gpointer) "--window-width=84");
          g_ptr_array_add(expect, (gpointer) "--window-height=24");
        } else if (!strcmp(actions[a], "notes")) {
          g_ptr_array_add(expect, (gpointer) "--title=Today's notes");
          g_ptr_array_add(expect, (gpointer) "--window-width=100");
          g_ptr_array_add(expect, (gpointer) "--window-height=36");
        }
        char *wd = g_strdup_printf("--working-directory=%s", s->home);
        g_ptr_array_add(expect, wd);
        g_ptr_array_add(expect, (gpointer) "-e");
        if (!strcmp(actions[a], "workspace") || !strcmp(actions[a], "notes")) {
          g_ptr_array_add(expect, (gpointer) "fish");
          g_ptr_array_add(expect, (gpointer) "-ic");
          g_ptr_array_add(expect, (gpointer)(!strcmp(actions[a], "workspace") ? "mux start dot" : "today"));
        } else {
          g_ptr_array_add(expect, (gpointer) "daily-workflow");
          g_ptr_array_add(expect, (gpointer)(!strcmp(actions[a], "agenda") ? "_agenda" : "_cleanup"));
        }
        const char **raw = g_new0(const char *, expect->len + 1);
        for (guint i = 0; i < expect->len; i++)
          raw[i] = expect->pdata[i];
        if (e) {
          EXPECT(args_match(e, raw, (int)expect->len));
          EXPECT(!strcmp(e->cwd, s->root));
          EXPECT(e->sid == e->pid && e->pgrp == e->pid);
          for (int i = 0; i < 3; i++)
            EXPECT(!strcmp(e->stdio[i], "/dev/null"));
        }
        g_free(raw);
        g_free(wd);
        g_ptr_array_free(expect, true);
        rows_free(rows);
      }
    sandbox_free(s);
  }
  begin("launch environment strips every HERDR_ key and overrides mux attachment only");
  {
    Sandbox *s = setup();
    const char *keys[] = {"backend", "plain", "other", "marker"};
    const char *vals[] = {"herdr", "keep-me", "keep-other", "test-marker"};
    for (size_t p = 0; p < s->nprograms; p++) {
      size_t before = row_count(s);
      const char *args[] = {"notes", NULL};
      Proc r = run_s(s, s->programs[p], args, "", 0, NULL);
      EXPECT(!r.error && r.status == 0);
      proc_clear(&r);
      EXPECT(wait_rows(s, before));
      Rows rows = load_rows(s->log);
      Row *e = nth(rows, before);
      EXPECT(e && e->herdr_n == 0 && env_exact(e, keys, vals, 4));
      EXPECT(e && !env_get(e, "attach") && !env_get(e, "herdr"));
      rows_free(rows);
    }
    sandbox_free(s);
  }
  begin("subprocess descriptors close and HOME remains one unsplit argument");
  {
    Sandbox *s = setup();
    for (size_t p = 0; p < s->nprograms; p++) {
      size_t before = row_count(s);
      char *home = g_build_filename(s->root, "hôme; $(touch nope) 'quote' \"q\"", NULL);
      char *envhome = g_strdup_printf("HOME=%s", home);
      GPtrArray *owned = NULL;
      char **env = with_env(s, envhome, &owned);
      char *argv[] = {pty_bin, "fd", s->programs[p], "workspace", NULL};
      Proc r = run_proc(pty_bin, argv, s->root, "", 0, (const char *const *)env, NULL, 8000);
      EXPECT(!r.error && r.status == 0);
      EXPECT(wait_rows(s, before));
      Rows rows = load_rows(s->log);
      Row *e = nth(rows, before);
      char *wd = g_strdup_printf("--working-directory=%s", home);
      EXPECT(e && !e->inherited);
      EXPECT(e && e->argc >= 5 && !strcmp(e->args[e->argc - 5], wd));
      char *nope = g_build_filename(s->root, "nope", NULL);
      EXPECT(!g_file_test(nope, G_FILE_TEST_EXISTS));
      g_free(nope);
      g_free(wd);
      rows_free(rows);
      proc_clear(&r);
      g_ptr_array_free(owned, true);
      g_free(envhome);
      g_free(home);
    }
    sandbox_free(s);
  }
  begin("HOME Path normalization preserves relative paths and two leading slashes");
  {
    Sandbox *s = setup();
    char *homes[6], *expected[6];
    homes[0] = g_strdup(""); expected[0] = g_strdup("/");
    homes[1] = g_strdup("."); expected[1] = g_strdup(".");
    homes[2] = g_strdup("relative//./home/"); expected[2] = g_strdup("relative/home");
    homes[3] = g_strdup_printf("/%s", s->home); expected[3] = g_strdup(homes[3]);
    homes[4] = g_strdup_printf("//%s", s->home); expected[4] = g_strdup(s->home);
    homes[5] = g_strdup_printf("%s/../home/", s->home); expected[5] = g_strdup_printf("%s/../home", s->home);
    for (size_t p = 0; p < s->nprograms; p++)
      for (size_t i = 0; i < 6; i++) {
        size_t before = row_count(s);
        char *envhome = g_strdup_printf("HOME=%s", homes[i]);
        GPtrArray *owned = NULL;
        char **env = with_env(s, envhome, &owned);
        char *argv[] = {s->programs[p], "workspace", NULL};
        Proc r = run_proc(s->programs[p], argv, s->root, "", 0, (const char *const *)env, NULL, 8000);
        EXPECT(!r.error && r.status == 0);
        EXPECT(wait_rows(s, before));
        Rows rows = load_rows(s->log);
        Row *e = nth(rows, before);
        char *wd = g_strdup_printf("--working-directory=%s", expected[i]);
        bool found = false;
        if (e)
          for (int a = 0; a < e->argc; a++)
            if (!strcmp(e->args[a], wd))
              found = true;
        EXPECT(found);
        rows_free(rows);
        proc_clear(&r);
        g_free(wd);
        g_free(envhome);
        g_ptr_array_free(owned, true);
      }
    for (size_t i = 0; i < 6; i++) { g_free(homes[i]); g_free(expected[i]); }
    sandbox_free(s);
  }
  begin("missing scope launcher returns the bounded path-free error");
  {
    Sandbox *s = setup();
    char *path = g_build_filename(s->bin, "systemd-run", NULL);
    unlink(path);
    g_free(path);
    for (size_t p = 0; p < s->nprograms; p++) {
      const char *args[] = {"notes", NULL};
      Proc r = run_s(s, s->programs[p], args, "", 0, NULL);
      EXPECT(!r.error && r.status == 1 && r.out_len == 0 && eq_mem(r.err, r.err_len, error));
      proc_clear(&r);
    }
    sandbox_free(s);
  }
  begin("later scope command failure is not a synchronous launch failure");
  {
    Sandbox *s = setup();
    const char *extra[] = {"DW_STATUS=9", NULL};
    for (size_t p = 0; p < s->nprograms; p++) {
      size_t before = row_count(s);
      const char *args[] = {"agenda", NULL};
      Proc r = run_s(s, s->programs[p], args, "", 0, extra);
      EXPECT(!r.error && r.status == 0 && wait_rows(s, before));
      proc_clear(&r);
    }
    sandbox_free(s);
  }
  begin("internal agenda inherits environment/stdio, propagates status and waits for Enter");
  {
    Sandbox *s = setup();
    int statuses[] = {0, 7, 255};
    const char *keys[] = {"backend", "attach", "herdr", "plain", "other", "marker"};
    const char *vals[] = {"tmux", "attach-value", "remove-me", "keep-me", "keep-other", "test-marker"};
    for (size_t p = 0; p < s->nprograms; p++)
      for (size_t i = 0; i < 3; i++) {
        size_t before = row_count(s);
        char *st = g_strdup_printf("DW_STATUS=%d", statuses[i]);
        const char *extra[] = {st, "DW_STDERR=fixture-error\n", NULL};
        const char *args[] = {"_agenda", NULL};
        Proc r = run_s(s, s->programs[p], args, "\n", 1, extra);
        char *stdout_expect = g_strdup_printf("fixture-daily-agenda\n%s", close_prompt);
        EXPECT(!r.error && r.status == statuses[i]);
        EXPECT(eq_mem(r.out, r.out_len, stdout_expect));
        EXPECT(eq_mem(r.err, r.err_len, "fixture-error\n"));
        Rows rows = load_rows(s->log);
        Row *e = nth(rows, before);
        EXPECT(e && !strcmp(e->name, "daily-agenda") && e->argc == 0);
        EXPECT(e && !strcmp(e->cwd, s->root) && e->sid != e->pid);
        EXPECT(e && env_exact(e, keys, vals, 6) && herdr_has(e, "HERDR_") && pipe_stdio(e));
        rows_free(rows);
        g_free(stdout_expect);
        g_free(st);
        proc_clear(&r);
      }
    sandbox_free(s);
  }
  begin("internal child input is the original stdin");
  {
    Sandbox *s = setup();
    const char *extra[] = {"DW_READ_STDIN=1", NULL};
    for (size_t p = 0; p < s->nprograms; p++) {
      size_t before = row_count(s);
      const char *args[] = {"_agenda", NULL};
      Proc r = run_s(s, s->programs[p], args, "child input\n", 12, extra);
      EXPECT(!r.error && r.status == 0);
      Rows rows = load_rows(s->log);
      Row *e = nth(rows, before);
      EXPECT(e && e->has_input && e->input_len == 12 && !memcmp(e->input, "child input\n", 12));
      rows_free(rows);
      proc_clear(&r);
    }
    sandbox_free(s);
  }
  begin("large child output streams without an adapter buffer or cap");
  {
    Sandbox *s = setup();
    size_t size = 5 * 1024 * 1024;
    char *large = g_strdup_printf("DW_LARGE=%zu", size);
    const char *extra[] = {large, NULL};
    const char *head = "fixture-daily-agenda\n";
    for (size_t p = 0; p < s->nprograms; p++) {
      const char *args[] = {"_agenda", NULL};
      Proc r = run_s(s, s->programs[p], args, "", 0, extra);
      EXPECT(!r.error && r.status == 0);
      EXPECT(r.out_len == strlen(head) + size + strlen(close_prompt));
      EXPECT(r.out && !memcmp(r.out, head, strlen(head)));
      EXPECT(!memcmp(r.out + r.out_len - strlen(close_prompt), close_prompt, strlen(close_prompt)));
      bool xs = true;
      for (size_t i = strlen(head); i < r.out_len - strlen(close_prompt); i++)
        if (r.out[i] != 'x') xs = false;
      EXPECT(xs);
      proc_clear(&r);
    }
    g_free(large);
    sandbox_free(s);
  }
  begin("exact DELETE alone authorizes the home gcall helper, never a shell");
  {
    Sandbox *s = setup();
    const char *inputs[] = {"DELETE\n\n", "DELETE"};
    size_t lens[] = {8, 6};
    for (size_t p = 0; p < s->nprograms; p++)
      for (size_t i = 0; i < 2; i++) {
        size_t before = row_count(s);
        const char *args[] = {"_cleanup", NULL};
        Proc r = run_s(s, s->programs[p], args, inputs[i], lens[i], NULL);
        char *expect = g_strdup_printf("%sfixture-gcall\n%s", banner, close_prompt);
        EXPECT(!r.error && r.status == 0 && r.err_len == 0 && eq_mem(r.out, r.out_len, expect));
        Rows rows = load_rows(s->log);
        Row *e = nth(rows, before);
        EXPECT(e && !strcmp(e->name, "gcall") && e->argc == 0);
        EXPECT(e && env_get(e, "backend") && !strcmp(env_get(e, "backend"), "tmux"));
        rows_free(rows);
        g_free(expect);
        proc_clear(&r);
      }
    sandbox_free(s);
  }
  begin("cleanup rejects whitespace, case, Unicode and embedded NUL confirmations");
  {
    Sandbox *s = setup();
    const char *inputs[] = {"\n\n", "delete\n\n", " DELETE\n\n", "DELETE \n\n", "DELETE\t\n\n",
                            "DELETE\0junk\n\n", "DELETE\0\n\n", "ＤＥＬＥＴＥ\n\n",
                            "DELETE;touch nope\n\n", "DELETE\r\n\r\n", "DELETE\r\r"};
    size_t lens[] = {2, 8, 9, 9, 9, 13, 9, strlen("ＤＥＬＥＴＥ\n\n"), 19, 10, 8};
    for (size_t p = 0; p < s->nprograms; p++)
      for (size_t i = 0; i < 11; i++) {
        const char *args[] = {"_cleanup", NULL};
        Proc r = run_s(s, s->programs[p], args, inputs[i], lens[i], NULL);
        char *expect = g_strdup_printf("%sCancelled. Nothing deleted.\n%s", banner, close_prompt);
        EXPECT(!r.error && r.status == 0 && r.err_len == 0 && eq_mem(r.out, r.out_len, expect));
        g_free(expect);
        proc_clear(&r);
      }
    EXPECT(row_count(s) == 0);
    char *nope = g_build_filename(s->root, "nope", NULL);
    EXPECT(!g_file_test(nope, G_FILE_TEST_EXISTS));
    g_free(nope);
    sandbox_free(s);
  }
  begin("malformed UTF-8 fails before confirmation or close");
  {
    Sandbox *s = setup();
    char input[9];
    input[0] = (char)0xff;
    memcpy(input + 1, "DELETE\n\n", 8);
    for (size_t p = 0; p < s->nprograms; p++) {
      const char *args[] = {"_cleanup", NULL};
      Proc r = run_s(s, s->programs[p], args, input, 9, NULL);
      EXPECT(!r.error && r.status == 1 && eq_mem(r.out, r.out_len, banner));
      if (!strcmp(s->programs[p], binary))
        EXPECT(eq_mem(r.err, r.err_len, error));
      else
        EXPECT(r.err && g_strstr_len(r.err, (gssize)r.err_len, "UnicodeDecodeError"));
      proc_clear(&r);
    }
    EXPECT(row_count(s) == 0);
    sandbox_free(s);
  }
  begin("cleanup EOF and partial confirmation never authorize execution");
  {
    Sandbox *s = setup();
    char nulled[7];
    memcpy(nulled, "DELETE", 6);
    nulled[6] = 0;
    const void *raw[] = {"", "DEL", nulled};
    size_t lens[] = {0, 3, 7};
    for (size_t p = 0; p < s->nprograms; p++)
      for (size_t i = 0; i < 3; i++) {
        const char *args[] = {"_cleanup", NULL};
        Proc r = run_s(s, s->programs[p], args, raw[i], lens[i], NULL);
        char *expect = g_strdup_printf("%s%s%s", banner, lens[i] ? "Cancelled. Nothing deleted.\n" : "", close_prompt);
        EXPECT(!r.error && r.status == 0 && eq_mem(r.out, r.out_len, expect));
        g_free(expect);
        proc_clear(&r);
      }
    EXPECT(row_count(s) == 0);
    sandbox_free(s);
  }
  begin("gcall exit/signal status propagates and close still follows");
  {
    Sandbox *s = setup();
    for (size_t p = 0; p < s->nprograms; p++) {
      int statuses[] = {2, 17};
      for (size_t i = 0; i < 2; i++) {
        char *st = g_strdup_printf("DW_STATUS=%d", statuses[i]);
        const char *extra[] = {st, NULL};
        const char *args[] = {"_cleanup", NULL};
        Proc r = run_s(s, s->programs[p], args, "DELETE\n\n", 8, extra);
        char *expect = g_strdup_printf("%sfixture-gcall\n%s", banner, close_prompt);
        EXPECT(!r.error && r.status == statuses[i] && eq_mem(r.out, r.out_len, expect));
        g_free(expect); g_free(st); proc_clear(&r);
      }
      const char *extra[] = {"DW_SIGNAL=SIGTERM", NULL};
      const char *args[] = {"_cleanup", NULL};
      Proc r = run_s(s, s->programs[p], args, "DELETE\n\n", 8, extra);
      char *expect = g_strdup_printf("%sfixture-gcall\n%s", banner, close_prompt);
      EXPECT(!r.error && r.status == 241 && eq_mem(r.out, r.out_len, expect));
      g_free(expect); proc_clear(&r);
    }
    sandbox_free(s);
  }
  begin("missing internal commands report failure without a close prompt");
  {
    Sandbox *s = setup();
    char *agenda = g_build_filename(s->bin, "daily-agenda", NULL);
    char *gcall = g_build_filename(s->home, ".bin", "gcall", NULL);
    unlink(agenda); unlink(gcall); g_free(agenda); g_free(gcall);
    const char *actions[] = {"_agenda", "_cleanup"};
    for (size_t p = 0; p < s->nprograms; p++)
      for (size_t i = 0; i < 2; i++) {
        const char *args[] = {actions[i], NULL};
        Proc r = run_s(s, s->programs[p], args, "DELETE\n", 7, NULL);
        EXPECT(!r.error && r.status == 1 && eq_mem(r.err, r.err_len, error));
        EXPECT(eq_mem(r.out, r.out_len, !strcmp(actions[i], "_cleanup") ? banner : ""));
        proc_clear(&r);
      }
    sandbox_free(s);
  }
  begin("cleanup stdin I/O errors fail closed rather than authorizing or cancelling");
  {
    Sandbox *s = setup();
    for (size_t p = 0; p < s->nprograms; p++) {
      char *argv[] = {pty_bin, "input-error", s->programs[p], "_cleanup", NULL};
      Proc r = run_proc(pty_bin, argv, s->root, "", 0, (const char *const *)s->env, NULL, 8000);
      EXPECT(!r.error && r.status == 1);
      if (!strcmp(s->programs[p], binary)) {
        EXPECT(eq_mem(r.err, r.err_len, error));
        EXPECT(eq_mem(r.out, r.out_len, banner));
      } else {
        EXPECT(r.err && g_strstr_len(r.err, (gssize)r.err_len, "<stdin> is a directory"));
        EXPECT(r.out_len == 0);
      }
      proc_clear(&r);
    }
    EXPECT(row_count(s) == 0);
    sandbox_free(s);
  }
  const char *pty_modes[] = {"key", "ctrl-c", "interrupt", "queued"};
  for (size_t m = 0; m < 4; m++) {
    char title[128];
    snprintf(title, sizeof title, "agenda PTY %s: raw masks and saved terminal restoration", pty_modes[m]);
    begin(title);
    Sandbox *s = setup();
    for (size_t p = 0; p < s->nprograms; p++) {
      char *argv[] = {pty_bin, (char *)pty_modes[m], s->programs[p], "_agenda", NULL};
      GPtrArray *env = g_ptr_array_new();
      for (size_t i = 0; s->env[i]; i++) g_ptr_array_add(env, s->env[i]);
      g_ptr_array_add(env, (gpointer) "DW_DELAY=60");
      g_ptr_array_add(env, NULL);
      Proc r = run_proc(pty_bin, argv, s->root, "", 0, (const char *const *)env->pdata, NULL, 9000);
      EXPECT(!r.error && r.status == 0 && r.out);
      if (r.out) {
        EXPECT(json_int(r.out, "status") == 0);
        EXPECT(json_bool(r.out, "raw") && json_bool(r.out, "restored"));
        EXPECT(json_bool(r.out, "queued_cleared") == !strcmp(pty_modes[m], "queued"));
        char *b64 = json_str(r.out, "output_b64");
        gsize out_len = 0;
        guchar *text = b64 ? g_base64_decode(b64, &out_len) : NULL;
        EXPECT(text && g_strstr_len((char *)text, (gssize)out_len, "fixture-daily-agenda"));
        EXPECT(!text || !g_strstr_len((char *)text, (gssize)out_len, "Press Enter"));
        g_free(text); g_free(b64);
      }
      proc_clear(&r);
      g_ptr_array_free(env, true);
    }
    sandbox_free(s);
  }
  const char *cleanup_modes[] = {"cleanup-cancel", "cleanup-interrupt", "cleanup-eof", "cleanup-confirm"};
  for (size_t m = 0; m < 4; m++) {
    char title[160];
    snprintf(title, sizeof title, "cleanup PTY %s: canonical confirmation, cancellation and close", cleanup_modes[m]);
    begin(title);
    Sandbox *s = setup();
    for (size_t p = 0; p < s->nprograms; p++) {
      size_t before = row_count(s);
      char *argv[] = {pty_bin, (char *)cleanup_modes[m], s->programs[p], "_cleanup", NULL};
      Proc r = run_proc(pty_bin, argv, s->root, "", 0, (const char *const *)s->env, NULL, 9000);
      EXPECT(!r.error && r.status == 0 && r.out);
      if (r.out) {
        EXPECT(json_int(r.out, "status") == 0);
        EXPECT(json_bool(r.out, "canonical") && json_bool(r.out, "restored") && !json_bool(r.out, "raw"));
        char *b64 = json_str(r.out, "output_b64");
        gsize out_len = 0;
        guchar *text = b64 ? g_base64_decode(b64, &out_len) : NULL;
        EXPECT(text && g_strstr_len((char *)text, (gssize)out_len, "Press Enter to close."));
        EXPECT(row_count(s) - before == (!strcmp(cleanup_modes[m], "cleanup-confirm") ? 1 : 0));
        g_free(text); g_free(b64);
      }
      proc_clear(&r);
    }
    sandbox_free(s);
  }
  begin("interrupt while waiting for an internal child kills/reaps it and skips close");
  {
    Sandbox *s = setup();
    for (size_t p = 0; p < s->nprograms; p++) {
      size_t before = row_count(s);
      int outp[2], errp[2], inp[2];
      if (pipe(outp) || pipe(errp) || pipe(inp)) {
        FAIL("pipe");
        continue;
      }
      pid_t pid = fork();
      if (!pid) {
        setsid();
        dup2(inp[0], 0); dup2(outp[1], 1); dup2(errp[1], 2);
        close(inp[1]); close(outp[0]); close(errp[0]);
        if (chdir(s->root) < 0) _exit(127);
        for (size_t i = 0; s->env[i]; i++) putenv(s->env[i]);
        putenv("DW_DELAY=10000");
        char *argv[] = {s->programs[p], "_agenda", NULL};
        execv(s->programs[p], argv);
        _exit(127);
      }
      close(inp[0]); close(outp[1]); close(errp[1]);
      EXPECT(wait_rows(s, before));
      Rows rows = load_rows(s->log);
      pid_t child = nth(rows, before) ? (pid_t)nth(rows, before)->pid : 0;
      rows_free(rows);
      kill(pid, SIGINT);
      char *out = NULL; size_t out_len = 0, cap = 0;
      int64_t deadline = now_ms() + 5000;
      int status = 0; bool done = false;
      while (!done && now_ms() < deadline) {
        struct pollfd fds[2] = {{outp[0], POLLIN, 0}, {errp[0], POLLIN, 0}};
        poll(fds, 2, 50);
        char tmp[1024];
        for (int i = 0; i < 2; i++) {
          int fd = i ? errp[0] : outp[0];
          if (fd >= 0 && (fds[i].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n > 0 && !i) grow(&out, &out_len, &cap, tmp, (size_t)n);
            else if (n <= 0) { close(fd); if (i) errp[0] = -1; else outp[0] = -1; }
          }
        }
        if (waitpid(pid, &status, WNOHANG) == pid) done = true;
      }
      EXPECT(done && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT);
      EXPECT(!out || !g_strstr_len(out, (gssize)out_len, "Press Enter"));
      bool gone = false;
      for (int i = 0; i < 50 && !gone; i++) {
        if (kill(child, 0) < 0 && errno == ESRCH) gone = true;
        else g_usleep(20000);
      }
      EXPECT(gone);
      free(out);
      if (inp[1] >= 0) close(inp[1]);
      if (outp[0] >= 0) close(outp[0]);
      if (errp[0] >= 0) close(errp[0]);
      if (!done) kill(-pid, SIGKILL);
    }
    sandbox_free(s);
  }
  g_free(binary); g_free(pty_bin); g_free(fake_bin);
  if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  puts("ok");
  return 0;
}
