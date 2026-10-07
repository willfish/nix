#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yyjson.h>

static int failures;
static const char *test_name;
static GPtrArray *temps;

static void fail(int line, const char *msg) {
  fprintf(stderr, "FAIL %s:%d: %s\n", test_name ? test_name : "?", line,
          msg ? msg : "");
  failures++;
}
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fail(__LINE__, #cond);                                                   \
      return;                                                                  \
    }                                                                          \
  } while (0)

static void cleanup_temps(void) {
  if (!temps)
    return;
  for (guint i = 0; i < temps->len; i++) {
    char *argv[] = {"rm", "-rf", "--", temps->pdata[i], NULL};
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                 NULL, NULL);
  }
}
static char *temp_dir(const char *prefix) {
  if (!temps) {
    temps = g_ptr_array_new();
    atexit(cleanup_temps);
  }
  char *tmpl = g_strdup_printf("%s/%sXXXXXX", g_get_tmp_dir(), prefix);
  if (!g_mkdtemp(tmpl)) {
    g_free(tmpl);
    return NULL;
  }
  g_ptr_array_add(temps, tmpl);
  return tmpl;
}
static char *self_exe(void) {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0)
    return g_strdup("hermes-checks");
  buf[n] = 0;
  return g_strdup(buf);
}
static bool write_file(const char *path, const void *data, size_t len,
                       mode_t mode) {
  char *dir = g_path_get_dirname(path);
  int mk = g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  if (mk != 0)
    return false;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return false;
  const char *p = data ? data : "";
  size_t left = len;
  while (left) {
    ssize_t n = write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      return false;
    }
    p += n;
    left -= (size_t)n;
  }
  bool ok = fchmod(fd, mode) == 0;
  if (close(fd) != 0)
    ok = false;
  return ok;
}
static char *read_file(const char *path, size_t *len) {
  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, NULL))
    return NULL;
  if (len)
    *len = length;
  return contents;
}
static bool file_eq(const char *path, const void *data, size_t len) {
  size_t n = 0;
  char *got = read_file(path, &n);
  if (!got)
    return false;
  bool ok = n == len && memcmp(got, data, len) == 0;
  g_free(got);
  return ok;
}
static bool mode_is(const char *path, mode_t mode) {
  struct stat st;
  return stat(path, &st) == 0 && (st.st_mode & 0777) == mode;
}
static ino_t inode_of(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? st.st_ino : 0;
}
typedef struct {
  int code;
  int signal;
  char *out;
  size_t out_len;
  char *err;
  size_t err_len;
  bool failed;
} Proc;
static void proc_free(Proc *p) {
  g_free(p->out);
  g_free(p->err);
  p->out = p->err = NULL;
}
static Proc proc_run(const char *file, char *const *argv, const char *cwd,
                     char **envp, int timeout_ms) {
  Proc p = {0};
  p.code = -1;
  p.failed = true;
  int outp[2], errp[2];
  if (pipe(outp) || pipe(errp))
    return p;
  pid_t pid = fork();
  if (pid < 0) {
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    return p;
  }
  if (pid == 0) {
    setpgid(0, 0);
    dup2(outp[1], STDOUT_FILENO);
    dup2(errp[1], STDERR_FILENO);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    for (int fd = 3; fd < 64; fd++)
      close(fd);
    if (cwd && chdir(cwd) != 0)
      _exit(127);
    execvpe(file, argv, envp ? envp : environ);
    _exit(127);
  }
  setpgid(pid, pid);
  close(outp[1]);
  close(errp[1]);
  GString *out = g_string_new(NULL), *err = g_string_new(NULL);
  int open_out = outp[0], open_err = errp[0];
  bool timed_out = false;
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  while (open_out >= 0 || open_err >= 0) {
    struct pollfd fds[2];
    int nfd = 0, out_i = -1, err_i = -1;
    if (open_out >= 0) {
      out_i = nfd;
      fds[nfd].fd = open_out;
      fds[nfd].events = POLLIN;
      nfd++;
    }
    if (open_err >= 0) {
      err_i = nfd;
      fds[nfd].fd = open_err;
      fds[nfd].events = POLLIN;
      nfd++;
    }
    int remain = (int)((deadline - g_get_monotonic_time()) / 1000);
    if (remain < 0)
      remain = 0;
    int pr = poll(fds, (nfds_t)nfd, remain);
    if (pr == 0) {
      timed_out = true;
      kill(-pid, SIGKILL);
      break;
    }
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      timed_out = true;
      kill(-pid, SIGKILL);
      break;
    }
    char buf[4096];
    if (out_i >= 0 && (fds[out_i].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = read(open_out, buf, sizeof buf);
      if (n > 0)
        g_string_append_len(out, buf, (gssize)n);
      else if (n == 0 || (n < 0 && errno != EINTR)) {
        close(open_out);
        open_out = -1;
      }
    }
    if (err_i >= 0 && (fds[err_i].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = read(open_err, buf, sizeof buf);
      if (n > 0)
        g_string_append_len(err, buf, (gssize)n);
      else if (n == 0 || (n < 0 && errno != EINTR)) {
        close(open_err);
        open_err = -1;
      }
    }
  }
  if (open_out >= 0)
    close(open_out);
  if (open_err >= 0)
    close(open_err);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  p.out_len = out->len;
  p.err_len = err->len;
  p.out = g_string_free(out, FALSE);
  p.err = g_string_free(err, FALSE);
  if (timed_out)
    return p;
  p.failed = false;
  if (WIFEXITED(status))
    p.code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) {
    p.signal = WTERMSIG(status);
    p.code = -1;
  }
  return p;
}
static char **env_blank(void) { return g_new0(char *, 1); }
static char **env_copy(char **src) {
  size_t n = 0;
  if (src)
    while (src[n])
      n++;
  char **out = g_new0(char *, n + 1);
  for (size_t i = 0; i < n; i++)
    out[i] = g_strdup(src[i]);
  return out;
}
static void env_set(char ***envp, const char *key, const char *value) {
  size_t klen = strlen(key), vlen = strlen(value);
  char *entry = g_malloc(klen + vlen + 2);
  if (!entry)
    abort();
  memcpy(entry, key, klen);
  entry[klen] = '=';
  memcpy(entry + klen + 1, value, vlen + 1);
  char **env = *envp;
  size_t n = 0;
  if (env) {
    for (; env[n]; n++) {
      if (!strncmp(env[n], key, klen) && env[n][klen] == '=') {
        g_free(env[n]);
        env[n] = entry;
        return;
      }
    }
  }
  char **next = g_realloc(env, (n + 2) * sizeof(char *));
  if (!next)
    abort();
  next[n] = entry;
  next[n + 1] = NULL;
  *envp = next;
}
static void env_free(char **env) {
  if (!env)
    return;
  for (size_t i = 0; env[i]; i++)
    g_free(env[i]);
  g_free(env);
}
static bool val_streq(yyjson_val *v, const char *s) {
  if (!yyjson_is_str(v) || !s)
    return false;
  size_t n = yyjson_get_len(v), m = strlen(s);
  return n == m && memcmp(yyjson_get_str(v), s, n) == 0;
}
static bool json_equal(yyjson_val *a, yyjson_val *b) {
  if (!a || !b)
    return a == b;
  if (yyjson_is_null(a) || yyjson_is_null(b))
    return yyjson_is_null(a) && yyjson_is_null(b);
  if (yyjson_is_bool(a) || yyjson_is_bool(b))
    return yyjson_is_bool(a) && yyjson_is_bool(b) &&
           yyjson_get_bool(a) == yyjson_get_bool(b);
  if (yyjson_is_num(a) || yyjson_is_num(b)) {
    if (!yyjson_is_num(a) || !yyjson_is_num(b))
      return false;
    if (yyjson_is_int(a) && yyjson_is_int(b))
      return yyjson_get_sint(a) == yyjson_get_sint(b);
    return yyjson_get_num(a) == yyjson_get_num(b);
  }
  if (yyjson_is_str(a) || yyjson_is_str(b)) {
    if (!yyjson_is_str(a) || !yyjson_is_str(b))
      return false;
    size_t n = yyjson_get_len(a);
    return n == yyjson_get_len(b) &&
           memcmp(yyjson_get_str(a), yyjson_get_str(b), n) == 0;
  }
  if (yyjson_is_arr(a) || yyjson_is_arr(b)) {
    if (!yyjson_is_arr(a) || !yyjson_is_arr(b) ||
        yyjson_arr_size(a) != yyjson_arr_size(b))
      return false;
    for (size_t i = 0; i < yyjson_arr_size(a); i++)
      if (!json_equal(yyjson_arr_get(a, i), yyjson_arr_get(b, i)))
        return false;
    return true;
  }
  if (!yyjson_is_obj(a) || !yyjson_is_obj(b) ||
      yyjson_obj_size(a) != yyjson_obj_size(b))
    return false;
  size_t idx, max;
  yyjson_val *key, *val;
  yyjson_obj_foreach(a, idx, max, key, val) {
    yyjson_val *other = yyjson_obj_get(b, yyjson_get_str(key));
    if (!other || !json_equal(val, other))
      return false;
  }
  return true;
}
static bool json_text_eq(yyjson_val *v, const char *text) {
  yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
  bool ok = doc && json_equal(v, yyjson_doc_get_root(doc));
  if (!ok) {
    char *got = v ? yyjson_val_write(v, YYJSON_WRITE_PRETTY, NULL) : NULL;
    fprintf(stderr, "json mismatch\n got: %s\n want: %s\n", got ? got : "null",
            text);
    free(got);
  }
  yyjson_doc_free(doc);
  return ok;
}
static bool arr_eq(yyjson_val *arr, const char **expect, size_t n) {
  if (!yyjson_is_arr(arr) || yyjson_arr_size(arr) != n)
    return false;
  for (size_t i = 0; i < n; i++)
    if (!val_streq(yyjson_arr_get(arr, i), expect[i]))
      return false;
  return true;
}
static bool paths_from(yyjson_val *files, size_t start, const char **expect,
                       size_t n) {
  if (!yyjson_is_arr(files) || yyjson_arr_size(files) != start + n)
    return false;
  for (size_t i = 0; i < n; i++) {
    yyjson_val *path =
        yyjson_obj_get(yyjson_arr_get(files, start + i), "path");
    if (!val_streq(path, expect[i]))
      return false;
  }
  return true;
}
static yyjson_val *file_by_path(yyjson_val *files, const char *path) {
  if (!yyjson_is_arr(files))
    return NULL;
  for (size_t i = 0; i < yyjson_arr_size(files); i++) {
    yyjson_val *item = yyjson_arr_get(files, i);
    if (val_streq(yyjson_obj_get(item, "path"), path))
      return item;
  }
  return NULL;
}
static bool mem_has(const char *hay, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (!m || m > n)
    return false;
  for (size_t i = 0; i + m <= n; i++)
    if (!memcmp(hay + i, needle, m))
      return true;
  return false;
}
static bool export_err_ok(const char *err) {
  if (!err)
    return true;
  return !strstr(err, "ERROR: AddressSanitizer") &&
         !strstr(err, "ERROR: LeakSanitizer") && !strstr(err, "runtime error:") &&
         !strstr(err, "DEADLYSIGNAL");
}
static bool managed_err_ok(const char *err) {
  if (!err)
    return true;
  return !strstr(err, "AddressSanitizer") && !strstr(err, "LeakSanitizer") &&
         !strstr(err, "runtime error:") && !strstr(err, "PRIVATE-SENTINEL");
}
static bool expect_code(Proc *p, int code, bool managed) {
  if (p->failed) {
    fprintf(stderr, "spawn failed\n");
    return false;
  }
  if (!(managed ? managed_err_ok(p->err) : export_err_ok(p->err))) {
    fprintf(stderr, "stderr rejected: %s\n", p->err ? p->err : "");
    return false;
  }
  if (p->code != code) {
    fprintf(stderr, "expected %d got %d signal %d\nstderr: %s\nstdout: %s\n",
            code, p->code, p->signal, p->err ? p->err : "",
            p->out ? p->out : "");
    return false;
  }
  return true;
}
static int fake_sops(int argc, char **argv) {
  size_t cap = 4096, n = 0;
  char *buf = malloc(cap);
  if (!buf)
    return 1;
  for (;;) {
    if (n == cap) {
      size_t next = cap * 2;
      char *grown = realloc(buf, next);
      if (!grown) {
        free(buf);
        return 1;
      }
      buf = grown;
      cap = next;
    }
    size_t got = fread(buf + n, 1, cap - n, stdin);
    n += got;
    if (got == 0)
      break;
  }
  if (ferror(stdin)) {
    free(buf);
    return 1;
  }
  const char *mode = getenv("SOPS_FIXTURE_MODE");
  if (mode && !strcmp(mode, "failure")) {
    fwrite(buf, 1, n, stdout);
    fputs("PRIVATE-FAILURE ", stderr);
    fwrite(buf, 1, n, stderr);
    free(buf);
    return 7;
  }
  if (mode && !strcmp(mode, "signal")) {
    free(buf);
    raise(SIGTERM);
    return 1;
  }
  if (mode && !strcmp(mode, "large-error")) {
    char *xs = malloc(200000);
    if (!xs) {
      free(buf);
      return 1;
    }
    memset(xs, 'x', 200000);
    fwrite(xs, 1, 200000, stderr);
    free(xs);
    free(buf);
    return 4;
  }
  yyjson_doc *in = yyjson_read(buf, n, 0);
  free(buf);
  if (!in)
    return 1;
  yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(out);
  yyjson_mut_doc_set_root(out, root);
  yyjson_mut_obj_add_val(out, root, "declaration",
                         yyjson_val_mut_copy(out, yyjson_doc_get_root(in)));
  yyjson_mut_val *args = yyjson_mut_arr(out);
  for (int i = 2; i < argc; i++)
    yyjson_mut_arr_add_strcpy(out, args, argv[i]);
  yyjson_mut_obj_add_val(out, root, "args", args);
  char *cwd = g_get_current_dir();
  yyjson_mut_obj_add_strcpy(out, root, "cwd", cwd);
  g_free(cwd);
  size_t len = 0;
  char *text = yyjson_mut_write(out, 0, &len);
  yyjson_doc_free(in);
  yyjson_mut_doc_free(out);
  if (!text)
    return 1;
  fwrite(text, 1, len, stdout);
  free(text);
  return 0;
}
static const char *required_files[] = {
    "config.yaml", "SOUL.md", "AGENTS.md", "profiles/qwen/config.yaml"};
static const char *runtime_keys[] = {
    "last_run_at", "next_run_at", "last_status", "last_error",
    "last_delivery_error", "last_dispatch", "failure_streak", "fire_claim"};
typedef struct {
  char *root, *home, *repo, *output, *commands;
  char **env;
} Fix;
static char *export_bin(void) {
  const char *env = getenv("HERMES_EXPORT_BIN");
  if (env && *env)
    return g_strdup(env);
  char *exe = self_exe(), *dir = g_path_get_dirname(exe);
  char *bin = g_build_filename(dir, "hermes-export", NULL);
  g_free(dir);
  g_free(exe);
  return bin;
}
static char *managed_bin(const char *name) {
  const char *dir = getenv("HERMES_MANAGED_BIN_DIR");
  if (!dir || !*dir) {
    char *exe = self_exe();
    dir = g_path_get_dirname(exe);
    char *bin = g_build_filename(dir, name, NULL);
    g_free((char *)dir);
    g_free(exe);
    return bin;
  }
  return g_build_filename(dir, name, NULL);
}
static Fix *setup_export(void) {
  Fix *f = g_new0(Fix, 1);
  f->root = temp_dir("hermes-export-");
  if (!f->root)
    return f;
  f->home = g_build_filename(f->root, "home", NULL);
  f->repo = g_build_filename(f->root, "repo", NULL);
  f->output = g_build_filename(f->repo, "secrets", "hermes.json", NULL);
  f->commands = g_build_filename(f->root, "bin", NULL);
  g_mkdir_with_parents(f->commands, 0700);
  char *parent = g_path_get_dirname(f->output);
  g_mkdir_with_parents(parent, 0700);
  g_free(parent);
  for (size_t i = 0; i < G_N_ELEMENTS(required_files); i++) {
    char *path = g_build_filename(f->home, required_files[i], NULL);
    char *text = g_strdup_printf("fixture %s\n", required_files[i]);
    write_file(path, text, strlen(text), 0600);
    g_free(text);
    g_free(path);
  }
  GString *jobs = g_string_new(
      "{\"jobs\":[{\"id\":\"job\",\"schedule\":{\"kind\":\"cron\"},\"prompt\":"
      "\"fixture prompt\",\"repeat\":{\"times\":null,\"completed\":7}");
  for (size_t i = 0; i < G_N_ELEMENTS(runtime_keys); i++)
    g_string_append_printf(jobs, ",\"%s\":\"bookkeeping\"", runtime_keys[i]);
  g_string_append(jobs, "}]}");
  char *jobs_path = g_build_filename(f->home, "cron/jobs.json", NULL);
  write_file(jobs_path, jobs->str, jobs->len, 0600);
  g_free(jobs_path);
  g_string_free(jobs, TRUE);
  char *exe = self_exe();
  char *script = g_strdup_printf("#!/bin/sh\nexec '%s' --fixture-sops \"$@\"\n",
                                 exe);
  char *sops = g_build_filename(f->commands, "sops", NULL);
  write_file(sops, script, strlen(script), 0700);
  g_free(script);
  g_free(sops);
  g_free(exe);
  const char *old_path = g_getenv("PATH");
  char *path =
      old_path ? g_strconcat(f->commands, ":", old_path, NULL) : g_strdup(f->commands);
  f->env = env_blank();
  env_set(&f->env, "PATH", path);
  env_set(&f->env, "HOME", f->root);
  g_free(path);
  return f;
}
static Proc run_export(Fix *f, bool old, const char **args, int nargs,
                       char **env) {
  GPtrArray *av = g_ptr_array_new_with_free_func(g_free);
  if (old) {
    const char *py = getenv("HERMES_EXPORT_PYTHON");
    g_ptr_array_add(av, g_strdup(py && *py ? py : "python3"));
    g_ptr_array_add(av, g_strdup(getenv("HERMES_EXPORT_LEGACY")));
  } else
    g_ptr_array_add(av, export_bin());
  if (!args) {
    g_ptr_array_add(av, g_strdup(f->home));
    g_ptr_array_add(av, g_strdup(f->output));
  } else
    for (int i = 0; i < nargs; i++)
      g_ptr_array_add(av, g_strdup(args[i]));
  g_ptr_array_add(av, NULL);
  Proc p = proc_run(av->pdata[0], (char **)av->pdata, f->root,
                    env ? env : f->env, 15000);
  g_ptr_array_free(av, TRUE);
  return p;
}
static yyjson_doc *read_json_file(const char *path) {
  size_t n = 0;
  char *text = read_file(path, &n);
  if (!text)
    return NULL;
  yyjson_doc *doc = yyjson_read(text, n, 0);
  g_free(text);
  return doc;
}
static yyjson_doc *parity(Fix *f) {
  Proc p = run_export(f, false, NULL, 0, NULL);
  if (!expect_code(&p, 0, false)) {
    proc_free(&p);
    fail(__LINE__, "export status");
    return NULL;
  }
  yyjson_doc *doc = read_json_file(f->output);
  const char *legacy = getenv("HERMES_EXPORT_LEGACY");
  if (legacy && *legacy) {
    size_t out_len = p.out_len;
    char *out = g_malloc(out_len + 1);
    if (out_len)
      memcpy(out, p.out, out_len);
    out[out_len] = 0;
    proc_free(&p);
    Proc old = run_export(f, true, NULL, 0, NULL);
    if (!expect_code(&old, 0, false)) {
      proc_free(&old);
      g_free(out);
      yyjson_doc_free(doc);
      fail(__LINE__, "legacy export");
      return NULL;
    }
    yyjson_doc *again = read_json_file(f->output);
    bool same = doc && again && json_equal(yyjson_doc_get_root(doc),
                                           yyjson_doc_get_root(again));
    bool out_same =
        old.out && out_len == old.out_len && !memcmp(old.out, out, out_len);
    yyjson_doc_free(again);
    proc_free(&old);
    g_free(out);
    if (!same || !out_same) {
      yyjson_doc_free(doc);
      fail(__LINE__, "legacy parity");
      return NULL;
    }
    return doc;
  }
  proc_free(&p);
  return doc;
}
static void test_capture(void) {
  test_name = "captures required files";
  Fix *f = setup_export();
  CHECK(f && f->root);
  char *script = g_build_filename(f->home, "scripts/run.sh", NULL);
  write_file(script, "#!/bin/sh\nprintf fixture\n", 22, 0710);
  g_free(script);
  char bytes[] = {0, (char)255, 65, 10};
  char *asset = g_build_filename(f->home, "assets/bytes", NULL);
  write_file(asset, bytes, 4, 0600);
  g_free(asset);
  char *auth = g_build_filename(f->home, "auth.json", NULL);
  write_file(auth, "fixture-auth", 12, 0600);
  g_free(auth);
  char *qauth = g_build_filename(f->home, "profiles/qwen/auth.json", NULL);
  write_file(qauth, "fixture-qwen-auth", 17, 0600);
  g_free(qauth);
  char *session = g_build_filename(f->home, "sessions/private.json", NULL);
  write_file(session, "must not export", 15, 0600);
  g_free(session);
  yyjson_doc *doc = parity(f);
  CHECK(doc);
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *d = yyjson_obj_get(root, "declaration");
  CHECK(yyjson_get_sint(yyjson_obj_get(d, "version")) == 1);
  const char *paths[] = {"config.yaml", "SOUL.md", "AGENTS.md",
                         "profiles/qwen/config.yaml", "scripts/run.sh",
                         "assets/bytes"};
  CHECK(paths_from(yyjson_obj_get(d, "files"), 0, paths, 6));
  yyjson_val *run = file_by_path(yyjson_obj_get(d, "files"), "scripts/run.sh");
  CHECK(yyjson_get_bool(yyjson_obj_get(run, "executable")));
  yyjson_val *last =
      yyjson_arr_get(yyjson_obj_get(d, "files"),
                     yyjson_arr_size(yyjson_obj_get(d, "files")) - 1);
  gsize n = 0;
  guchar *decoded =
      g_base64_decode(yyjson_get_str(yyjson_obj_get(last, "content")), &n);
  CHECK(n == 4 && decoded && !memcmp(decoded, bytes, 4));
  g_free(decoded);
  const char *seeds[] = {"auth.json", "profiles/qwen/auth.json"};
  yyjson_val *seed_files = yyjson_obj_get(d, "seed_files");
  CHECK(yyjson_arr_size(seed_files) == 2);
  CHECK(val_streq(yyjson_obj_get(yyjson_arr_get(seed_files, 0), "path"),
                  seeds[0]));
  CHECK(val_streq(yyjson_obj_get(yyjson_arr_get(seed_files, 1), "path"),
                  seeds[1]));
  CHECK(json_text_eq(
      yyjson_obj_get(d, "jobs"),
      "[{\"id\":\"job\",\"schedule\":{\"kind\":\"cron\"},\"prompt\":\"fixture "
      "prompt\",\"repeat\":{\"times\":null,\"completed\":0}}]"));
  CHECK(val_streq(yyjson_obj_get(root, "cwd"), f->repo));
  const char *sops_args[] = {"--encrypt", "--input-type", "json",
                             "--output-type", "json", "--filename-override",
                             f->output, "/dev/stdin"};
  CHECK(arr_eq(yyjson_obj_get(root, "args"), sops_args, 8));
  yyjson_doc_free(doc);
}
static void test_sorted(void) {
  test_name = "recursive capture is component-sorted";
  Fix *f = setup_export();
  CHECK(f && f->root);
  const char *files[] = {
      "scripts/a/z", "scripts/a.md", "scripts/b", "skills/example/SKILL.md",
      "skills/.private.md", "skills/example/.state",
      "skills/node_modules/vendor/a", "skills/__pycache__/cache",
      "scripts/nested/apply-house-telegram.py", "scripts/weekly-hermes-update.sh"};
  for (size_t i = 0; i < G_N_ELEMENTS(files); i++) {
    char *path = g_build_filename(f->home, files[i], NULL);
    write_file(path, "fixture", 7, 0600);
    g_free(path);
  }
  yyjson_doc *doc = parity(f);
  CHECK(doc);
  const char *expect[] = {"skills/example/SKILL.md", "scripts/a/z",
                          "scripts/a.md", "scripts/b"};
  CHECK(paths_from(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc),
                                                 "declaration"),
                                  "files"),
                   4, expect, 4));
  yyjson_doc_free(doc);
}
static void test_mem0(void) {
  test_name = "mem0 plugin relocation";
  Fix *f = setup_export();
  CHECK(f && f->root);
  char *first = g_build_filename(f->home, "plugins/mem0-selfhosted/config.txt", NULL);
  write_file(first, "declared-first", 14, 0600);
  g_free(first);
  const char *names[] = {"config.txt", ".hidden", "node_modules/fixture",
                         "__pycache__/cache", "memory.py"};
  const char *values[] = {"second", "plugin-hidden", "plugin-node", "ignored",
                          "plugin-source"};
  for (size_t i = 0; i < 5; i++) {
    char *rel = g_build_filename("hermes-agent/plugins/memory/mem0-selfhosted",
                                 names[i], NULL);
    char *path = g_build_filename(f->home, rel, NULL);
    write_file(path, values[i], strlen(values[i]), 0600);
    g_free(path);
    g_free(rel);
  }
  yyjson_doc *doc = parity(f);
  CHECK(doc);
  yyjson_val *files = yyjson_obj_get(
      yyjson_obj_get(yyjson_doc_get_root(doc), "declaration"), "files");
  const char *expect[] = {
      "plugins/mem0-selfhosted/config.txt", "plugins/mem0-selfhosted/.hidden",
      "plugins/mem0-selfhosted/memory.py",
      "plugins/mem0-selfhosted/node_modules/fixture"};
  size_t found = 0;
  for (size_t i = 0; i < yyjson_arr_size(files); i++) {
    yyjson_val *item = yyjson_arr_get(files, i);
    const char *path = yyjson_get_str(yyjson_obj_get(item, "path"));
    if (path && g_str_has_prefix(path, "plugins/mem0-selfhosted/")) {
      CHECK(found < 4);
      CHECK(val_streq(yyjson_obj_get(item, "path"), expect[found]));
      if (found == 0) {
        gsize n = 0;
        guchar *decoded =
            g_base64_decode(yyjson_get_str(yyjson_obj_get(item, "content")), &n);
        CHECK(n == 14 && decoded && !memcmp(decoded, "declared-first", 14));
        g_free(decoded);
      }
      found++;
    }
  }
  CHECK(found == 4);
  yyjson_doc_free(doc);
}
static void test_symlinks(void) {
  test_name = "leaf symlinks stay owned elsewhere";
  Fix *f = setup_export();
  CHECK(f && f->root);
  char *linked = g_build_filename(f->root, "linked", NULL);
  write_file(linked, "linked data", 11, 0600);
  char *soul = g_build_filename(f->home, "SOUL.md", NULL);
  unlink(soul);
  CHECK(symlink(linked, soul) == 0);
  char *auth = g_build_filename(f->home, "auth.json", NULL);
  CHECK(symlink(linked, auth) == 0);
  char *assets = g_build_filename(f->home, "assets", NULL);
  g_mkdir_with_parents(assets, 0700);
  char *link = g_build_filename(assets, "link", NULL);
  CHECK(symlink(linked, link) == 0);
  char *directory = g_build_filename(f->root, "directory", NULL);
  g_mkdir_with_parents(directory, 0700);
  char *skip = g_build_filename(directory, "skip", NULL);
  write_file(skip, "not followed", 12, 0600);
  char *dirlink = g_build_filename(assets, "directory", NULL);
  CHECK(symlink(directory, dirlink) == 0);
  yyjson_doc *doc = parity(f);
  CHECK(doc);
  yyjson_val *d = yyjson_obj_get(yyjson_doc_get_root(doc), "declaration");
  const char *paths[] = {"config.yaml", "AGENTS.md", "profiles/qwen/config.yaml"};
  CHECK(paths_from(yyjson_obj_get(d, "files"), 0, paths, 3));
  CHECK(yyjson_arr_size(yyjson_obj_get(d, "seed_files")) == 0);
  yyjson_doc_free(doc);
  unlink(linked);
  write_file(f->output, "previous encrypted output", 25, 0600);
  const char *legacy = getenv("HERMES_EXPORT_LEGACY");
  int rounds = legacy && *legacy ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    Proc p = run_export(f, old == 1, NULL, 0, NULL);
    CHECK(expect_code(&p, 1, false));
    proc_free(&p);
    CHECK(file_eq(f->output, "previous encrypted output", 25));
  }
  g_free(linked);
  g_free(soul);
  g_free(auth);
  g_free(assets);
  g_free(link);
  g_free(directory);
  g_free(skip);
  g_free(dirlink);
}
static void store_bad(const char *name) {
  Fix *f = setup_export();
  if (!f || !f->root) {
    fail(__LINE__, "setup");
    return;
  }
  char *rel = !strcmp(name, ".md")
                  ? g_build_filename(
                        "hermes-agent/plugins/memory/mem0-selfhosted", name, NULL)
                  : g_build_filename("scripts", name, NULL);
  char *path = g_build_filename(f->home, rel, NULL);
  char body[64];
  body[0] = 0;
  memcpy(body + 1, "/nix/store/fixture-sensitive", 28);
  if (!write_file(path, body, 29, 0600) ||
      !write_file(f->output, "old ciphertext", 14, 0600)) {
    fail(__LINE__, "write");
    g_free(rel);
    g_free(path);
    return;
  }
  const char *legacy = getenv("HERMES_EXPORT_LEGACY");
  int rounds = legacy && *legacy ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    Proc p = run_export(f, old == 1, NULL, 0, NULL);
    if (!expect_code(&p, 1, false) || !file_eq(f->output, "old ciphertext", 14) ||
        mem_has(p.err, p.err_len, "fixture-sensitive") ||
        mem_has(p.err, p.err_len, "/nix/store")) {
      fprintf(stderr, "store reject failed for %s\n%s\n", name,
              p.err ? p.err : "");
      fail(__LINE__, "store reference");
      proc_free(&p);
      g_free(rel);
      g_free(path);
      return;
    }
    proc_free(&p);
  }
  g_free(rel);
  g_free(path);
}
static void test_store(void) {
  test_name = "store references";
  Fix *f = setup_export();
  CHECK(f && f->root);
  char *md = g_build_filename(f->home, "scripts/reference.md", NULL);
  write_file(md, "/nix/store/fixture-path", 23, 0600);
  g_free(md);
  yyjson_doc *doc = parity(f);
  CHECK(doc);
  CHECK(file_by_path(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc),
                                                   "declaration"),
                                    "files"),
                     "scripts/reference.md"));
  yyjson_doc_free(doc);
  const char *names[] = {"run.sh", "README.MD", ".md", "binary"};
  for (size_t i = 0; i < 4; i++) {
    store_bad(names[i]);
    CHECK(failures == 0 || test_name);
    if (failures)
      return;
  }
}
static void test_runtime_jobs(void) {
  test_name = "runtime fields removed only at top level";
  Fix *f = setup_export();
  CHECK(f && f->root);
  GString *jobs = g_string_new("{\"jobs\":[");
  g_string_append(
      jobs,
      "{\"id\":\"one\",\"enabled\":false,\"state\":\"completed\",\"repeat\":{"
      "\"times\":5,\"completed\":9,\"other\":true},\"nested\":{\"last_run_at\":"
      "\"keep\"}");
  for (size_t i = 0; i < G_N_ELEMENTS(runtime_keys); i++)
    g_string_append_printf(jobs, ",\"%s\":\"drop\"", runtime_keys[i]);
  g_string_append(
      jobs, "},{\"id\":\"two\",\"repeat\":null},{\"id\":\"three\",\"repeat\":5},"
            "{\"id\":\"four\",\"repeat\":{}}],\"extra\":\"not exported\"}");
  char *path = g_build_filename(f->home, "cron/jobs.json", NULL);
  write_file(path, jobs->str, jobs->len, 0600);
  g_free(path);
  g_string_free(jobs, TRUE);
  yyjson_doc *doc = parity(f);
  CHECK(doc);
  CHECK(json_text_eq(
      yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "declaration"),
                     "jobs"),
      "[{\"id\":\"one\",\"enabled\":false,\"state\":\"completed\",\"repeat\":{"
      "\"times\":5,\"completed\":0,\"other\":true},\"nested\":{\"last_run_at\":"
      "\"keep\"}},{\"id\":\"two\",\"repeat\":null},{\"id\":\"three\","
      "\"repeat\":5},{\"id\":\"four\",\"repeat\":{\"completed\":0}}]"));
  yyjson_doc_free(doc);
}
static void missing_case(const char *type) {
  Fix *f = setup_export();
  if (!f || !f->root) {
    fail(__LINE__, "setup");
    return;
  }
  char *agents = g_build_filename(f->home, "AGENTS.md", NULL);
  char *jobs = g_build_filename(f->home, "cron/jobs.json", NULL);
  if (!strcmp(type, "missing"))
    unlink(agents);
  if (!strcmp(type, "directory")) {
    unlink(agents);
    g_mkdir_with_parents(agents, 0700);
  }
  if (!strcmp(type, "bad-json"))
    write_file(jobs, "bad-json", 8, 0600);
  if (!strcmp(type, "missing-jobs"))
    write_file(jobs, "{}", 2, 0600);
  if (!strcmp(type, "nonobject-job"))
    write_file(jobs, "{\"jobs\":[4]}", 12, 0600);
  write_file(f->output, "old ciphertext", 14, 0600);
  const char *legacy = getenv("HERMES_EXPORT_LEGACY");
  int rounds = legacy && *legacy ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    Proc p = run_export(f, old == 1, NULL, 0, NULL);
    if (!expect_code(&p, 1, false) || !file_eq(f->output, "old ciphertext", 14)) {
      fprintf(stderr, "missing case %s failed\n", type);
      fail(__LINE__, type);
      proc_free(&p);
      g_free(agents);
      g_free(jobs);
      return;
    }
    proc_free(&p);
  }
  g_free(agents);
  g_free(jobs);
}
static void test_missing(void) {
  test_name = "missing required config";
  const char *types[] = {"missing", "directory", "bad-json", "missing-jobs",
                         "nonobject-job"};
  for (size_t i = 0; i < 5; i++) {
    missing_case(types[i]);
    if (failures)
      return;
  }
}
static void test_encrypt_fail(void) {
  test_name = "failed encryption suppresses output";
  Fix *f = setup_export();
  CHECK(f && f->root);
  char *auth = g_build_filename(f->home, "auth.json", NULL);
  write_file(auth, "DO-NOT-PRINT-CREDENTIAL", 22, 0600);
  g_free(auth);
  write_file(f->output, "old ciphertext", 14, 0600);
  const char *modes[] = {"failure", "signal", "large-error"};
  const char *legacy = getenv("HERMES_EXPORT_LEGACY");
  int rounds = legacy && *legacy ? 2 : 1;
  for (size_t m = 0; m < 3; m++)
    for (int old = 0; old < rounds; old++) {
      char **env = env_copy(f->env);
      env_set(&env, "SOPS_FIXTURE_MODE", modes[m]);
      Proc p = run_export(f, old == 1, NULL, 0, env);
      env_free(env);
      CHECK(expect_code(&p, 1, false));
      CHECK(p.out_len == 0);
      CHECK(p.err && strstr(p.err, "SOPS encryption failed; no declaration written"));
      CHECK(!strstr(p.err, "PRIVATE-FAILURE") && !strstr(p.err, "DO-NOT-PRINT") &&
            !strstr(p.err, "content"));
      CHECK(file_eq(f->output, "old ciphertext", 14));
      proc_free(&p);
    }
}
static void test_relative(void) {
  test_name = "relative outputs retain filename override";
  Fix *f = setup_export();
  CHECK(f && f->root);
  const char *args[] = {f->home, "repo/secrets/hermes.json"};
  Proc p = run_export(f, false, args, 2, NULL);
  CHECK(expect_code(&p, 0, false));
  proc_free(&p);
  yyjson_doc *doc = read_json_file(f->output);
  CHECK(doc);
  yyjson_val *sargs = yyjson_obj_get(yyjson_doc_get_root(doc), "args");
  size_t n = yyjson_arr_size(sargs);
  CHECK(n >= 2);
  CHECK(val_streq(yyjson_arr_get(sargs, n - 2), "repo/secrets/hermes.json"));
  CHECK(val_streq(yyjson_obj_get(yyjson_doc_get_root(doc), "cwd"), f->repo));
  yyjson_doc_free(doc);
  char *target = g_build_filename(f->root, "other/secrets/export.json", NULL);
  write_file(target, "old", 3, 0640);
  unlink(f->output);
  CHECK(symlink(target, f->output) == 0);
  p = run_export(f, false, NULL, 0, NULL);
  CHECK(expect_code(&p, 0, false));
  proc_free(&p);
  char *resolved = realpath(f->output, NULL);
  CHECK(resolved && !strcmp(resolved, target));
  free(resolved);
  CHECK(mode_is(target, 0640));
  doc = read_json_file(f->output);
  CHECK(doc);
  char *other = g_build_filename(f->root, "other", NULL);
  CHECK(val_streq(yyjson_obj_get(yyjson_doc_get_root(doc), "cwd"), other));
  const char *legacy = getenv("HERMES_EXPORT_LEGACY");
  if (legacy && *legacy) {
    yyjson_doc *again_before = doc;
    p = run_export(f, true, NULL, 0, NULL);
    CHECK(expect_code(&p, 0, false));
    proc_free(&p);
    yyjson_doc *again = read_json_file(f->output);
    CHECK(again && json_equal(yyjson_doc_get_root(again_before),
                              yyjson_doc_get_root(again)));
    yyjson_doc_free(again);
  }
  yyjson_doc_free(doc);
  g_free(other);
  g_free(target);
}
static void test_cli(void) {
  test_name = "CLI misuse";
  Fix *f = setup_export();
  CHECK(f && f->root);
  const char *bad0[] = {NULL};
  Proc p = run_export(f, false, bad0, 0, NULL);
  CHECK(expect_code(&p, 2, false));
  proc_free(&p);
  const char *bad1[] = {f->home};
  p = run_export(f, false, bad1, 1, NULL);
  CHECK(expect_code(&p, 2, false));
  proc_free(&p);
  const char *bad3[] = {f->home, f->output, "extra"};
  p = run_export(f, false, bad3, 3, NULL);
  CHECK(expect_code(&p, 2, false));
  proc_free(&p);
  const char *bad[] = {"--bad"};
  p = run_export(f, false, bad, 1, NULL);
  CHECK(expect_code(&p, 2, false));
  proc_free(&p);
  const char *helps[] = {"-h", "--help", "--h"};
  for (int i = 0; i < 3; i++) {
    const char *one[] = {helps[i]};
    p = run_export(f, false, one, 1, NULL);
    CHECK(expect_code(&p, 0, false));
    proc_free(&p);
  }
  char *sops = g_build_filename(f->commands, "sops", NULL);
  unlink(sops);
  g_free(sops);
  char **env = env_copy(f->env);
  env_set(&env, "PATH", f->commands);
  p = run_export(f, false, NULL, 0, env);
  env_free(env);
  CHECK(expect_code(&p, 1, false));
  CHECK(p.out_len == 0);
  CHECK(!g_file_test(f->output, G_FILE_TEST_EXISTS));
  proc_free(&p);
}
static char *trim_copy(const char *text, size_t n) {
  size_t start = 0;
  while (start < n &&
         (text[start] == ' ' || text[start] == '\n' || text[start] == '\r' ||
          text[start] == '\t'))
    start++;
  while (n > start && (text[n - 1] == ' ' || text[n - 1] == '\n' ||
                       text[n - 1] == '\r' || text[n - 1] == '\t'))
    n--;
  char *out = g_malloc(n - start + 1);
  memcpy(out, text + start, n - start);
  out[n - start] = 0;
  return out;
}
static void test_sops_roundtrip(void) {
  test_name = "real SOPS encryption";
  Fix *f = setup_export();
  CHECK(f && f->root);
  char *key = g_build_filename(f->root, "age.key", NULL);
  char *argv1[] = {"age-keygen", "-o", key, NULL};
  Proc age = proc_run("age-keygen", argv1, NULL, NULL, 15000);
  CHECK(expect_code(&age, 0, false));
  proc_free(&age);
  char *argv2[] = {"age-keygen", "-y", key, NULL};
  Proc pub = proc_run("age-keygen", argv2, NULL, NULL, 15000);
  CHECK(expect_code(&pub, 0, false));
  char *recipient = trim_copy(pub.out, pub.out_len);
  proc_free(&pub);
  char *policy = g_strdup_printf("creation_rules:\n  - path_regex: .*\n    age: %s\n",
                                 recipient);
  char *policy_path = g_build_filename(f->repo, ".sops.yaml", NULL);
  write_file(policy_path, policy, strlen(policy), 0600);
  g_free(policy_path);
  g_free(policy);
  char *auth = g_build_filename(f->home, "auth.json", NULL);
  write_file(auth, "fixture-private-auth-token", 25, 0600);
  g_free(auth);
  char **env = env_blank();
  env_set(&env, "PATH", g_getenv("PATH") ? g_getenv("PATH") : "");
  env_set(&env, "HOME", f->root);
  env_set(&env, "SOPS_AGE_KEY_FILE", key);
  char *bin = export_bin();
  char *argv[] = {bin, f->home, f->output, NULL};
  Proc p = proc_run(bin, argv, NULL, env, 15000);
  CHECK(expect_code(&p, 0, false));
  CHECK(p.err_len == 0);
  proc_free(&p);
  size_t n = 0;
  char *encrypted = read_file(f->output, &n);
  CHECK(encrypted);
  CHECK(mem_has(encrypted, n, "ENC[AES256_GCM"));
  CHECK(!mem_has(encrypted, n, "fixture-private-auth-token"));
  CHECK(!mem_has(encrypted, n, "Zml4dHVyZS1wcml2YXRlLWF1dGgtdG9rZW4="));
  g_free(encrypted);
  char *dargs[] = {"sops", "--decrypt", f->output, NULL};
  Proc dec = proc_run("sops", dargs, NULL, env, 15000);
  CHECK(expect_code(&dec, 0, false));
  yyjson_doc *data = yyjson_read(dec.out, dec.out_len, 0);
  CHECK(data);
  CHECK(yyjson_get_sint(yyjson_obj_get(yyjson_doc_get_root(data), "version")) == 1);
  yyjson_val *seed =
      yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(data), "seed_files"), 0);
  gsize dn = 0;
  guchar *decoded =
      g_base64_decode(yyjson_get_str(yyjson_obj_get(seed, "content")), &dn);
  CHECK(dn == 25 && decoded && !memcmp(decoded, "fixture-private-auth-token", 25));
  g_free(decoded);
  struct stat st;
  CHECK(stat(key, &st) == 0 && (st.st_mode & 077) == 0);
  yyjson_doc_free(data);
  proc_free(&dec);
  env_free(env);
  g_free(bin);
  g_free(recipient);
  g_free(key);
}

static const char *managed_python(void) {
  const char *py = getenv("HERMES_MANAGED_PYTHON");
  return py && *py ? py : "python3";
}
static Proc run_managed(const char *kind, const char **args, int nargs, bool old) {
  GPtrArray *av = g_ptr_array_new_with_free_func(g_free);
  if (old) {
    g_ptr_array_add(av, g_strdup(managed_python()));
    const char *file =
        !strcmp(kind, "declaration") ? "hermes_declaration.py" : "hermes_profile.py";
    g_ptr_array_add(
        av, g_build_filename(getenv("HERMES_MANAGED_LEGACY"), file, NULL));
  } else {
    char *name = g_strconcat("hermes-", kind, NULL);
    g_ptr_array_add(av, managed_bin(name));
    g_free(name);
  }
  for (int i = 0; i < nargs; i++)
    g_ptr_array_add(av, g_strdup(args[i]));
  g_ptr_array_add(av, NULL);
  Proc p = proc_run(av->pdata[0], (char **)av->pdata, NULL, NULL, 35000);
  g_ptr_array_free(av, TRUE);
  return p;
}
static bool yaml_equals(const char *path, const char *expected) {
  const char *yq = getenv("HERMES_YQ");
  if (!yq || !*yq)
    yq = "yq";
  char *argv[] = {(char *)yq, "-o=json", ".", (char *)path, NULL};
  Proc p = proc_run(yq, argv, NULL, NULL, 15000);
  if (p.code != 0) {
    fprintf(stderr, "yq failed: %s\n", p.err ? p.err : "");
    proc_free(&p);
    return false;
  }
  yyjson_doc *got = yyjson_read(p.out, p.out_len, 0);
  yyjson_doc *exp = yyjson_read(expected, strlen(expected), 0);
  bool ok = got && exp && json_equal(yyjson_doc_get_root(got), yyjson_doc_get_root(exp));
  if (!ok)
    fprintf(stderr, "yaml got: %s\nwant: %s\n", p.out ? p.out : "", expected);
  yyjson_doc_free(got);
  yyjson_doc_free(exp);
  proc_free(&p);
  return ok;
}
static char *file_obj(const char *path, const void *content, size_t len, bool exe) {
  char *b64 = g_base64_encode(content, len);
  char *obj = g_strdup_printf(
      "{\"path\":\"%s\",\"content\":\"%s\",\"executable\":%s}", path, b64,
      exe ? "true" : "false");
  g_free(b64);
  return obj;
}
static char *decl_of(char **files, size_t nfiles, const char *seeds,
                     const char *jobs) {
  GString *s = g_string_new("{\"version\":1,\"files\":[");
  for (size_t i = 0; i < nfiles; i++) {
    if (i)
      g_string_append_c(s, ',');
    g_string_append(s, files[i]);
  }
  g_string_append_printf(s, "],\"seed_files\":%s,\"jobs\":%s}",
                         seeds ? seeds : "[]", jobs ? jobs : "[]");
  return g_string_free(s, FALSE);
}
static Proc apply_decl(const char *root, const char *json, bool old,
                       const char **extra, int nextra) {
  char *path = g_build_filename(root, "declaration.json", NULL);
  char *home = g_build_filename(root, "home", NULL);
  write_file(path, json, strlen(json), 0600);
  char *owned_bin = NULL, *owned_script = NULL;
  GPtrArray *av = g_ptr_array_new();
  if (old) {
    g_ptr_array_add(av, (gpointer)managed_python());
    owned_script = g_build_filename(getenv("HERMES_MANAGED_LEGACY"),
                                    "hermes_declaration.py", NULL);
    g_ptr_array_add(av, owned_script);
  } else {
    owned_bin = managed_bin("hermes-declaration");
    g_ptr_array_add(av, owned_bin);
  }
  g_ptr_array_add(av, path);
  g_ptr_array_add(av, home);
  for (int i = 0; i < nextra; i++)
    g_ptr_array_add(av, (gpointer)extra[i]);
  g_ptr_array_add(av, NULL);
  Proc p = proc_run(av->pdata[0], (char **)av->pdata, NULL, NULL, 35000);
  g_free(owned_bin);
  g_free(owned_script);
  g_free(path);
  g_free(home);
  g_ptr_array_free(av, FALSE);
  return p;
}
static bool legacy_managed(void) {
  const char *legacy = getenv("HERMES_MANAGED_LEGACY");
  return legacy && *legacy;
}
static void test_profile_overlay(void) {
  test_name = "profile overlays";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *p = g_build_filename(root, "profile.yaml", NULL);
    char *o = g_build_filename(root, "overlay.json", NULL);
    const char *before =
        "display:\n  theme: custom\n  streaming: false\ncustom_providers:\n  - "
        "name: other\n    api_key: retained\n  - name: qwen-local\n    base_url: "
        "old\n    options: {keep: true}\n";
    write_file(p, before, strlen(before), 0600);
    const char *overlay =
        "{\"display\":{\"streaming\":true},\"custom_providers\":[{\"name\":"
        "\"qwen-local\",\"base_url\":\"http://127.0.0.1:8081/v1\"},{\"name\":"
        "\"new\",\"enabled\":true}]}";
    write_file(o, overlay, strlen(overlay), 0600);
    const char *args[] = {p, o};
    Proc r = run_managed("profile", args, 2, old == 1);
    CHECK(expect_code(&r, 0, true));
    CHECK(r.out && strstr(r.out, "Updated Qwen profile"));
    CHECK(yaml_equals(
        p,
        "{\"display\":{\"theme\":\"custom\",\"streaming\":true},"
        "\"custom_providers\":[{\"name\":\"other\",\"api_key\":\"retained\"},{"
        "\"name\":\"qwen-local\",\"base_url\":\"http://127.0.0.1:8081/v1\","
        "\"options\":{\"keep\":true}},{\"name\":\"new\",\"enabled\":true}]}"));
    proc_free(&r);
    g_free(p);
    g_free(o);
  }
}
static int count_prefix(const char *dir, const char *prefix) {
  GDir *d = g_dir_open(dir, 0, NULL);
  if (!d)
    return -1;
  int n = 0;
  const char *name;
  while ((name = g_dir_read_name(d)))
    if (g_str_has_prefix(name, prefix))
      n++;
  g_dir_close(d);
  return n;
}
static char *first_prefix(const char *dir, const char *prefix) {
  GDir *d = g_dir_open(dir, 0, NULL);
  if (!d)
    return NULL;
  char *found = NULL;
  const char *name;
  while ((name = g_dir_read_name(d)))
    if (g_str_has_prefix(name, prefix)) {
      found = g_strdup(name);
      break;
    }
  g_dir_close(d);
  return found;
}
static void test_profile_backup(void) {
  test_name = "profile backups";
  int rounds = legacy_managed() ? 2 : 1;
  const char *before =
      "# keep this in backup\nagent: {max_turns: 90}\ncompression: {enabled: false}\n";
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *p = g_build_filename(root, "profile.yaml", NULL);
    char *o = g_build_filename(root, "overlay.json", NULL);
    write_file(p, before, strlen(before), 0600);
    const char *overlay =
        "{\"agent\":{\"max_turns\":\"unlimited\"},\"compression\":{\"enabled\":true}}";
    write_file(o, overlay, strlen(overlay), 0600);
    const char *args[] = {p, o};
    Proc r = run_managed("profile", args, 2, old == 1);
    CHECK(expect_code(&r, 0, true));
    proc_free(&r);
    CHECK(count_prefix(root, "profile.yaml.before-local-llm-") == 1);
    char *name = first_prefix(root, "profile.yaml.before-local-llm-");
    char *backup = g_build_filename(root, name, NULL);
    CHECK(file_eq(backup, before, strlen(before)));
    CHECK(mode_is(backup, 0600));
    CHECK(mode_is(p, 0600));
    ino_t ino = inode_of(p);
    size_t n = 0;
    char *data = read_file(p, &n);
    r = run_managed("profile", args, 2, old == 1);
    CHECK(expect_code(&r, 0, true));
    CHECK(r.out_len == 0);
    CHECK(inode_of(p) == ino);
    CHECK(file_eq(p, data, n));
    CHECK(count_prefix(root, ".local-llm-") == 0);
    proc_free(&r);
    g_free(data);
    g_free(backup);
    g_free(name);
    g_free(p);
    g_free(o);
  }
}
static void test_yaml_scalars(void) {
  test_name = "YAML scalar types";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *p = g_build_filename(root, "profile.yaml", NULL);
    char *o = g_build_filename(root, "overlay.json", NULL);
    const char *before =
        "base: &base {enabled: yes, retries: 0x10, quoted: \"012\", empty: null}\n"
        "copy: *base\nmerged:\n  <<: *base\n  retries: 4\ntext: \"false\"\nfloat: 1.5\n";
    write_file(p, before, strlen(before), 0600);
    write_file(o, "{\"other\":true}", 14, 0600);
    const char *args[] = {p, o};
    Proc r = run_managed("profile", args, 2, old == 1);
    CHECK(expect_code(&r, 0, true));
    CHECK(yaml_equals(
        p,
        "{\"base\":{\"enabled\":true,\"retries\":16,\"quoted\":\"012\",\"empty\":null},"
        "\"copy\":{\"enabled\":true,\"retries\":16,\"quoted\":\"012\",\"empty\":null},"
        "\"merged\":{\"enabled\":true,\"retries\":4,\"quoted\":\"012\",\"empty\":null},"
        "\"text\":\"false\",\"float\":1.5,\"other\":true}"));
    proc_free(&r);
    g_free(p);
    g_free(o);
  }
}
static void test_yaml_numbers(void) {
  test_name = "YAML 1.1 number forms";
  char *root = temp_dir("hermes-managed-");
  CHECK(root);
  char *p = g_build_filename(root, "profile.yaml", NULL);
  char *o = g_build_filename(root, "overlay.json", NULL);
  const char *before =
      "octal: 012\nhex: 0x10\nbase60: 1:20\nfloat60: 1:20.5\nexponent: 1.0e+3\n"
      "notExponent: 1.0e3\nnotOctal: 08\nquoted: \"012\"\n";
  write_file(p, before, strlen(before), 0600);
  write_file(o, "{\"other\":true}", 14, 0600);
  const char *args[] = {p, o};
  Proc r = run_managed("profile", args, 2, false);
  CHECK(expect_code(&r, 0, true));
  CHECK(yaml_equals(
      p,
      "{\"octal\":10,\"hex\":16,\"base60\":80,\"float60\":80.5,\"exponent\":1000,"
      "\"notExponent\":\"1.0e3\",\"notOctal\":\"08\",\"quoted\":\"012\",\"other\":true}"));
  proc_free(&r);
  g_free(p);
  g_free(o);
}
static void test_runtime_keys(void) {
  test_name = "runtime keys";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *p = g_build_filename(root, "deep/profile.yaml", NULL);
    char *o = g_build_filename(root, "overlay.json", NULL);
    char *k = g_build_filename(root, "key", NULL);
    const char *overlay =
        "{\"model\":{\"provider\":\"qwen-local\"},\"custom_providers\":[{\"name\":"
        "\"other\",\"api_key\":\"retained\"},{\"name\":\"qwen-local\"}]}";
    write_file(o, overlay, strlen(overlay), 0600);
    const char key[] = "\vfixture-runtime-key\xC2\x85\n";
    write_file(k, key, sizeof key - 1, 0600);
    const char *args[] = {p, o, "--key-file", k};
    Proc r = run_managed("profile", args, 4, old == 1);
    CHECK(expect_code(&r, 0, true));
    proc_free(&r);
    const char *yq = getenv("HERMES_YQ");
    if (!yq || !*yq)
      yq = "yq";
    char *argv[] = {(char *)yq, "-o=json", ".", p, NULL};
    Proc y = proc_run(yq, argv, NULL, NULL, 15000);
    CHECK(expect_code(&y, 0, true));
    yyjson_doc *doc = yyjson_read(y.out, y.out_len, 0);
    CHECK(doc);
    yyjson_val *v = yyjson_doc_get_root(doc);
    CHECK(val_streq(yyjson_obj_get(yyjson_obj_get(v, "model"), "api_key"),
                    "fixture-runtime-key"));
    CHECK(val_streq(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(v, "custom_providers"), 1),
                                   "api_key"),
                    "fixture-runtime-key"));
    CHECK(val_streq(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(v, "custom_providers"), 0),
                                   "api_key"),
                    "retained"));
    size_t on = 0;
    char *otext = read_file(o, &on);
    CHECK(otext && !mem_has(otext, on, "fixture-runtime-key"));
    g_free(otext);
    yyjson_doc_free(doc);
    proc_free(&y);
    write_file(k, " \n", 2, 0600);
    size_t before_n = 0;
    char *before = read_file(p, &before_n);
    r = run_managed("profile", args, 4, old == 1);
    CHECK(expect_code(&r, 1, true));
    CHECK(file_eq(p, before, before_n));
    proc_free(&r);
    g_free(before);
    g_free(p);
    g_free(o);
    g_free(k);
  }
}
static void test_profile_unsafe(void) {
  test_name = "profile symlinks and unsafe YAML";
  char *root = temp_dir("hermes-managed-");
  CHECK(root);
  char *p = g_build_filename(root, "profile.yaml", NULL);
  char *o = g_build_filename(root, "overlay.json", NULL);
  char *target = g_build_filename(root, "target", NULL);
  write_file(target, "keep", 4, 0600);
  CHECK(symlink(target, p) == 0);
  write_file(o, "{}", 2, 0600);
  const char *args[] = {p, o};
  Proc r = run_managed("profile", args, 2, false);
  CHECK(expect_code(&r, 1, true));
  CHECK(file_eq(target, "keep", 4));
  proc_free(&r);
  unlink(p);
  const char *tag = "!!python/object/apply:os.system [PRIVATE-SENTINEL]";
  write_file(p, tag, strlen(tag), 0600);
  r = run_managed("profile", args, 2, false);
  CHECK(expect_code(&r, 1, true));
  CHECK(file_eq(p, tag, strlen(tag)));
  proc_free(&r);
  g_free(p);
  g_free(o);
  g_free(target);
}
static void test_fresh_decl(void) {
  test_name = "fresh declaration";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *home = g_build_filename(root, "home", NULL);
    char *a = file_obj("config.yaml", "model: fixture\n", 14, false);
    char *b = file_obj("scripts/run.sh", "#!/bin/sh\necho fixture\n", 22, true);
    char *files[] = {a, b};
    char *seed = file_obj("auth.json", "initial-oauth", 13, false);
    char *seeds = g_strdup_printf("[%s]", seed);
    char *json = decl_of(files, 2, seeds, "[]");
    char *history = g_build_filename(home, "sessions/history", NULL);
    write_file(history, "keep", 4, 0600);
    Proc r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 0, true));
    proc_free(&r);
    CHECK(mode_is(home, 0700));
    char *config = g_build_filename(home, "config.yaml", NULL);
    char *script = g_build_filename(home, "scripts/run.sh", NULL);
    CHECK(mode_is(config, 0600));
    CHECK(mode_is(script, 0700));
    char *managed = g_build_filename(home, ".managed", NULL);
    char *qmanaged = g_build_filename(home, "profiles/qwen/.managed", NULL);
    CHECK(file_eq(managed, "home-manager\n", 13));
    CHECK(file_eq(qmanaged, "home-manager\n", 13));
    ino_t ino = inode_of(config);
    char *auth = g_build_filename(home, "auth.json", NULL);
    write_file(auth, "refreshed-oauth", 15, 0600);
    r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 0, true));
    CHECK(inode_of(config) == ino);
    CHECK(file_eq(auth, "refreshed-oauth", 15));
    CHECK(file_eq(history, "keep", 4));
    proc_free(&r);
    g_free(a);
    g_free(b);
    g_free(seed);
    g_free(seeds);
    g_free(json);
    g_free(history);
    g_free(config);
    g_free(script);
    g_free(managed);
    g_free(qmanaged);
    g_free(auth);
    g_free(home);
  }
}
static void test_schedules(void) {
  test_name = "equal schedules retain history";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *home = g_build_filename(root, "home", NULL);
    const char *existing =
        "{\"extra\":\"keep\",\"jobs\":[{\"id\":\"a\",\"schedule\":{\"kind\":\"cron\","
        "\"expression\":\"old\"},\"last_run_at\":\"yesterday\",\"failure_streak\":3,"
        "\"repeat\":{\"completed\":7},\"state\":\"completed\"},{\"id\":\"gone\","
        "\"schedule\":{\"kind\":\"once\"}}]}";
    char *jobs = g_build_filename(home, "cron/jobs.json", NULL);
    write_file(jobs, existing, strlen(existing), 0600);
    char *file = file_obj("config.yaml", "model: fixture\n", 14, false);
    char *files[] = {file};
    const char *declared =
        "[{\"id\":\"a\",\"schedule\":{\"kind\":\"cron\",\"expression\":\"old\"},"
        "\"enabled\":true,\"repeat\":{\"times\":9,\"completed\":0}}]";
    char *json = decl_of(files, 1, "[]", declared);
    Proc r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 0, true));
    proc_free(&r);
    yyjson_doc *doc = read_json_file(jobs);
    CHECK(doc);
    yyjson_val *rootv = yyjson_doc_get_root(doc);
    CHECK(val_streq(yyjson_obj_get(rootv, "extra"), "keep"));
    CHECK(json_text_eq(
        yyjson_obj_get(rootv, "jobs"),
        "[{\"id\":\"a\",\"schedule\":{\"kind\":\"cron\",\"expression\":\"old\"},"
        "\"enabled\":false,\"repeat\":{\"times\":9,\"completed\":7},\"last_run_at\":"
        "\"yesterday\",\"failure_streak\":3,\"state\":\"completed\",\"next_run_at\":null}]"));
    yyjson_doc_free(doc);
    g_free(json);
    const char *changed =
        "[{\"id\":\"a\",\"schedule\":{\"kind\":\"cron\",\"expression\":\"new\"},"
        "\"enabled\":true,\"repeat\":{\"times\":9,\"completed\":0}}]";
    json = decl_of(files, 1, "[]", changed);
    r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 0, true));
    proc_free(&r);
    doc = read_json_file(jobs);
    CHECK(doc);
    yyjson_val *fresh =
        yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "jobs"), 0);
    CHECK(!yyjson_obj_get(fresh, "last_run_at"));
    CHECK(yyjson_get_sint(yyjson_obj_get(yyjson_obj_get(fresh, "repeat"), "completed")) ==
          0);
    CHECK(yyjson_get_bool(yyjson_obj_get(fresh, "enabled")));
    yyjson_doc_free(doc);
    g_free(json);
    g_free(file);
    g_free(jobs);
    g_free(home);
  }
}
static char *sha256_hex(const char *text) {
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(sum, (const guchar *)text, strlen(text));
  char *hex = g_strdup(g_checksum_get_string(sum));
  g_checksum_free(sum);
  return hex;
}
static void test_file_backups(void) {
  test_name = "changed and stale declared files";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *home = g_build_filename(root, "home", NULL);
    char *soul = g_build_filename(home, "SOUL.md", NULL);
    write_file(soul, "old soul", 8, 0600);
    char *a = file_obj("SOUL.md", "new soul", 8, false);
    char *b = file_obj("skills/stale.md", "stale", 5, false);
    char *files[] = {a, b};
    char *json = decl_of(files, 2, "[]", "[]");
    Proc r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 0, true));
    proc_free(&r);
    char *hash = sha256_hex("old soul");
    char *backup = g_build_filename(home, "backups/home-manager", hash, NULL);
    CHECK(file_eq(backup, "old soul", 8));
    CHECK(mode_is(backup, 0600));
    g_free(json);
    json = decl_of(files, 1, "[]", "[]");
    r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 0, true));
    proc_free(&r);
    char *stale = g_build_filename(home, "skills/stale.md", NULL);
    CHECK(!g_file_test(stale, G_FILE_TEST_EXISTS));
    char *shash = sha256_hex("stale");
    char *sbackup = g_build_filename(home, "backups/home-manager", shash, NULL);
    CHECK(file_eq(sbackup, "stale", 5));
    char *index = g_build_filename(home, ".home-manager-files.json", NULL);
    yyjson_doc *doc = read_json_file(index);
    CHECK(doc && json_text_eq(yyjson_doc_get_root(doc), "[\"SOUL.md\"]"));
    yyjson_doc_free(doc);
    g_free(json);
    g_free(a);
    g_free(b);
    g_free(soul);
    g_free(backup);
    g_free(hash);
    g_free(stale);
    g_free(shash);
    g_free(sbackup);
    g_free(index);
    g_free(home);
  }
}
static void bad_path_case(const char *bad, bool old) {
  char *root = temp_dir("hermes-managed-");
  if (!root) {
    fail(__LINE__, "temp");
    return;
  }
  char *home = g_build_filename(root, "home", NULL);
  char *soul = g_build_filename(home, "SOUL.md", NULL);
  write_file(soul, "keep", 4, 0600);
  char *a = file_obj("SOUL.md", "replace", 7, false);
  char *b = file_obj(bad, "bad", 3, false);
  char *files[] = {a, b};
  char *json = decl_of(files, 2, "[]", "[]");
  Proc r = apply_decl(root, json, old, NULL, 0);
  if (!expect_code(&r, 1, true) || !file_eq(soul, "keep", 4)) {
    fprintf(stderr, "bad path %s\n", bad);
    fail(__LINE__, bad);
  }
  proc_free(&r);
  g_free(json);
  g_free(a);
  g_free(b);
  g_free(soul);
  g_free(home);
}
static void test_bad_paths(void) {
  test_name = "paths and invalid payloads";
  const char *bads[] = {"../outside", "/tmp/escaped", "sessions/data",
                        "cron/jobs.json", "profiles/other/config.yaml"};
  int rounds = legacy_managed() ? 2 : 1;
  for (size_t i = 0; i < 5; i++)
    for (int old = 0; old < rounds; old++) {
      bad_path_case(bads[i], old == 1);
      if (failures)
        return;
    }
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *home = g_build_filename(root, "home", NULL);
    char *soul = g_build_filename(home, "SOUL.md", NULL);
    write_file(soul, "keep", 4, 0600);
    char *skills = g_build_filename(home, "skills", NULL);
    CHECK(symlink(root, skills) == 0);
    char *a = file_obj("SOUL.md", "replace", 7, false);
    char *b = file_obj("skills/x", "bad", 3, false);
    char *files[] = {a, b};
    char *json = decl_of(files, 2, "[]", "[]");
    Proc r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 1, true));
    CHECK(file_eq(soul, "keep", 4));
    proc_free(&r);
    unlink(skills);
    g_free(json);
    char *jobs =
        "[{\"id\":\"same\"},{\"id\":\"same\"}]";
    json = decl_of(files, 1, "[]", jobs);
    r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 1, true));
    CHECK(file_eq(soul, "keep", 4));
    proc_free(&r);
    g_free(json);
    json = g_strdup(
        "{\"version\":1,\"files\":[{\"path\":\"SOUL.md\",\"content\":\"not base64!\","
        "\"executable\":false}],\"seed_files\":[],\"jobs\":[]}");
    r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 1, true));
    proc_free(&r);
    g_free(json);
    g_free(a);
    g_free(b);
    g_free(skills);
    g_free(soul);
    g_free(home);
  }
}
static void test_duplicate_paths(void) {
  test_name = "duplicate normalized file paths";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *a = file_obj("SOUL.md", "first", 5, false);
    char *b = file_obj("./SOUL.md", "last", 4, false);
    char *files[] = {a, b};
    char *json = decl_of(files, 2, "[]", "[]");
    Proc r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 0, true));
    char *soul = g_build_filename(root, "home/SOUL.md", NULL);
    CHECK(file_eq(soul, "last", 4));
    proc_free(&r);
    g_free(soul);
    g_free(json);
    g_free(a);
    g_free(b);
  }
}
static void test_index_guard(void) {
  test_name = "managed-file index";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *home = g_build_filename(root, "home", NULL);
    char *soul = g_build_filename(home, "SOUL.md", NULL);
    write_file(soul, "keep", 4, 0600);
    char *index = g_build_filename(home, ".home-manager-files.json", NULL);
    write_file(index, "[\"sessions/history\"]", 20, 0600);
    char *history = g_build_filename(home, "sessions/history", NULL);
    write_file(history, "runtime", 7, 0600);
    char *file = file_obj("SOUL.md", "changed", 7, false);
    char *files[] = {file};
    char *json = decl_of(files, 1, "[]", "[]");
    Proc r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 1, true));
    CHECK(file_eq(soul, "keep", 4));
    CHECK(file_eq(history, "runtime", 7));
    proc_free(&r);
    write_file(index, "[]", 2, 0600);
    char *jobs = g_build_filename(home, "cron/jobs.json", NULL);
    write_file(jobs, "{invalid", 8, 0600);
    r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 1, true));
    CHECK(file_eq(soul, "keep", 4));
    proc_free(&r);
    g_free(json);
    g_free(file);
    g_free(jobs);
    g_free(history);
    g_free(index);
    g_free(soul);
    g_free(home);
  }
}
static bool hex64(const char *text) {
  if (!text || strlen(text) != 64)
    return false;
  for (int i = 0; i < 64; i++) {
    char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}
static void qwen_case(const char *which, bool old) {
  char *root = temp_dir("hermes-managed-");
  if (!root) {
    fail(__LINE__, "temp");
    return;
  }
  char *home = g_build_filename(root, "home", NULL);
  char *overlay = g_build_filename(root, "overlay.json", NULL);
  char *key = g_build_filename(root, "local/api-key", NULL);
  const char *initial = !strcmp(which, "declared")
                            ? "model: {api_key: declared-key}\n"
                            : "model: {}\n";
  char *file = file_obj("profiles/qwen/config.yaml", initial, strlen(initial), false);
  char *files[] = {file};
  char *json = decl_of(files, 1, "[]", "[]");
  const char *over =
      "{\"model\":{\"default\":\"new-model\"},\"custom_providers\":[{\"name\":\"qwen-local\"}]}";
  write_file(overlay, over, strlen(over), 0600);
  if (!strcmp(which, "runtime"))
    write_file(key, " runtime-key\n", 13, 0600);
  const char *extra[] = {"--qwen-overlay", overlay, "--key-file", key};
  Proc r = apply_decl(root, json, old, extra, 4);
  if (!expect_code(&r, 0, true)) {
    fail(__LINE__, which);
    proc_free(&r);
    goto done;
  }
  proc_free(&r);
  char *config = g_build_filename(home, "profiles/qwen/config.yaml", NULL);
  const char *yq = getenv("HERMES_YQ");
  if (!yq || !*yq)
    yq = "yq";
  char *argv[] = {(char *)yq, "-o=json", ".", config, NULL};
  Proc y = proc_run(yq, argv, NULL, NULL, 15000);
  size_t kn = 0;
  char *raw_file = read_file(key, &kn);
  char *raw = raw_file ? trim_copy(raw_file, kn) : NULL;
  yyjson_doc *doc = y.out ? yyjson_read(y.out, y.out_len, 0) : NULL;
  yyjson_val *v = doc ? yyjson_doc_get_root(doc) : NULL;
  bool ok = expect_code(&y, 0, true) && v && raw &&
            val_streq(yyjson_obj_get(yyjson_obj_get(v, "model"), "api_key"), raw) &&
            val_streq(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(v, "custom_providers"), 0),
                                     "api_key"),
                      raw);
  if (!strcmp(which, "runtime"))
    ok = ok && !strcmp(raw, "runtime-key");
  if (!strcmp(which, "declared"))
    ok = ok && !strcmp(raw, "declared-key");
  if (!strcmp(which, "generated"))
    ok = ok && hex64(raw) && mode_is(key, 0600);
  if (!ok) {
    fprintf(stderr, "qwen %s raw=%s yaml=%s\n", which, raw ? raw : "",
            y.out ? y.out : "");
    fail(__LINE__, which);
  }
  proc_free(&y);
  size_t before_n = 0;
  char *before = read_file(config, &before_n);
  r = apply_decl(root, json, old, extra, 4);
  if (!expect_code(&r, 0, true) || !file_eq(config, before, before_n))
    fail(__LINE__, "qwen idempotent");
  proc_free(&r);
  g_free(before);
  yyjson_doc_free(doc);
  g_free(raw);
  g_free(raw_file);
  g_free(config);
done:
  g_free(json);
  g_free(file);
  g_free(key);
  g_free(overlay);
  g_free(home);
}
static void test_qwen(void) {
  test_name = "Qwen overlays";
  const char *which[] = {"runtime", "declared", "generated"};
  int rounds = legacy_managed() ? 2 : 1;
  for (size_t i = 0; i < 3; i++)
    for (int old = 0; old < rounds; old++) {
      qwen_case(which[i], old == 1);
      if (failures)
        return;
    }
}
static void test_telegram(void) {
  test_name = "telegram route";
  int rounds = legacy_managed() ? 2 : 1;
  for (int old = 0; old < rounds; old++) {
    char *root = temp_dir("hermes-managed-");
    CHECK(root);
    char *home = g_build_filename(root, "home", NULL);
    char *house = g_build_filename(home, "house-telegram.json", NULL);
    const char *house_text = "{\"group_id\":-1001,\"topics\":{\"Qwen\":787}}";
    write_file(house, house_text, strlen(house_text), 0600);
    char *file = file_obj("config.yaml", "model: fixture\n", 14, false);
    char *files[] = {file};
    char *json = decl_of(files, 1, "[]", "[]");
    Proc r = apply_decl(root, json, old == 1, NULL, 0);
    CHECK(expect_code(&r, 0, true));
    proc_free(&r);
    char *config = g_build_filename(home, "config.yaml", NULL);
    const char *yq = getenv("HERMES_YQ");
    if (!yq || !*yq)
      yq = "yq";
    char *argv[] = {(char *)yq, "-o=json", ".", config, NULL};
    Proc y = proc_run(yq, argv, NULL, NULL, 15000);
    CHECK(expect_code(&y, 0, true));
    yyjson_doc *doc = yyjson_read(y.out, y.out_len, 0);
    CHECK(doc);
    yyjson_val *v = yyjson_doc_get_root(doc);
    CHECK(yyjson_get_bool(yyjson_obj_get(v, "multiplex_profiles")));
    CHECK(json_text_eq(
        yyjson_obj_get(v, "profile_routes"),
        "[{\"name\":\"telegram-qwen\",\"platform\":\"telegram\",\"chat_id\":\"-1001\","
        "\"thread_id\":\"787\",\"profile\":\"qwen\",\"enabled\":true}]"));
    CHECK(json_equal(yyjson_obj_get(v, "profile_routes"),
                     yyjson_obj_get(yyjson_obj_get(v, "gateway"), "profile_routes")));
    proc_free(&y);
    yyjson_doc_free(doc);
    if (old == 0) {
      ino_t ino = inode_of(config);
      const char *args[] = {config, "--house", house};
      r = run_managed("telegram-route", args, 3, false);
      CHECK(expect_code(&r, 0, true));
      CHECK(r.out_len == 0);
      CHECK(inode_of(config) == ino);
      proc_free(&r);
      write_file(house, "{\"group_id\":-1001,\"topics\":{}}", 30, 0600);
      r = run_managed("telegram-route", args, 3, false);
      CHECK(expect_code(&r, 0, true));
      proc_free(&r);
      y = proc_run(yq, argv, NULL, NULL, 15000);
      CHECK(expect_code(&y, 0, true));
      doc = yyjson_read(y.out, y.out_len, 0);
      CHECK(doc);
      v = yyjson_doc_get_root(doc);
      CHECK(yyjson_arr_size(yyjson_obj_get(v, "profile_routes")) == 0);
      CHECK(yyjson_arr_size(yyjson_obj_get(yyjson_obj_get(v, "gateway"),
                                           "profile_routes")) == 0);
      yyjson_doc_free(doc);
      proc_free(&y);
    }
    g_free(json);
    g_free(file);
    g_free(config);
    g_free(house);
    g_free(home);
  }
}
static void test_gateway_shape(void) {
  test_name = "invalid gateway shape";
  char *root = temp_dir("hermes-managed-");
  CHECK(root);
  char *p = g_build_filename(root, "profile.yaml", NULL);
  char *o = g_build_filename(root, "overlay.json", NULL);
  char *key = g_build_filename(root, "key", NULL);
  write_file(p, "model: PRIVATE-SENTINEL\n", 23, 0600);
  write_file(o, "{\"model\":{\"provider\":\"qwen\"}}", 29, 0600);
  write_file(key, "fixture-key", 11, 0600);
  const char *args[] = {p, o, "--key-file", key};
  Proc r = run_managed("profile", args, 4, false);
  CHECK(expect_code(&r, 0, true));
  proc_free(&r);
  write_file(p, "gateway: [PRIVATE-SENTINEL]\n", 28, 0600);
  char *house = g_build_filename(root, "house.json", NULL);
  write_file(house, "{\"group_id\":\"1\",\"topics\":{\"Qwen\":2}}", 38, 0600);
  size_t n = 0;
  char *before = read_file(p, &n);
  const char *targs[] = {p, "--house", house};
  r = run_managed("telegram-route", targs, 3, false);
  CHECK(expect_code(&r, 1, true));
  CHECK(file_eq(p, before, n));
  proc_free(&r);
  g_free(before);
  g_free(house);
  g_free(key);
  g_free(o);
  g_free(p);
}
static void test_partial(void) {
  test_name = "later filesystem failure";
  char *root = temp_dir("hermes-managed-");
  CHECK(root);
  char *home = g_build_filename(root, "home", NULL);
  char *soul = g_build_filename(home, "SOUL.md", NULL);
  write_file(soul, "old", 3, 0600);
  char *scripts = g_build_filename(home, "scripts", NULL);
  write_file(scripts, "not a directory", 15, 0600);
  char *a = file_obj("SOUL.md", "new", 3, false);
  char *b = file_obj("scripts/file", "content", 7, false);
  char *files[] = {a, b};
  char *json = decl_of(files, 2, "[]", "[]");
  Proc r = apply_decl(root, json, false, NULL, 0);
  CHECK(expect_code(&r, 1, true));
  CHECK(file_eq(soul, "new", 3));
  char *index = g_build_filename(home, ".home-manager-files.json", NULL);
  char *managed = g_build_filename(home, ".managed", NULL);
  CHECK(!g_file_test(index, G_FILE_TEST_EXISTS));
  CHECK(!g_file_test(managed, G_FILE_TEST_EXISTS));
  CHECK(count_prefix(home, ".hermes-managed-") == 0);
  proc_free(&r);
  g_free(json);
  g_free(a);
  g_free(b);
  g_free(index);
  g_free(managed);
  g_free(scripts);
  g_free(soul);
  g_free(home);
}
static bool read_until(int fd, const char *needle, int timeout_ms) {
  GString *buf = g_string_new(NULL);
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  while (!strstr(buf->str, needle)) {
    int remain = (int)((deadline - g_get_monotonic_time()) / 1000);
    if (remain < 0)
      break;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, remain) <= 0)
      break;
    char tmp[64];
    ssize_t n = read(fd, tmp, sizeof tmp);
    if (n <= 0)
      break;
    g_string_append_len(buf, tmp, (gssize)n);
  }
  bool ok = strstr(buf->str, needle) != NULL;
  g_string_free(buf, TRUE);
  return ok;
}
static void test_lock(void) {
  test_name = "scheduler lock";
  char *root = temp_dir("hermes-lock-");
  CHECK(root);
  char *home = g_build_filename(root, "home", NULL);
  char *cron = g_build_filename(home, "cron", NULL);
  g_mkdir_with_parents(cron, 0700);
  char *lock = g_build_filename(home, "cron/.jobs.lock", NULL);
  int outp[2];
  CHECK(pipe(outp) == 0);
  pid_t holder = fork();
  CHECK(holder >= 0);
  if (holder == 0) {
    dup2(outp[1], STDOUT_FILENO);
    close(outp[0]);
    close(outp[1]);
    execlp("flock", "flock", lock, "bash", "-c", "printf ready; sleep 1",
           (char *)NULL);
    _exit(127);
  }
  close(outp[1]);
  CHECK(read_until(outp[0], "ready", 5000));
  close(outp[0]);
  char *file = file_obj("SOUL.md", "written", 7, false);
  char *files[] = {file};
  char *json = decl_of(files, 1, "[]", "[]");
  char *decl = g_build_filename(root, "declaration.json", NULL);
  write_file(decl, json, strlen(json), 0600);
  char *bin = managed_bin("hermes-declaration");
  int errfd = open("/dev/null", O_WRONLY);
  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    dup2(errfd, STDERR_FILENO);
    dup2(errfd, STDOUT_FILENO);
    execl(bin, bin, decl, home, (char *)NULL);
    _exit(127);
  }
  g_usleep(150000);
  char *soul = g_build_filename(home, "SOUL.md", NULL);
  CHECK(!g_file_test(soul, G_FILE_TEST_EXISTS));
  int status = 0;
  gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
  pid_t waited = 0;
  while (g_get_monotonic_time() < deadline) {
    waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
      break;
    g_usleep(50000);
  }
  if (waited != child) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    fail(__LINE__, "declaration timed out");
  } else
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(file_eq(soul, "written", 7));
  kill(holder, SIGKILL);
  waitpid(holder, NULL, 0);
  kill(child, SIGKILL);
  g_free(soul);
  g_free(bin);
  g_free(decl);
  g_free(json);
  g_free(file);
  g_free(lock);
  g_free(cron);
  g_free(home);
  if (errfd >= 0)
    close(errfd);
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--fixture-sops"))
    return fake_sops(argc, argv);
  void (*tests[])(void) = {
      test_capture,       test_sorted,         test_mem0,
      test_symlinks,      test_store,          test_runtime_jobs,
      test_missing,       test_encrypt_fail,   test_relative,
      test_cli,           test_sops_roundtrip, test_profile_overlay,
      test_profile_backup, test_yaml_scalars,  test_yaml_numbers,
      test_runtime_keys,  test_profile_unsafe, test_fresh_decl,
      test_schedules,     test_file_backups,   test_bad_paths,
      test_duplicate_paths, test_index_guard,  test_qwen,
      test_telegram,      test_gateway_shape,  test_partial,
      test_lock};
  for (size_t i = 0; i < G_N_ELEMENTS(tests); i++) {
    int before = failures;
    fprintf(stderr, "RUN %s\n", "hermes");
    tests[i]();
    fprintf(stderr, "%s %s\n", failures == before ? "PASS" : "FAIL", test_name);
  }
  fprintf(stderr, "%d failures\n", failures);
  return failures ? 1 : 0;
}
