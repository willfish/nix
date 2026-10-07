#define _GNU_SOURCE
#include "json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static const char *test_name;
static const char *bins;
static const char *failure_lib;
static const char *legacy;
static const char *python;
static const char *bus_bin;
static const char *bus_argv;
static const char *bus_legacy;
static const char *bus_python;
static Json *wrappers;
static char *self_exe;

static void fail_msg(const char *file, int line, const char *msg) {
  fprintf(stderr, "FAIL %s:%d %s: %s\n", file, line, test_name, msg);
  failures++;
}
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond))                                                               \
      fail_msg(__FILE__, __LINE__, #cond);                                    \
  } while (0)

static char *snippet(const char *text) {
  size_t n = text ? strlen(text) : 0;
  size_t keep = n > 240 ? 240 : n;
  char *out = g_malloc(keep + 1);
  memcpy(out, text ? text : "", keep);
  out[keep] = 0;
  for (size_t i = 0; i < keep; i++)
    if (out[i] == '\n' || out[i] == '\r')
      out[i] = ' ';
  return out;
}

typedef struct {
  pid_t pid;
  int out_fd, err_fd;
  GString *out, *err;
  bool out_open, err_open;
} Child;

static void env_set(GPtrArray *env, const char *key, const char *value) {
  size_t klen = strlen(key);
  for (guint i = 0; i < env->len;) {
    char *item = env->pdata[i];
    if (!strncmp(item, key, klen) && item[klen] == '=') {
      g_free(item);
      g_ptr_array_remove_index(env, i);
    } else
      i++;
  }
  if (value)
    g_ptr_array_add(env, g_strdup_printf("%s=%s", key, value));
}

static char **env_finish(GPtrArray *env) {
  g_ptr_array_add(env, NULL);
  char **out = g_new(char *, env->len);
  for (guint i = 0; i < env->len; i++)
    out[i] = env->pdata[i];
  g_ptr_array_free(env, false);
  return out;
}

static GPtrArray *env_inherited(void) {
  GPtrArray *env = g_ptr_array_new();
  for (char **item = environ; item && *item; item++)
    g_ptr_array_add(env, g_strdup(*item));
  return env;
}

static void env_free(char **env) {
  if (!env)
    return;
  for (char **item = env; *item; item++)
    g_free(*item);
  g_free(env);
}

static bool spawn_child(Child *child, const char *cwd, char **argv, char **env) {
  int outp[2], errp[2];
  memset(child, 0, sizeof *child);
  if (pipe(outp) != 0 || pipe(errp) != 0)
    return false;
  pid_t pid = fork();
  if (pid < 0) {
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    return false;
  }
  if (pid == 0) {
    if (setpgid(0, 0) != 0)
      _exit(127);
    if (cwd && chdir(cwd) != 0)
      _exit(127);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
    dup2(outp[1], STDOUT_FILENO);
    dup2(errp[1], STDERR_FILENO);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    execvpe(argv[0], argv, env ? env : environ);
    _exit(127);
  }
  setpgid(pid, pid);
  close(outp[1]);
  close(errp[1]);
  fcntl(outp[0], F_SETFL, O_NONBLOCK);
  fcntl(errp[0], F_SETFL, O_NONBLOCK);
  child->pid = pid;
  child->out_fd = outp[0];
  child->err_fd = errp[0];
  child->out = g_string_new(NULL);
  child->err = g_string_new(NULL);
  child->out_open = child->err_open = true;
  return child->out && child->err;
}

static void pump_fd(int fd, GString *out, bool *open) {
  if (!*open)
    return;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
    if (n <= 0) {
      *open = false;
      close(fd);
      return;
    }
    g_string_append_len(out, buf, n);
  }
}

static void child_pump(Child *child) {
  pump_fd(child->out_fd, child->out, &child->out_open);
  pump_fd(child->err_fd, child->err, &child->err_open);
}

static int wait_child(Child *child, int timeout_ms, bool *error) {
  gint64 start = g_get_monotonic_time();
  int status = 0;
  bool exited = false;
  *error = false;
  while (child->out_open || child->err_open || !exited) {
    gint64 elapsed = (g_get_monotonic_time() - start) / 1000;
    if (elapsed > timeout_ms) {
      kill(-child->pid, SIGKILL);
      *error = true;
      timeout_ms = (int)elapsed + 2000;
    }
    struct pollfd fds[2];
    nfds_t nfds = 0;
    if (child->out_open) {
      fds[nfds].fd = child->out_fd;
      fds[nfds].events = POLLIN;
      nfds++;
    }
    if (child->err_open) {
      fds[nfds].fd = child->err_fd;
      fds[nfds].events = POLLIN;
      nfds++;
    }
    if (nfds)
      poll(fds, nfds, 20);
    child_pump(child);
    if (!exited) {
      pid_t got = waitpid(child->pid, &status, WNOHANG);
      if (got == child->pid)
        exited = true;
      else if (got < 0 && errno != EINTR)
        break;
    }
  }
  if (!exited) {
    kill(-child->pid, SIGKILL);
    waitpid(child->pid, &status, 0);
    *error = true;
  }
  if (child->out_open)
    close(child->out_fd);
  if (child->err_open)
    close(child->err_fd);
  if (*error || !WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}

typedef struct {
  int status;
  char *out, *err;
  bool error;
} Run;

static void run_free(Run *run) {
  g_free(run->out);
  g_free(run->err);
  run->out = run->err = NULL;
}

static Run run_argv(const char *cwd, char **argv, char **env, int timeout_ms) {
  Run run = {0};
  Child child;
  if (!spawn_child(&child, cwd, argv, env)) {
    run.error = true;
    run.status = -1;
    run.out = g_strdup("");
    run.err = g_strdup("spawn failed");
    return run;
  }
  bool error = false;
  run.status = wait_child(&child, timeout_ms, &error);
  run.error = error || run.status < 0;
  run.out = g_string_free(child.out, false);
  run.err = g_string_free(child.err, false);
  if (!run.out)
    run.out = g_strdup("");
  if (!run.err)
    run.err = g_strdup("");
  return run;
}

static bool sanitizer_text(const char *err) {
  return strstr(err, "ERROR: AddressSanitizer") ||
         strstr(err, "ERROR: LeakSanitizer") || strstr(err, "runtime error:") ||
         strstr(err, "DEADLYSIGNAL");
}

static bool check_run(const Run *run, int status, bool empty_err, bool asan) {
  if (run->error) {
    char *text = snippet(run->err);
    char *msg = g_strdup_printf("spawn error: %s", text);
    fail_msg(__FILE__, __LINE__, msg);
    g_free(msg);
    g_free(text);
    return false;
  }
  if (asan && sanitizer_text(run->err)) {
    char *text = snippet(run->err);
    fail_msg(__FILE__, __LINE__, text);
    g_free(text);
    return false;
  }
  if (empty_err && run->err[0]) {
    char *text = snippet(run->err);
    char *msg = g_strdup_printf("stderr not empty: %s", text);
    fail_msg(__FILE__, __LINE__, msg);
    g_free(msg);
    g_free(text);
    return false;
  }
  if (run->status != status) {
    char *text = snippet(run->err);
    char *msg = g_strdup_printf("status %d != %d (%s)", run->status, status, text);
    fail_msg(__FILE__, __LINE__, msg);
    g_free(msg);
    g_free(text);
    return false;
  }
  return true;
}

static char *kind_program(const char *kind, bool old, char **script) {
  *script = NULL;
  if (!old)
    return g_strdup_printf("%s/pi-merge-%s", bins, kind);
  GString *path = g_string_new(legacy);
  g_string_replace(path, "{kind}", kind, 0);
  *script = g_string_free(path, false);
  return g_strdup(python);
}

static Run merge_run(const char *kind, char **args, int nargs, bool old,
                     char **env) {
  char *script = NULL;
  char *program = kind_program(kind, old, &script);
  char **argv = g_new0(char *, (guint)nargs + 3);
  int at = 0;
  argv[at++] = program;
  if (script)
    argv[at++] = script;
  for (int i = 0; i < nargs; i++)
    argv[at++] = args[i];
  Run run = run_argv(NULL, argv, env, 15000);
  g_free(argv);
  g_free(program);
  g_free(script);
  return run;
}

static bool write_bytes(const char *path, const void *data, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return false;
  const char *bytes = data;
  size_t at = 0;
  while (at < n) {
    ssize_t wrote = write(fd, bytes + at, n - at);
    if (wrote < 0 && errno == EINTR)
      continue;
    if (wrote <= 0) {
      close(fd);
      return false;
    }
    at += (size_t)wrote;
  }
  return close(fd) == 0;
}

static bool write_text(const char *path, const char *text) {
  return write_bytes(path, text, strlen(text));
}

static char *read_file(const char *path, size_t *len) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char *buf = malloc((size_t)size + 1);
  if (!buf) {
    fclose(file);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)size, file);
  fclose(file);
  if (got != (size_t)size) {
    free(buf);
    return NULL;
  }
  buf[size] = 0;
  if (len)
    *len = (size_t)size;
  return buf;
}

static bool exists_path(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static mode_t mode_bits(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return st.st_mode & 0777;
}

static void rm_tree(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) {
    unlink(path);
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(dir))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    char *child = g_build_filename(path, ent->d_name, NULL);
    struct stat st;
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
      rm_tree(child);
    else
      unlink(child);
    g_free(child);
  }
  closedir(dir);
  rmdir(path);
}

static char *make_temp(const char *prefix) {
  char *path = g_strdup_printf("/tmp/%sXXXXXX", prefix);
  if (!path || !mkdtemp(path)) {
    g_free(path);
    return NULL;
  }
  return path;
}

static char *repeat_text(const char *prefix, const char *unit, size_t times,
                         const char *suffix) {
  size_t unit_len = strlen(unit), prefix_len = strlen(prefix),
         suffix_len = strlen(suffix);
  if (times && unit_len > (SIZE_MAX - prefix_len - suffix_len) / times)
    return NULL;
  size_t n = prefix_len + unit_len * times + suffix_len;
  char *out = malloc(n + 1);
  if (!out)
    return NULL;
  memcpy(out, prefix, prefix_len);
  size_t at = prefix_len;
  for (size_t i = 0; i < times; i++) {
    memcpy(out + at, unit, unit_len);
    at += unit_len;
  }
  memcpy(out + at, suffix, suffix_len);
  out[at + suffix_len] = 0;
  return out;
}

static bool same_json(Json *a, Json *b) {
  if (!a || !b || a->kind != b->kind)
    return false;
  switch (a->kind) {
  case J_NULL:
    return true;
  case J_BOOL:
    return a->boolean == b->boolean;
  case J_INT:
    return a->string->len == b->string->len &&
           !memcmp(a->string->str, b->string->str, a->string->len);
  case J_REAL:
    return a->real == b->real;
  case J_STRING:
    return a->string->len == b->string->len &&
           !memcmp(a->string->str, b->string->str, a->string->len);
  case J_ARRAY:
    if (a->values->len != b->values->len)
      return false;
    for (guint i = 0; i < a->values->len; i++)
      if (!same_json(a->values->pdata[i], b->values->pdata[i]))
        return false;
    return true;
  case J_OBJECT:
    if (a->keys->len != b->keys->len)
      return false;
    for (guint i = 0; i < a->keys->len; i++) {
      GString *key = a->keys->pdata[i];
      Json *other = json_get(b, key->str, key->len);
      if (!same_json(a->values->pdata[i], other))
        return false;
    }
    return true;
  }
  return false;
}

static bool file_json_equals(const char *path, const char *expected) {
  size_t n = 0;
  char *data = read_file(path, &n);
  if (!data || memchr(data, 0, n)) {
    free(data);
    return false;
  }
  Json *actual = json_parse(data);
  Json *want = json_parse(expected);
  bool ok = actual && want && same_json(actual, want);
  if (actual)
    json_free(actual);
  if (want)
    json_free(want);
  free(data);
  return ok;
}

static Json *parse_file(const char *path) {
  size_t n = 0;
  char *data = read_file(path, &n);
  if (!data || memchr(data, 0, n)) {
    free(data);
    return NULL;
  }
  Json *value = json_parse(data);
  free(data);
  return value;
}

static bool bytes_equal(const char *path, const char *data, size_t n) {
  size_t got = 0;
  char *file = read_file(path, &got);
  bool ok = file && got == n && !memcmp(file, data, n);
  free(file);
  return ok;
}

static char *preload_value(void) {
  const char *asan = getenv("PI_CONFIG_ASAN_RT");
  if (asan && *asan)
    return g_strdup_printf("%s:%s", asan, failure_lib);
  return g_strdup(failure_lib);
}

static char **fault_env(const char *path, const char *mode, const char *release) {
  char *preload = preload_value();
  GPtrArray *env = env_inherited();
  env_set(env, "LD_PRELOAD", preload);
  env_set(env, "PI_CONFIG_FAILURE_PATH", path);
  env_set(env, "PI_CONFIG_FAIL", mode);
  if (release)
    env_set(env, "PI_CONFIG_RELEASE", release);
  g_free(preload);
  return env_finish(env);
}

static void settings_case(const char *defaults, const char *value, bool absent,
                          const char *expected, int status) {
  int passes = legacy ? 2 : 1;
  char *native = NULL;
  size_t native_len = 0;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      break;
    char *file = g_build_filename(root, "settings.json", NULL);
    char *declared = g_build_filename(root, "defaults.json", NULL);
    CHECK(write_text(declared, defaults));
    if (!absent)
      CHECK(write_text(file, value));
    char *args[] = {declared, file};
    Run run = merge_run("settings", args, 2, old, NULL);
    check_run(&run, status, false, true);
    if (!status && exists_path(file)) {
      if (expected)
        CHECK(file_json_equals(file, expected));
      size_t n = 0;
      char *data = read_file(file, &n);
      CHECK(data);
      if (data) {
        if (!old) {
          free(native);
          native = data;
          native_len = n;
        } else {
          CHECK(native && n == native_len && !memcmp(data, native, n));
          free(data);
        }
      }
    }
    run_free(&run);
    g_free(file);
    g_free(declared);
    rm_tree(root);
    g_free(root);
  }
  free(native);
}

static bool write_seed(const char *root) {
  char *refresh = g_build_filename(root, "refresh", NULL);
  char *account = g_build_filename(root, "account", NULL);
  const char refresh_bytes[] = "  fixture-refresh\r\n";
  const char account_bytes[] = "\xe2\x80\x83" "fixture-account\t";
  bool ok = write_bytes(refresh, refresh_bytes, sizeof refresh_bytes - 1) &&
            write_bytes(account, account_bytes, sizeof account_bytes - 1);
  g_free(refresh);
  g_free(account);
  return ok;
}

static void add_seed(GPtrArray *args, const char *root) {
  g_ptr_array_add(args, g_strdup("--oauth-provider"));
  g_ptr_array_add(args, g_strdup("openai-codex"));
  g_ptr_array_add(args, g_strdup("--refresh-file"));
  g_ptr_array_add(args, g_build_filename(root, "refresh", NULL));
  g_ptr_array_add(args, g_strdup("--account-file"));
  g_ptr_array_add(args, g_build_filename(root, "account", NULL));
}

static char **args_finish(GPtrArray *args, int *count) {
  *count = (int)args->len;
  char **out = g_new(char *, args->len + 1);
  for (guint i = 0; i < args->len; i++)
    out[i] = args->pdata[i];
  out[args->len] = NULL;
  g_ptr_array_free(args, false);
  return out;
}

static void args_free(char **args) {
  if (!args)
    return;
  for (char **item = args; *item; item++)
    g_free(*item);
  g_free(args);
}

static const char *seeded =
    "{\"other\":{\"type\":\"api_key\",\"key\":\"keep\"},\"openai-codex\":{"
    "\"type\":\"oauth\",\"refresh\":\"fixture-refresh\",\"accountId\":\"fixture-"
    "account\",\"access\":\"\",\"expires\":0}}";

static void test_fill(void) {
  test_name = "settings fill only missing top-level keys";
  settings_case("{\"a\":1,\"nested\":{\"a\":1,\"b\":2},\"nil\":3,\"flag\":true}",
                "{\"nested\":{\"a\":9},\"nil\":null,\"flag\":false,\"keep\":[1,2]}",
                false,
                "{\"nested\":{\"a\":9},\"nil\":null,\"flag\":false,\"keep\":[1,2],\"a\":1}",
                0);
  settings_case("{\"a\":1}", "", true, "{\"a\":1}", 0);
  settings_case("{\"a\":1}", "", false, "{\"a\":1}", 0);
}

static void test_retarget(void) {
  test_name = "old provider/model pairs retarget";
  const char *pairs[][2] = {
      {"xai", "grok-4.6"},
      {"xai", "grok-4.7"},
      {"opencode-go", "deepseek-v4.1-flash"},
      {"opencode-go", "glm-5.3"},
      {"opencode-go", "space-bunny-free"},
  };
  for (int i = 0; i < 5; i++) {
    char *input = g_strdup_printf(
        "{\"defaultProvider\":\"%s\",\"defaultModel\":\"%s\",\"extra\":1}",
        pairs[i][0], pairs[i][1]);
    settings_case("{\"defaultProvider\":\"declared\",\"defaultModel\":\"model\"}",
                  input, false,
                  "{\"defaultProvider\":\"declared\",\"defaultModel\":\"model\",\"extra\":1}",
                  0);
    g_free(input);
  }
  settings_case("{\"defaultProvider\":\"declared\",\"defaultModel\":\"model\"}",
                "{\"defaultProvider\":\"xai\",\"defaultModel\":\"grok-next\"}", false,
                "{\"defaultProvider\":\"xai\",\"defaultModel\":\"grok-next\"}", 0);
  settings_case("{}", "{\"defaultProvider\":\"xai\",\"defaultModel\":\"grok-4.7\"}",
                false, "{\"defaultProvider\":null,\"defaultModel\":null}", 0);
  settings_case("{\"defaultModel\":\"grok-4.7\"}", "{\"defaultProvider\":\"xai\"}",
                false, "{\"defaultProvider\":null,\"defaultModel\":\"grok-4.7\"}", 0);
}

static void test_unhashable(void) {
  test_name = "unhashable provider/model settings fail before writing";
  const char *keys[] = {"defaultProvider", "defaultModel"};
  const char *values[] = {"[]", "{}"};
  for (int k = 0; k < 2; k++)
    for (int v = 0; v < 2; v++) {
      char *input = g_strdup_printf("{\"%s\":%s}", keys[k], values[v]);
      settings_case("{\"new\":true}", input, false, NULL, 1);
      g_free(input);
    }
}

static void test_noop(void) {
  test_name = "no-op settings/auth retain original bytes, inode and mode";
  const char *kinds[] = {"auth", "settings"};
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++)
    for (int k = 0; k < 2; k++) {
      char *root = make_temp("pi-config-");
      CHECK(root);
      if (!root)
        return;
      char *file = g_strdup_printf("%s/%s.json", root, kinds[k]);
      CHECK(write_text(file, "{ \"keep\": 1 }"));
      CHECK(chmod(file, 0640) == 0);
      struct stat before;
      CHECK(stat(file, &before) == 0);
      char *defaults = g_build_filename(root, "defaults.json", NULL);
      CHECK(write_text(defaults, "{}"));
      Run run;
      if (!strcmp(kinds[k], "auth")) {
        char *args[] = {file};
        run = merge_run("auth", args, 1, old, NULL);
      } else {
        char *args[] = {defaults, file};
        run = merge_run("settings", args, 2, old, NULL);
      }
      check_run(&run, 0, false, true);
      CHECK(bytes_equal(file, "{ \"keep\": 1 }", 13));
      struct stat after;
      CHECK(stat(file, &after) == 0);
      CHECK(after.st_ino == before.st_ino);
      CHECK((after.st_mode & 0777) == 0640);
      char *absent = g_build_filename(root, "missing", "file", NULL);
      Run missing;
      if (!strcmp(kinds[k], "auth")) {
        char *args[] = {absent};
        missing = merge_run("auth", args, 1, old, NULL);
      } else {
        char *args[] = {defaults, absent};
        missing = merge_run("settings", args, 2, old, NULL);
      }
      check_run(&missing, 0, false, true);
      char *missing_dir = g_build_filename(root, "missing", NULL);
      CHECK(!exists_path(missing_dir));
      run_free(&run);
      run_free(&missing);
      g_free(missing_dir);
      g_free(absent);
      g_free(defaults);
      g_free(file);
      rm_tree(root);
      g_free(root);
    }
}

static void test_migration(void) {
  test_name = "one-time memory migration";
  const char *defaults =
      "{\"observational-memory-jev\":{\"enabledByDefault\":false}}";
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    char *file = g_build_filename(root, "settings.json", NULL);
    char *declared = g_build_filename(root, "defaults.json", NULL);
    char *marker = g_build_filename(root, ".om-default-off-migrated", NULL);
    CHECK(write_text(declared, defaults));
    CHECK(write_text(file,
                     "{\"observational-memory-jev\":{\"enabledByDefault\":true,\"other\":\"keep\"}}"));
    char *args[] = {declared, file};
    Run run = merge_run("settings", args, 2, old, NULL);
    check_run(&run, 0, false, true);
    CHECK(file_json_equals(file,
                           "{\"observational-memory-jev\":{\"enabledByDefault\":false,\"other\":\"keep\"}}"));
    CHECK(bytes_equal(marker, "observational memory home default is off\n", 41));
    CHECK(mode_bits(marker) == 0600);
    CHECK(write_text(file, "{\"observational-memory-jev\":{\"enabledByDefault\":true}}"));
    Run again = merge_run("settings", args, 2, old, NULL);
    check_run(&again, 0, false, true);
    Json *parsed = parse_file(file);
    Json *entry = parsed ? json_get(parsed, "observational-memory-jev", 24) : NULL;
    Json *flag = entry ? json_get(entry, "enabledByDefault", 16) : NULL;
    CHECK(flag && flag->kind == J_BOOL && flag->boolean);
    if (parsed)
      json_free(parsed);
    run_free(&run);
    run_free(&again);
    g_free(marker);
    g_free(declared);
    g_free(file);
    rm_tree(root);
    g_free(root);
  }
  const char *values[] = {"false", "null", "0", "1", "\"true\"", "[]", "{}"};
  for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
    char *input = g_strdup_printf(
        "{\"observational-memory-jev\":{\"enabledByDefault\":%s}}", values[i]);
    settings_case(defaults, input, false, input, 0);
    g_free(input);
  }
}

static void test_marker_only(void) {
  test_name = "marker-only runs do not rewrite settings";
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    char *file = g_build_filename(root, "settings.json", NULL);
    char *declared = g_build_filename(root, "defaults.json", NULL);
    char *marker = g_build_filename(root, ".om-default-off-migrated", NULL);
    CHECK(write_text(file, "{\"observational-memory-jev\":null}"));
    struct stat before;
    CHECK(stat(file, &before) == 0);
    CHECK(write_text(declared,
                     "{\"observational-memory-jev\":{\"enabledByDefault\":false}}"));
    char *args[] = {declared, file};
    Run run = merge_run("settings", args, 2, old, NULL);
    check_run(&run, 0, false, true);
    struct stat after;
    CHECK(stat(file, &after) == 0 && after.st_ino == before.st_ino);
    CHECK(exists_path(marker));
    run_free(&run);
    g_free(marker);
    g_free(declared);
    g_free(file);
    rm_tree(root);
    g_free(root);
  }
  const char *values[] = {NULL, "null", "true", "0", "1", "\"false\""};
  for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
    for (int old = 0; old < passes; old++) {
      char *root = make_temp("pi-config-");
      CHECK(root);
      if (!root)
        return;
      char *file = g_build_filename(root, "settings.json", NULL);
      char *declared = g_build_filename(root, "defaults.json", NULL);
      char *marker = g_build_filename(root, ".om-default-off-migrated", NULL);
      CHECK(write_text(file, "{}"));
      if (!values[i])
        CHECK(write_text(declared, "{}"));
      else {
        char *text = g_strdup_printf(
            "{\"observational-memory-jev\":{\"enabledByDefault\":%s}}", values[i]);
        CHECK(write_text(declared, text));
        g_free(text);
      }
      char *args[] = {declared, file};
      Run run = merge_run("settings", args, 2, old, NULL);
      check_run(&run, 0, false, true);
      CHECK(!exists_path(marker));
      run_free(&run);
      g_free(marker);
      g_free(declared);
      g_free(file);
      rm_tree(root);
      g_free(root);
    }
}

static void test_marker_links(void) {
  test_name = "marker directories and symlinks suppress migration";
  const char *modes[] = {"directory", "symlink", "dangling"};
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++)
    for (int m = 0; m < 3; m++) {
      char *root = make_temp("pi-config-");
      CHECK(root);
      if (!root)
        return;
      char *file = g_build_filename(root, "settings.json", NULL);
      char *declared = g_build_filename(root, "defaults.json", NULL);
      char *marker = g_build_filename(root, ".om-default-off-migrated", NULL);
      char *target = g_build_filename(root, "target", NULL);
      CHECK(write_text(declared,
                       "{\"observational-memory-jev\":{\"enabledByDefault\":false}}"));
      CHECK(write_text(file, "{\"observational-memory-jev\":{\"enabledByDefault\":true}}"));
      if (!strcmp(modes[m], "directory"))
        CHECK(mkdir(marker, 0700) == 0);
      else {
        if (!strcmp(modes[m], "symlink"))
          CHECK(write_text(target, "keep"));
        CHECK(symlink("target", marker) == 0);
      }
      char *args[] = {declared, file};
      Run run = merge_run("settings", args, 2, old, NULL);
      check_run(&run, 0, false, true);
      Json *parsed = parse_file(file);
      Json *entry = parsed ? json_get(parsed, "observational-memory-jev", 24) : NULL;
      Json *flag = entry ? json_get(entry, "enabledByDefault", 16) : NULL;
      bool expect = strcmp(modes[m], "dangling") != 0;
      CHECK(flag && flag->kind == J_BOOL && flag->boolean == expect);
      if (!strcmp(modes[m], "dangling"))
        CHECK(bytes_equal(target, "observational memory home default is off\n", 41));
      if (parsed)
        json_free(parsed);
      run_free(&run);
      g_free(target);
      g_free(marker);
      g_free(declared);
      g_free(file);
      rm_tree(root);
      g_free(root);
    }
}

static void test_auth_seed(void) {
  test_name = "auth removes named providers and seeds the exact schema";
  const char *currents[] = {
      NULL,
      "null",
      "42",
      "\"value\"",
      "[]",
      "{}",
      "{\"type\":\"api_key\",\"key\":\"obsolete\"}",
      "{\"type\":\"OAuth\"}",
  };
  int passes = legacy ? 2 : 1;
  for (size_t c = 0; c < sizeof currents / sizeof currents[0]; c++) {
    char *native = NULL;
    size_t native_len = 0;
    for (int old = 0; old < passes; old++) {
      char *root = make_temp("pi-config-");
      CHECK(root);
      if (!root)
        return;
      CHECK(write_seed(root));
      char *file = g_build_filename(root, "auth.json", NULL);
      GString *body = g_string_new("{\"other\":{\"type\":\"api_key\",\"key\":\"keep\"},\"openai\":{\"key\":\"drop\"}");
      if (currents[c])
        g_string_append_printf(body, ",\"openai-codex\":%s", currents[c]);
      g_string_append_c(body, '}');
      CHECK(write_text(file, body->str));
      g_string_free(body, true);
      GPtrArray *list = g_ptr_array_new();
      g_ptr_array_add(list, g_strdup(file));
      g_ptr_array_add(list, g_strdup("--drop"));
      g_ptr_array_add(list, g_strdup("openai"));
      add_seed(list, root);
      int nargs = 0;
      char **args = args_finish(list, &nargs);
      Run run = merge_run("auth", args, nargs, old, NULL);
      check_run(&run, 0, false, true);
      CHECK(file_json_equals(file, seeded));
      CHECK(mode_bits(file) == 0600);
      size_t n = 0;
      char *data = read_file(file, &n);
      CHECK(data);
      if (data) {
        if (old) {
          CHECK(native && n == native_len && !memcmp(data, native, n));
          free(data);
        } else {
          free(native);
          native = data;
          native_len = n;
        }
      }
      args_free(args);
      run_free(&run);
      g_free(file);
      rm_tree(root);
      g_free(root);
    }
    free(native);
  }
}

static void test_oauth_retain(void) {
  test_name = "existing OAuth state is retained";
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    CHECK(write_seed(root));
    char *file = g_build_filename(root, "auth.json", NULL);
    CHECK(write_text(file, "{\"openai-codex\":{\"type\":\"oauth\"}}"));
    GPtrArray *seeded_args = g_ptr_array_new();
    g_ptr_array_add(seeded_args, g_strdup(file));
    add_seed(seeded_args, root);
    int nargs = 0;
    char **args = args_finish(seeded_args, &nargs);
    Run run = merge_run("auth", args, nargs, old, NULL);
    check_run(&run, 0, false, true);
    CHECK(file_json_equals(file, "{\"openai-codex\":{\"type\":\"oauth\"}}"));
    args_free(args);
    GPtrArray *dropped = g_ptr_array_new();
    g_ptr_array_add(dropped, g_strdup(file));
    g_ptr_array_add(dropped, g_strdup("--drop"));
    g_ptr_array_add(dropped, g_strdup("openai-codex"));
    add_seed(dropped, root);
    args = args_finish(dropped, &nargs);
    Run again = merge_run("auth", args, nargs, old, NULL);
    check_run(&again, 0, false, true);
    Json *parsed = parse_file(file);
    Json *entry = parsed ? json_get(parsed, "openai-codex", 12) : NULL;
    Json *refresh = entry ? json_get(entry, "refresh", 7) : NULL;
    CHECK(refresh && refresh->kind == J_STRING && refresh->string->len == 15 &&
          !memcmp(refresh->string->str, "fixture-refresh", 15));
    if (parsed)
      json_free(parsed);
    args_free(args);
    run_free(&run);
    run_free(&again);
    g_free(file);
    rm_tree(root);
    g_free(root);
  }
}

static void test_missing_secrets(void) {
  test_name = "missing or empty secret pairs disable seeding";
  const char *modes[] = {"missing-refresh", "missing-account", "empty-refresh",
                         "empty-account"};
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++)
    for (int m = 0; m < 4; m++) {
      char *root = make_temp("pi-config-");
      CHECK(root);
      if (!root)
        return;
      CHECK(write_seed(root));
      char *file = g_build_filename(root, "auth.json", NULL);
      CHECK(write_text(file, "{\"old\":1,\"keep\":2}"));
      bool refresh = strstr(modes[m], "refresh");
      char *secret = g_build_filename(root, refresh ? "refresh" : "account", NULL);
      if (g_str_has_prefix(modes[m], "missing"))
        CHECK(unlink(secret) == 0);
      else {
        const char empty[] = " \xe2\x80\x83\n\x1c";
        CHECK(write_bytes(secret, empty, sizeof empty - 1));
      }
      GPtrArray *list = g_ptr_array_new();
      g_ptr_array_add(list, g_strdup(file));
      g_ptr_array_add(list, g_strdup("--drop"));
      g_ptr_array_add(list, g_strdup("old"));
      add_seed(list, root);
      int nargs = 0;
      char **args = args_finish(list, &nargs);
      Run run = merge_run("auth", args, nargs, old, NULL);
      check_run(&run, 0, false, true);
      CHECK(file_json_equals(file, "{\"keep\":2}"));
      args_free(args);
      run_free(&run);
      g_free(secret);
      g_free(file);
      rm_tree(root);
      g_free(root);
    }
}

static void test_secret_interior(void) {
  test_name = "secret values retain interior whitespace and NUL";
  int passes = legacy ? 2 : 1;
  char *native = NULL;
  size_t native_len = 0;
  const char refresh[] = "\x1c\xe2\x80\x83尾\r\nline\rkeep\0value\xc2\xa0";
  const char account[] = "\taccount\"\\\0end\xe3\x80\x80";
  const char expected[] = "尾\nline\nkeep\0value";
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    CHECK(write_seed(root));
    char *refresh_path = g_build_filename(root, "refresh", NULL);
    char *account_path = g_build_filename(root, "account", NULL);
    CHECK(write_bytes(refresh_path, refresh, sizeof refresh - 1));
    CHECK(write_bytes(account_path, account, sizeof account - 1));
    char *file = g_build_filename(root, "auth.json", NULL);
    GPtrArray *list = g_ptr_array_new();
    g_ptr_array_add(list, g_strdup(file));
    add_seed(list, root);
    int nargs = 0;
    char **args = args_finish(list, &nargs);
    Run run = merge_run("auth", args, nargs, old, NULL);
    check_run(&run, 0, false, true);
    Json *parsed = parse_file(file);
    Json *entry = parsed ? json_get(parsed, "openai-codex", 12) : NULL;
    Json *value = entry ? json_get(entry, "refresh", 7) : NULL;
    CHECK(value && value->kind == J_STRING &&
          value->string->len == sizeof expected - 1 &&
          !memcmp(value->string->str, expected, sizeof expected - 1));
    if (parsed)
      json_free(parsed);
    size_t n = 0;
    char *data = read_file(file, &n);
    CHECK(data);
    if (data) {
      if (old) {
        CHECK(native && n == native_len && !memcmp(data, native, n));
        free(data);
      } else {
        free(native);
        native = data;
        native_len = n;
      }
    }
    args_free(args);
    run_free(&run);
    g_free(file);
    g_free(account_path);
    g_free(refresh_path);
    rm_tree(root);
    g_free(root);
  }
  free(native);
}

static void append_cp(GString *out, uint32_t cp) {
  char buf[6];
  int n = g_unichar_to_utf8(cp, buf);
  if (n > 0)
    g_string_append_len(out, buf, n);
}

static void test_secret_whitespace(void) {
  test_name = "secret stripping includes every Python whitespace code point";
  uint32_t chars[40];
  int count = 0;
  uint32_t fixed[] = {0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e, 0x1f,
                      0x20, 0x85, 0xa0, 0x1680};
  for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
    chars[count++] = fixed[i];
  for (uint32_t cp = 0x2000; cp <= 0x200a; cp++)
    chars[count++] = cp;
  uint32_t tail[] = {0x2028, 0x2029, 0x202f, 0x205f, 0x3000};
  for (size_t i = 0; i < sizeof tail / sizeof tail[0]; i++)
    chars[count++] = tail[i];
  int passes = legacy ? 2 : 1;
  for (int c = 0; c < count; c++)
    for (int old = 0; old < passes; old++) {
      char *root = make_temp("pi-config-");
      CHECK(root);
      if (!root)
        return;
      CHECK(write_seed(root));
      GString *refresh = g_string_new(NULL);
      GString *account = g_string_new(NULL);
      append_cp(refresh, chars[c]);
      g_string_append(refresh, "fixture-refresh");
      append_cp(refresh, chars[c]);
      append_cp(account, chars[c]);
      g_string_append(account, "fixture-account");
      append_cp(account, chars[c]);
      char *refresh_path = g_build_filename(root, "refresh", NULL);
      char *account_path = g_build_filename(root, "account", NULL);
      CHECK(write_bytes(refresh_path, refresh->str, refresh->len));
      CHECK(write_bytes(account_path, account->str, account->len));
      char *file = g_build_filename(root, "auth.json", NULL);
      GPtrArray *list = g_ptr_array_new();
      g_ptr_array_add(list, g_strdup(file));
      add_seed(list, root);
      int nargs = 0;
      char **args = args_finish(list, &nargs);
      Run run = merge_run("auth", args, nargs, old, NULL);
      check_run(&run, 0, false, true);
      Json *parsed = parse_file(file);
      Json *entry = parsed ? json_get(parsed, "openai-codex", 12) : NULL;
      Json *refresh_value = entry ? json_get(entry, "refresh", 7) : NULL;
      Json *account_value = entry ? json_get(entry, "accountId", 9) : NULL;
      CHECK(refresh_value && refresh_value->kind == J_STRING &&
            refresh_value->string->len == 15 &&
            !memcmp(refresh_value->string->str, "fixture-refresh", 15));
      CHECK(account_value && account_value->kind == J_STRING &&
            account_value->string->len == 15 &&
            !memcmp(account_value->string->str, "fixture-account", 15));
      if (parsed)
        json_free(parsed);
      args_free(args);
      run_free(&run);
      g_free(file);
      g_free(account_path);
      g_free(refresh_path);
      g_string_free(account, true);
      g_string_free(refresh, true);
      rm_tree(root);
      g_free(root);
    }
}

static void test_secrets_before_load(void) {
  test_name = "both secret files are validated before auth is loaded";
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    CHECK(write_seed(root));
    char *file = g_build_filename(root, "auth.json", NULL);
    CHECK(write_text(file, "{\"openai-codex\":{\"type\":\"oauth\"}}"));
    size_t before_len = 0;
    char *before = read_file(file, &before_len);
    CHECK(before);
    char *refresh = g_build_filename(root, "refresh", NULL);
    char bad = (char)255;
    CHECK(write_bytes(refresh, &bad, 1));
    GPtrArray *list = g_ptr_array_new();
    g_ptr_array_add(list, g_strdup(file));
    add_seed(list, root);
    int nargs = 0;
    char **args = args_finish(list, &nargs);
    Run run = merge_run("auth", args, nargs, old, NULL);
    check_run(&run, 1, false, true);
    CHECK(bytes_equal(file, before, before_len));
    char *account = g_build_filename(root, "account", NULL);
    CHECK(unlink(account) == 0);
    Run again = merge_run("auth", args, nargs, old, NULL);
    check_run(&again, 0, false, true);
    free(before);
    args_free(args);
    run_free(&run);
    run_free(&again);
    g_free(account);
    g_free(refresh);
    g_free(file);
    rm_tree(root);
    g_free(root);
  }
}

static void malformed_one(const char *kind, const void *data, size_t n, bool old) {
  char *root = make_temp("pi-config-");
  CHECK(root);
  if (!root)
    return;
  char *file = g_strdup_printf("%s/%s.json", root, kind);
  char *defaults = g_build_filename(root, "defaults.json", NULL);
  char *marker = g_build_filename(root, ".om-default-off-migrated", NULL);
  CHECK(write_bytes(file, data, n));
  CHECK(write_text(defaults,
                   "{\"new\":1,\"observational-memory-jev\":{\"enabledByDefault\":false}}"));
  size_t before_len = 0;
  char *before = read_file(file, &before_len);
  Run run;
  if (!strcmp(kind, "auth")) {
    char *args[] = {file, "--drop", "x"};
    run = merge_run("auth", args, 3, old, NULL);
  } else {
    char *args[] = {defaults, file};
    run = merge_run("settings", args, 2, old, NULL);
  }
  check_run(&run, 1, false, true);
  CHECK(bytes_equal(file, before, before_len));
  CHECK(!exists_path(marker));
  char *tmp = g_strdup_printf("%s.tmp", file);
  CHECK(!exists_path(tmp));
  free(before);
  run_free(&run);
  g_free(tmp);
  g_free(marker);
  g_free(defaults);
  g_free(file);
  rm_tree(root);
  g_free(root);
}

static void test_malformed(void) {
  test_name = "malformed JSON fails without replacing files";
  const char *texts[] = {" ", "[1]", "null", "true", "1", "{\"x\":1,}",
                         "{\"x\":\"\\q\"}", "\ufeff{}"};
  int passes = legacy ? 2 : 1;
  const char *kinds[] = {"auth", "settings"};
  for (int old = 0; old < passes; old++)
    for (size_t t = 0; t < sizeof texts / sizeof texts[0]; t++)
      for (int k = 0; k < 2; k++)
        malformed_one(kinds[k], texts[t], strlen(texts[t]), old);
  const char nuljson[] = {'{', '}', 0};
  const char badbyte[] = {(char)255};
  for (int old = 0; old < passes; old++)
    for (int k = 0; k < 2; k++) {
      malformed_one(kinds[k], nuljson, 3, old);
      malformed_one(kinds[k], badbyte, 1, old);
    }
  const char *defaults[] = {"", "[]", "null", "1"};
  for (size_t i = 0; i < 4; i++)
    settings_case(defaults[i], "{}", false, NULL, 1);
}

static void test_round_trip(void) {
  test_name = "JSON round trips preserve duplicate keys and constants";
  const char *value =
      "{\"keep\":1,\"dup\":0,\"dup\":2,\"nul\\u0000\":1,\"surrogate\":\"\\ud800\","
      "\"pair\":\"\\ud83d\\ude00\",\"numbers\":[-0,-0.0,18446744073709551615,1e-5,"
      "1e16,NaN,Infinity,-Infinity,1e999]}";
  settings_case("{\"new\":1}", value, false, NULL, 0);
  char *big = repeat_text("{\"big\":", "1", 4300, "}");
  char *over = repeat_text("{\"big\":", "1", 4301, "}");
  CHECK(big && over);
  if (big)
    settings_case("{\"new\":1}", big, false, NULL, 0);
  if (over)
    settings_case("{\"new\":1}", over, false, NULL, 1);
  free(big);
  free(over);
}

static uint32_t lcg(uint32_t *seed) {
  *seed = *seed * 1664525u + 1013904223u;
  return *seed;
}

static void test_real_corpus(void) {
  test_name = "deterministic real values retain legacy shortest-decimal output";
  uint32_t seed = 73;
  GString *payload = g_string_new("{\"values\":[");
  bool first = true;
  for (int i = 0; i < 500; i++) {
    uint32_t lo = lcg(&seed), hi = lcg(&seed);
    uint64_t bits = (uint64_t)lo | ((uint64_t)hi << 32);
    double value;
    memcpy(&value, &bits, sizeof value);
    if (!isfinite(value))
      continue;
    char text[128];
    g_ascii_formatd(text, sizeof text, "%.17g", value);
    if (!first)
      g_string_append_c(payload, ',');
    first = false;
    g_string_append(payload, text);
  }
  g_string_append(payload, "]}");
  settings_case("{\"new\":1}", payload->str, false, NULL, 0);
  g_string_free(payload, true);
}

static void test_unbounded_input(void) {
  test_name = "input is not constrained to panel payload size";
  char *text = repeat_text("{\"text\":\"", "🙂", 10000, "\"}");
  char *expected = repeat_text("{\"text\":\"", "🙂", 10000, "\",\"new\":true}");
  CHECK(text && expected);
  if (text && expected)
    settings_case("{\"new\":true}", text, false, expected, 0);
  free(text);
  free(expected);
  int passes = legacy ? 2 : 1;
  char *value = repeat_text("{\"nested\":", "[", 5000, "");
  char *closing = repeat_text("", "]", 5000, "}");
  CHECK(value && closing);
  if (!value || !closing) {
    free(value);
    free(closing);
    return;
  }
  char *body = g_strdup_printf("%s0%s", value, closing);
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      break;
    char *path = g_build_filename(root, "auth.json", NULL);
    CHECK(write_text(path, body));
    char *args[] = {path};
    Run run = merge_run("auth", args, 1, old, NULL);
    check_run(&run, 0, false, true);
    CHECK(bytes_equal(path, body, strlen(body)));
    run_free(&run);
    g_free(path);
    rm_tree(root);
    g_free(root);
  }
  free(body);
  free(closing);
  free(value);
}

static void test_pretty_depth(void) {
  test_name = "pretty dumps support nesting beyond the panel encoder";
  const char *leaves[] = {"0", "0.5", "[]", "{}", "\"x\""};
  for (int i = 0; i < 5; i++) {
    char *open = repeat_text("{\"deep\":", "[", 1200, "");
    char *close = repeat_text("", "]", 1200, "}");
    CHECK(open && close);
    if (open && close) {
      char *payload = g_strdup_printf("%s%s%s", open, leaves[i], close);
      settings_case("{\"new\":true}", payload, false, NULL, 0);
      g_free(payload);
    }
    free(open);
    free(close);
  }
  char *open = repeat_text("{\"deep\":", "[", 10000, "");
  char *close = repeat_text("", "]", 10000, "}");
  CHECK(open && close);
  if (open && close) {
    char *payload = g_strdup_printf("%s0%s", open, close);
    settings_case("{\"new\":true}", payload, false, NULL, 1);
    g_free(payload);
  }
  free(open);
  free(close);
}

static void test_negative_providers(void) {
  test_name = "negative-looking provider arguments";
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    char *path = g_build_filename(root, "auth.json", NULL);
    CHECK(write_text(path, "{\"-1\":1,\"-.5\":2,\"-١\":3,\"keep\":4}"));
    char *args[] = {path, "--drop", "-1", "--drop", "-.5", "--drop", "-١"};
    Run run = merge_run("auth", args, 7, old, NULL);
    check_run(&run, 0, false, true);
    CHECK(file_json_equals(path, "{\"keep\":4}"));
    run_free(&run);
    g_free(path);
    rm_tree(root);
    g_free(root);
  }
}

static void test_path_links(void) {
  test_name = "parent and destination links preserve Pathlib behavior";
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    char *defaults = g_build_filename(root, "defaults.json", NULL);
    char *actual = g_build_filename(root, "actual", NULL);
    char *alias = g_build_filename(root, "alias", NULL);
    char *target = g_build_filename(root, "target", NULL);
    char *leaf = g_build_filename(root, "actual", "settings.json", NULL);
    CHECK(write_text(defaults, "{\"new\":1}"));
    CHECK(mkdir(actual, 0700) == 0);
    CHECK(symlink("actual", alias) == 0);
    CHECK(write_text(target, "{\"keep\":2}"));
    CHECK(symlink("../target", leaf) == 0);
    char *spelled = g_strdup_printf("%s/alias/./settings.json//", root);
    char *args[] = {defaults, spelled};
    Run run = merge_run("settings", args, 2, old, NULL);
    check_run(&run, 0, false, true);
    struct stat st;
    CHECK(lstat(leaf, &st) == 0 && !S_ISLNK(st.st_mode));
    CHECK(file_json_equals(leaf, "{\"keep\":2,\"new\":1}"));
    CHECK(file_json_equals(target, "{\"keep\":2}"));
    char *other = g_strdup_printf("%s/missing/../other.json", root);
    char *more[] = {defaults, other};
    Run created = merge_run("settings", more, 2, old, NULL);
    check_run(&created, 0, false, true);
    char *missing = g_build_filename(root, "missing", NULL);
    CHECK(exists_path(missing));
    run_free(&run);
    run_free(&created);
    g_free(missing);
    g_free(other);
    g_free(spelled);
    g_free(leaf);
    g_free(target);
    g_free(alias);
    g_free(actual);
    g_free(defaults);
    rm_tree(root);
    g_free(root);
  }
}

static void test_cli(void) {
  test_name = "CLI validation happens before mutations";
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    char *file = g_build_filename(root, "auth.json", NULL);
    const char *invalid[][6] = {
        {NULL},
        {file, "extra", NULL},
        {file, "--unknown", NULL},
        {file, "--drop", NULL},
        {file, "--oauth-provider", "p", NULL},
        {file, "--refresh-file", "a", "--oauth-provider", "p", NULL},
    };
    int counts[] = {0, 2, 2, 2, 3, 5};
    for (int i = 0; i < 6; i++) {
      char *args[6];
      for (int n = 0; n < counts[i]; n++)
        args[n] = (char *)invalid[i][n];
      Run run = merge_run("auth", args, counts[i], old, NULL);
      check_run(&run, 2, false, true);
      run_free(&run);
    }
    CHECK(!exists_path(file));
    CHECK(write_text(file, "{\"x\":1,\"y\":2,\"z\":3}"));
    char *drop[] = {file, "--dr=x", "--drop", "y", "--drop", "x"};
    Run dropped = merge_run("auth", drop, 6, old, NULL);
    check_run(&dropped, 0, false, true);
    CHECK(file_json_equals(file, "{\"z\":3}"));
    char *empty[] = {file, "--oauth-provider="};
    Run provider = merge_run("auth", empty, 2, old, NULL);
    check_run(&provider, 0, false, true);
    const char *help_ok[][4] = {{"-h"}, {"--unknown", "-h"}, {"one", "two", "--help"}, {"-hh"}};
    int help_n[] = {1, 2, 3, 1};
    for (int i = 0; i < 4; i++) {
      char *args[4];
      for (int n = 0; n < help_n[i]; n++)
        args[n] = (char *)help_ok[i][n];
      Run run = merge_run("auth", args, help_n[i], old, NULL);
      check_run(&run, 0, false, true);
      run_free(&run);
    }
    const char *help_bad[][4] = {{"--drop", "-h"}, {"--help=x", "-h"}, {"-h=x", "--help"}};
    int bad_n[] = {2, 2, 2};
    for (int i = 0; i < 3; i++) {
      char *args[3];
      for (int n = 0; n < bad_n[i]; n++)
        args[n] = (char *)help_bad[i][n];
      Run run = merge_run("auth", args, bad_n[i], old, NULL);
      check_run(&run, 2, false, true);
      run_free(&run);
    }
    Run settings = merge_run("settings", NULL, 0, old, NULL);
    check_run(&settings, 2, false, true);
    run_free(&dropped);
    run_free(&provider);
    run_free(&settings);
    g_free(file);
    rm_tree(root);
    g_free(root);
  }
}

static void test_unsearchable(void) {
  test_name = "unsearchable parent errors";
  if (geteuid() == 0) {
    fprintf(stderr, "SKIP %s\n", test_name);
    return;
  }
  int passes = legacy ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    char *locked = g_build_filename(root, "locked", NULL);
    char *auth = g_build_filename(locked, "auth.json", NULL);
    char *account = g_build_filename(root, "account", NULL);
    char *secret = g_build_filename(locked, "secret", NULL);
    char *dest = g_build_filename(root, "auth.json", NULL);
    CHECK(mkdir(locked, 0700) == 0);
    CHECK(write_text(auth, "{\"keep\":1}"));
    CHECK(write_text(account, "account"));
    CHECK(chmod(locked, 0) == 0);
    char *args[] = {auth};
    Run run = merge_run("auth", args, 1, old, NULL);
    check_run(&run, 1, false, true);
    char *seeded[] = {dest, "--refresh-file", secret, "--account-file", account};
    Run blocked = merge_run("auth", seeded, 5, old, NULL);
    check_run(&blocked, 1, false, true);
    CHECK(chmod(locked, 0700) == 0);
    run_free(&run);
    run_free(&blocked);
    g_free(dest);
    g_free(secret);
    g_free(account);
    g_free(auth);
    g_free(locked);
    rm_tree(root);
    g_free(root);
  }
}

static void test_short_write(void) {
  test_name = "private staging and short writes preserve the final output";
  const char *kinds[] = {"auth", "settings"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    char *path = g_strdup_printf("%s/%s.json", root, kinds[k]);
    char *defaults = g_build_filename(root, "defaults.json", NULL);
    char *body = repeat_text("{\"drop\":1,\"keep\":\"", "x", 10000, "\"}");
    CHECK(body);
    CHECK(write_text(path, body));
    CHECK(write_text(defaults, "{\"new\":true}"));
    char *tmp = g_strdup_printf("%s.tmp", path);
    char **env = fault_env(tmp, "short-write", NULL);
    Run run;
    if (!strcmp(kinds[k], "auth")) {
      char *args[] = {path, "--drop", "drop"};
      run = merge_run("auth", args, 3, false, env);
    } else {
      char *args[] = {defaults, path};
      run = merge_run("settings", args, 2, false, env);
    }
    check_run(&run, 0, false, true);
    CHECK(mode_bits(path) == 0600);
    CHECK(!exists_path(tmp));
    Json *parsed = parse_file(path);
    Json *keep = parsed ? json_get(parsed, "keep", 4) : NULL;
    CHECK(keep && keep->kind == J_STRING && keep->string->len == 10000);
    if (parsed)
      json_free(parsed);
    env_free(env);
    run_free(&run);
    g_free(tmp);
    free(body);
    g_free(defaults);
    g_free(path);
    rm_tree(root);
    g_free(root);
  }
}

static void test_publish_failures(void) {
  test_name = "write/chmod/close/rename failures omit the memory marker";
  const char *modes[] = {"create", "write", "chmod", "close", "rename"};
  for (int m = 0; m < 5; m++) {
    char *root = make_temp("pi-config-");
    CHECK(root);
    if (!root)
      return;
    char *defaults = g_build_filename(root, "defaults.json", NULL);
    char *path = g_build_filename(root, "settings.json", NULL);
    char *marker = g_build_filename(root, ".om-default-off-migrated", NULL);
    CHECK(write_text(defaults, "{\"observational-memory-jev\":{\"enabledByDefault\":false}}"));
    CHECK(write_text(path, "{\"observational-memory-jev\":{\"enabledByDefault\":true}}"));
    size_t before_len = 0;
    char *before = read_file(path, &before_len);
    char *tmp = g_strdup_printf("%s.tmp", path);
    char **env = fault_env(tmp, modes[m], NULL);
    char *args[] = {defaults, path};
    Run run = merge_run("settings", args, 2, false, env);
    check_run(&run, 1, false, true);
    CHECK(bytes_equal(path, before, before_len));
    CHECK(!exists_path(marker));
    if (strcmp(modes[m], "create"))
      CHECK(mode_bits(tmp) == 0600);
    env_free(env);
    Run again = merge_run("settings", args, 2, false, NULL);
    check_run(&again, 0, false, true);
    Json *parsed = parse_file(path);
    Json *entry = parsed ? json_get(parsed, "observational-memory-jev", 24) : NULL;
    Json *flag = entry ? json_get(entry, "enabledByDefault", 16) : NULL;
    CHECK(flag && flag->kind == J_BOOL && !flag->boolean);
    CHECK(exists_path(marker));
    if (parsed)
      json_free(parsed);
    free(before);
    run_free(&run);
    run_free(&again);
    g_free(tmp);
    g_free(marker);
    g_free(path);
    g_free(defaults);
    rm_tree(root);
    g_free(root);
  }
}

static void test_marker_failure(void) {
  test_name = "marker failure does not roll back published settings";
  char *root = make_temp("pi-config-");
  CHECK(root);
  if (!root)
    return;
  char *defaults = g_build_filename(root, "defaults.json", NULL);
  char *path = g_build_filename(root, "settings.json", NULL);
  char *marker = g_build_filename(root, ".om-default-off-migrated", NULL);
  CHECK(write_text(defaults, "{\"observational-memory-jev\":{\"enabledByDefault\":false}}"));
  CHECK(write_text(path, "{\"observational-memory-jev\":{\"enabledByDefault\":true}}"));
  char **env = fault_env(marker, "create", NULL);
  char *args[] = {defaults, path};
  Run run = merge_run("settings", args, 2, false, env);
  check_run(&run, 1, false, true);
  Json *parsed = parse_file(path);
  Json *entry = parsed ? json_get(parsed, "observational-memory-jev", 24) : NULL;
  Json *flag = entry ? json_get(entry, "enabledByDefault", 16) : NULL;
  CHECK(flag && flag->kind == J_BOOL && !flag->boolean);
  CHECK(!exists_path(marker));
  if (parsed)
    json_free(parsed);
  env_free(env);
  Run again = merge_run("settings", args, 2, false, NULL);
  check_run(&again, 0, false, true);
  CHECK(exists_path(marker));
  run_free(&run);
  run_free(&again);
  g_free(marker);
  g_free(path);
  g_free(defaults);
  rm_tree(root);
  g_free(root);
}

static bool js_truthy(Json *value) {
  if (!value || value->kind == J_NULL)
    return false;
  if (value->kind == J_BOOL)
    return value->boolean;
  if (value->kind == J_INT)
    return !(value->string->len == 1 && value->string->str[0] == '0');
  if (value->kind == J_REAL)
    return value->real != 0.0 && !isnan(value->real);
  if (value->kind == J_STRING)
    return value->string->len != 0;
  return true;
}

static void test_staged_private(void) {
  test_name = "staged auth is mode 0600 before credentials are written";
  char *root = make_temp("pi-config-publication-");
  CHECK(root);
  if (!root)
    return;
  char *path = g_build_filename(root, "auth.json", NULL);
  char *tmp = g_strdup_printf("%s.tmp", path);
  char *release = g_build_filename(root, "release", NULL);
  CHECK(write_text(path, "{\"keep\":1}"));
  CHECK(write_text(tmp, "old"));
  CHECK(chmod(tmp, 0666) == 0);
  CHECK(write_seed(root));
  GPtrArray *list = g_ptr_array_new();
  g_ptr_array_add(list, g_strdup(path));
  add_seed(list, root);
  int nargs = 0;
  char **args = args_finish(list, &nargs);
  char *script = NULL;
  char *program = kind_program("auth", false, &script);
  char **argv = g_new0(char *, (guint)nargs + 2);
  argv[0] = program;
  for (int i = 0; i < nargs; i++)
    argv[i + 1] = args[i];
  char **env = fault_env(tmp, "", release);
  Child child;
  CHECK(spawn_child(&child, NULL, argv, env));
  if (child.pid) {
    for (int i = 0; i < 500 && mode_bits(tmp) != 0600; i++) {
      child_pump(&child);
      usleep(5000);
    }
    CHECK(mode_bits(tmp) == 0600);
    CHECK(file_json_equals(path, "{\"keep\":1}"));
    CHECK(write_text(release, "go"));
    bool error = false;
    int status = wait_child(&child, 15000, &error);
    CHECK(!error && status == 0);
    Json *parsed = parse_file(path);
    Json *entry = parsed ? json_get(parsed, "openai-codex", 12) : NULL;
    CHECK(js_truthy(entry));
    if (parsed)
      json_free(parsed);
  }
  env_free(env);
  g_free(argv);
  g_free(program);
  args_free(args);
  g_free(release);
  g_free(tmp);
  g_free(path);
  rm_tree(root);
  g_free(root);
}

static Run bus_run(const char *url, bool present, bool old) {
  GPtrArray *env = env_inherited();
  env_set(env, "PI_AGENT_BUS_URL", present ? url : NULL);
  char **envp = env_finish(env);
  Run run;
  if (!old) {
    char *argv[] = {(char *)bus_bin, NULL};
    run = run_argv(NULL, argv, envp, 10000);
  } else {
    char *argv[] = {(char *)bus_python, (char *)bus_legacy, NULL};
    run = run_argv(NULL, argv, envp, 10000);
  }
  env_free(envp);
  return run;
}

static bool host_equals(const Run *run, const char *host) {
  size_t n = strlen(host);
  return strlen(run->out) == n + 1 && !memcmp(run->out, host, n) && run->out[n] == '\n';
}

static void check_host(const char *url, bool present, const char *host) {
  Run run = bus_run(url, present, false);
  if (check_run(&run, 0, true, false))
    CHECK(host_equals(&run, host));
  if (bus_legacy) {
    Run old = bus_run(url, present, true);
    if (check_run(&old, 0, true, false))
      CHECK(host_equals(&old, host));
    run_free(&old);
  }
  run_free(&run);
}

static void test_bus_defaults(void) {
  test_name = "unset and empty URLs use the client default";
  const char *urls[] = {NULL, "", "http://terminus:7420", "https://TERMINUS:7420/"};
  for (int i = 0; i < 4; i++)
    check_host(urls[i], urls[i] != NULL, "terminus");
  const char *schemes[] = {"HTTP", "Https", "httpS", "ftp", "ws"};
  for (int i = 0; i < 5; i++) {
    char *url = g_strdup_printf("%s://terminus:7420", schemes[i]);
    check_host(url, true, "");
    g_free(url);
  }
  check_host("http://bus.EXAMPLE:7420/base", true, "bus.example");
}

static void test_bus_labels(void) {
  test_name = "label boundaries match the original host spelling";
  char *long_label = repeat_text("", "a", 63, "");
  char *too_long = repeat_text("", "a", 64, "");
  char *dotted = repeat_text("", "b", 63, ".");
  char *prefix = long_label ? g_strdup_printf("%s.", long_label) : NULL;
  char *many = prefix && dotted ? repeat_text(prefix, dotted, 4, "z") : NULL;
  g_free(prefix);
  char *accepted[7];
  accepted[0] = g_strdup("a");
  accepted[1] = g_strdup("9z");
  accepted[2] = g_strdup("a-9");
  accepted[3] = long_label;
  accepted[4] = many;
  accepted[5] = g_strdup("example.0xz");
  accepted[6] = g_strdup("0x.example");
  for (int i = 0; i < 7; i++) {
    if (!accepted[i])
      continue;
    char *url = g_strdup_printf("http://%s", accepted[i]);
    check_host(url, true, accepted[i]);
    g_free(url);
  }
  char *rejected[14];
  rejected[0] = g_strdup(".");
  rejected[1] = g_strdup("a.");
  rejected[2] = g_strdup(".a");
  rejected[3] = g_strdup("a..b");
  rejected[4] = g_strdup("-a");
  rejected[5] = g_strdup("a-");
  rejected[6] = g_strdup("a.-b");
  rejected[7] = g_strdup("a.b-");
  rejected[8] = too_long;
  rejected[9] = g_strdup("a_b");
  rejected[10] = g_strdup("bücher.example");
  rejected[11] = g_strdup("尾.example");
  rejected[12] = g_strdup("xn--");
  rejected[13] = g_strdup("[::1]");
  for (int i = 0; i < 14; i++) {
    if (!rejected[i])
      continue;
    char *url = g_strdup_printf("http://%s", rejected[i]);
    check_host(url, true, "");
    g_free(url);
    if (rejected[i] != too_long)
      g_free(rejected[i]);
  }
  for (int i = 0; i < 7; i++)
    if (accepted[i] != long_label && accepted[i] != many)
      g_free(accepted[i]);
  free(long_label);
  free(too_long);
  free(dotted);
  free(many);
}

static void test_bus_ipv4(void) {
  test_name = "only canonical dotted IPv4 is accepted";
  const char *good[] = {"0.0.0.0", "127.0.0.1", "255.255.255.255", "192.168.1.9"};
  for (int i = 0; i < 4; i++) {
    char *url = g_strdup_printf("http://%s:7420", good[i]);
    check_host(url, true, good[i]);
    g_free(url);
  }
  const char *bad[] = {"127.1",
                       "2130706433",
                       "0177.0.0.1",
                       "127.00.0.1",
                       "0x7f000001",
                       "0X7F000001",
                       "127.0.0.256",
                       "256.0.0.1",
                       "1.2.3.4.5",
                       "bus.123",
                       "bus.0x",
                       "bus.0xabc",
                       "0",
                       "1",
                       "0.0.0.00",
                       "1.2.3.999999999999999999999999"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char *url = g_strdup_printf("http://%s", bad[i]);
    check_host(url, true, "");
    g_free(url);
  }
}

static void test_bus_ports(void) {
  test_name = "ports retain the ASCII-digit grammar";
  char *long_port = repeat_text("", "9", 200, "");
  const char *good[] = {"0", "00080", "65535", "65536", long_port};
  for (int i = 0; i < 5; i++) {
    if (!good[i])
      continue;
    char *url = g_strdup_printf("https://bus.example:%s/p", good[i]);
    check_host(url, true, "bus.example");
    g_free(url);
  }
  free(long_port);
  const char *bad[] = {"", "-1", "+80", "0x50", "８０", "1:2", "80.0", " 80"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char *url = g_strdup_printf("http://bus.example:%s", bad[i]);
    check_host(url, true, "");
    g_free(url);
  }
}

static char *url_with_cp(const char *prefix, uint32_t cp, const char *suffix) {
  GString *url = g_string_new(prefix);
  append_cp(url, cp);
  g_string_append(url, suffix);
  return g_string_free(url, false);
}

static void test_bus_paths(void) {
  test_name = "paths reject Python whitespace, queries, fragments and backslashes";
  const char *good[] = {"", "/", "//x", "/尾🙂", "/%3f%23%20", NULL, NULL, NULL, "/a:b@c"};
  char *zwsp = url_with_cp("http://bus.example:7420/", 0x200b, "");
  char *word = url_with_cp("http://bus.example:7420/", 0x2060, "");
  char *esc = url_with_cp("http://bus.example:7420/", 0x1b, "");
  char *owned[9];
  for (int i = 0; i < 9; i++)
    owned[i] = good[i] ? g_strdup_printf("http://bus.example:7420%s", good[i]) : NULL;
  owned[5] = zwsp;
  owned[6] = word;
  owned[7] = esc;
  for (int i = 0; i < 9; i++) {
    check_host(owned[i], true, "bus.example");
    g_free(owned[i]);
  }
  uint32_t bad[] = {' ',    '\t',   '\n',   '\r',   '\v',   '\f',   0x1c,
                    0x1d,   0x1e,   0x1f,   0x85,   0xa0,   0x1680, 0x2000,
                    0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007,
                    0x2008, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f,
                    0x3000, '?',    '#',    '\\'};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char *url = url_with_cp("http://bus.example/a", bad[i], "b");
    check_host(url, true, "");
    g_free(url);
  }
}

static void test_bus_ambiguous(void) {
  test_name = "credentials and ambiguous authority spellings fail silently";
  const char *urls[] = {
      " http://bus",
      "http://bus ",
      "http://bus\n",
      "http://user:fake-pass@bus:7420",
      "http://user@bus",
      "http://bad,host",
      "http://bus\\@127.1",
      "http://[bad",
      "http:///bus",
      "//bus",
      "http://%62us",
      "http://bus?query",
      "http://bus#fragment",
      "http://bus:80?query",
  };
  for (size_t i = 0; i < sizeof urls / sizeof urls[0]; i++)
    check_host(urls[i], true, "");
}

static bool host_pattern(const char *text) {
  size_t n = strlen(text);
  if (!n || text[n - 1] != '\n')
    return false;
  for (size_t i = 0; i + 1 < n; i++) {
    char c = text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
      return false;
  }
  return true;
}

static void test_bus_corpus(void) {
  test_name = "deterministic authority corpus";
  uint32_t seed = 37;
  const char *chars = "abcXYZ09.-_:@/\\?#% ";
  size_t nchars = strlen(chars);
  for (int i = 0; i < 180; i++) {
    int length = 1 + (i % 30);
    GString *text = g_string_new("http://");
    for (int j = 0; j < length; j++) {
      seed = seed * 1664525u + 1013904223u;
      g_string_append_c(text, chars[seed % nchars]);
    }
    Run run = bus_run(text->str, true, false);
    if (check_run(&run, 0, true, false))
      CHECK(host_pattern(run.out));
    if (bus_legacy) {
      Run old = bus_run(text->str, true, true);
      if (check_run(&old, 0, true, false))
        CHECK(strlen(run.out) == strlen(old.out) && !strcmp(run.out, old.out));
      run_free(&old);
    }
    run_free(&run);
    g_string_free(text, true);
  }
}

static char *hex_encode(const unsigned char *data, size_t n) {
  char *out = malloc(n * 2 + 1);
  if (!out)
    return NULL;
  const char *digits = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = digits[data[i] >> 4];
    out[i * 2 + 1] = digits[data[i] & 15];
  }
  out[n * 2] = 0;
  return out;
}

static void test_bus_invalid_utf8(void) {
  test_name = "invalid UTF-8 environment bytes never become host characters";
  const unsigned char raw[] = {255, 0xed, 0xa0, 0x80};
  char *hex = hex_encode(raw, sizeof raw);
  CHECK(hex);
  if (!hex)
    return;
  const char *prefixes[] = {"http://bus.example/path-", "http://bus-"};
  const char *suffixes[] = {"", ".example"};
  const char *expected[] = {"bus.example\n", "\n"};
  int passes = bus_legacy ? 2 : 1;
  for (int i = 0; i < 2; i++) {
    GString *bytes = g_string_new(prefixes[i]);
    g_string_append_len(bytes, (const char *)raw, sizeof raw);
    g_string_append(bytes, suffixes[i]);
    char *encoded = hex_encode((const unsigned char *)bytes->str, bytes->len);
    g_string_free(bytes, true);
    CHECK(encoded);
    if (!encoded)
      continue;
    for (int old = 0; old < passes; old++) {
      char *argv[5];
      argv[0] = (char *)bus_argv;
      argv[1] = encoded;
      if (old) {
        argv[2] = (char *)bus_python;
        argv[3] = (char *)bus_legacy;
        argv[4] = NULL;
      } else {
        argv[2] = (char *)bus_bin;
        argv[3] = NULL;
      }
      Run run = run_argv(NULL, argv, NULL, 10000);
      if (check_run(&run, 0, true, false))
        CHECK(!strcmp(run.out, expected[i]));
      run_free(&run);
    }
    free(encoded);
  }
  free(hex);
}

static int wrapper_stub(int argc, char **argv) {
  const char *keys[] = {
      "PI_AGENT_BUS_URL", "PI_AGENT_BUS_TOKEN", "PI_AGENT_BUS_ENABLED",
      "PI_AGENT_BUS_CONTROL", "PI_AGENT_BUS_OPERATOR_READ",
      "PI_AGENT_BUS_OPERATOR_HISTORY", "TYPESAFE_API_KEY", "NO_PROXY", "no_proxy",
      "CAPTURE_BRANCH"};
  GString *out = g_string_new("{\"args\":[");
  for (int i = 2; i < argc; i++) {
    if (i > 2)
      g_string_append_c(out, ',');
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)argv[i]; *p; p++) {
      if (*p == '"' || *p == '\\') {
        g_string_append_c(out, '\\');
        g_string_append_c(out, (char)*p);
      } else if (*p < 0x20) {
        char buf[7];
        const char *digits = "0123456789abcdef";
        buf[0] = '\\';
        buf[1] = 'u';
        buf[2] = '0';
        buf[3] = '0';
        buf[4] = digits[*p >> 4];
        buf[5] = digits[*p & 15];
        g_string_append_len(out, buf, 6);
      } else
        g_string_append_c(out, (char)*p);
    }
    g_string_append_c(out, '"');
  }
  g_string_append(out, "],\"env\":{");
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    if (i)
      g_string_append_c(out, ',');
    g_string_append_c(out, '"');
    g_string_append(out, keys[i]);
    g_string_append(out, "\":");
    const char *value = getenv(keys[i]);
    if (!value)
      g_string_append(out, "null");
    else {
      g_string_append_c(out, '"');
      for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') {
          g_string_append_c(out, '\\');
          g_string_append_c(out, (char)*p);
        } else
          g_string_append_c(out, (char)*p);
      }
      g_string_append_c(out, '"');
    }
  }
  g_string_append(out, "}}\n");
  fwrite(out->str, 1, out->len, stdout);
  g_string_free(out, true);
  return 0;
}

static bool js_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static char *replace_store(const char *text, const char *tool, const char *repl,
                           bool boundary) {
  GString *out = g_string_new(NULL);
  const char *p = text;
  size_t tlen = strlen(tool);
  while (*p) {
    const char *store = strstr(p, "/nix/store/");
    if (!store) {
      g_string_append(out, p);
      break;
    }
    const char *bin = strstr(store, "/bin/");
    bool clean = bin != NULL;
    if (clean)
      for (const char *q = store; q < bin; q++)
        if (js_space((unsigned char)*q) || *q == '\'' || *q == '"')
          clean = false;
    const char *name = clean ? bin + 5 : NULL;
    if (!clean || strncmp(name, tool, tlen) ||
        (boundary && !js_space((unsigned char)name[tlen]))) {
      g_string_append_len(out, p, (gssize)(store - p + 1));
      p = store + 1;
      continue;
    }
    g_string_append_len(out, p, (gssize)(store - p));
    g_string_append(out, repl);
    p = name + tlen;
  }
  return g_string_free(out, false);
}

static bool store_tool_remains(const char *text) {
  const char *tools[] = {"read-sops-secret", "prompt-capture", "pi"};
  const char *p = text;
  while ((p = strstr(p, "/nix/store/"))) {
    const char *bin = strstr(p, "/bin/");
    bool clean = bin != NULL;
    if (clean)
      for (const char *q = p; q < bin; q++)
        if (js_space((unsigned char)*q) || *q == '\'' || *q == '"')
          clean = false;
    if (clean) {
      const char *name = bin + 5;
      for (int i = 0; i < 3; i++) {
        size_t n = strlen(tools[i]);
        if (!strncmp(name, tools[i], n) && js_space((unsigned char)name[n]))
          return true;
      }
    }
    p++;
  }
  return false;
}

static bool write_mode(const char *path, const char *text, mode_t mode) {
  return write_text(path, text) && chmod(path, mode) == 0;
}

typedef struct {
  const char *const *env_keys;
  const char *const *env_values;
  int env_n;
  const char *const *args;
  int arg_n;
  bool public_profile;
  bool parser_failure;
  bool disabled;
} Launch;

static bool string_is(Json *value, const char *text) {
  if (!text)
    return value && value->kind == J_NULL;
  return value && value->kind == J_STRING && value->string->len == strlen(text) &&
         !memcmp(value->string->str, text, value->string->len);
}

static int count_line(GPtrArray *lines, const char *text) {
  int count = 0;
  for (guint i = 0; i < lines->len; i++)
    if (!strcmp(lines->pdata[i], text))
      count++;
  return count;
}

static bool launch_wrapper(const Launch *spec, Json **result, GPtrArray **reads) {
  *result = NULL;
  *reads = g_ptr_array_new_with_free_func(g_free);
  if (!wrappers)
    return false;
  Json *source = json_get(wrappers, spec->public_profile ? "public" : "foundation",
                          spec->public_profile ? 6 : 10);
  if (!source || source->kind != J_STRING) {
    fail_msg(__FILE__, __LINE__, "wrapper text missing");
    return false;
  }
  const char *text = source->string->str;
  if (!strstr(text, "/pi-capture-bus-host") || strstr(text, "python")) {
    fail_msg(__FILE__, __LINE__, "wrapper text does not match capture contract");
    return false;
  }
  char *root = make_temp("pi-bus-wrapper-");
  if (!root)
    return false;
  char *pi = g_build_filename(root, "pi", NULL);
  char *capture = g_build_filename(root, "capture", NULL);
  char *secret = g_build_filename(root, "secret", NULL);
  char *failed = g_build_filename(root, "failed", NULL);
  char *script = g_strdup_printf("#!/bin/sh\nexec '%s' --wrapper-stub \"$@\"\n", self_exe);
  bool files = write_mode(pi, script, 0700) &&
               write_mode(capture,
                          "#!/usr/bin/env bash\n[[ $1 == pi && $2 == -- ]] || exit 73\n"
                          "shift 2\nexport CAPTURE_BRANCH=1\nexec \"$@\"\n",
                          0700) &&
               write_mode(secret,
                          "#!/usr/bin/env bash\nprintf \"%s\\n\" \"${1##*/}\" >> "
                          "\"$FIXTURE_READS\"\nprintf \"%s\\n\" fixture-secret\n"
                          "exit \"${SECRET_STATUS:-0}\"\n",
                          0700) &&
               write_mode(failed, "#!/usr/bin/env bash\nexit 1\n", 0700);
  CHECK(files);
  char *replaced = replace_store(text, "read-sops-secret", secret, false);
  char *next = replace_store(replaced, "prompt-capture", capture, false);
  g_free(replaced);
  replaced = replace_store(next, "pi-capture-bus-host",
                           spec->parser_failure ? failed : bus_bin, false);
  g_free(next);
  next = replace_store(replaced, "pi", pi, true);
  g_free(replaced);
  if (store_tool_remains(next))
    fail_msg(__FILE__, __LINE__, "store command remained");
  if (spec->disabled) {
    if (!strstr(next, "bus_enabled=1"))
      fail_msg(__FILE__, __LINE__, "bus_enabled=1 missing");
    char *at = strstr(next, "bus_enabled=1");
    if (at)
      memcpy(at, "bus_enabled=0", 13);
  }
  char *wrapper = g_build_filename(root, "wrapper", NULL);
  CHECK(write_mode(wrapper, next, 0700));
  char *reads_path = g_build_filename(root, "reads", NULL);
  GPtrArray *env = g_ptr_array_new();
  const char *path = getenv("PATH");
  env_set(env, "PATH", path ? path : "/usr/bin");
  env_set(env, "HOME", root);
  env_set(env, "FIXTURE_READS", reads_path);
  for (int i = 0; i < spec->env_n; i++)
    env_set(env, spec->env_keys[i], spec->env_values[i]);
  char **envp = env_finish(env);
  char **argv = g_new0(char *, (guint)spec->arg_n + 3);
  argv[0] = g_strdup("bash");
  argv[1] = g_strdup(wrapper);
  for (int i = 0; i < spec->arg_n; i++)
    argv[i + 2] = g_strdup(spec->args[i]);
  Run run = run_argv(NULL, argv, envp, 10000);
  bool ok = check_run(&run, 0, true, false);
  if (ok) {
    *result = json_parse(run.out);
    if (!*result)
      fail_msg(__FILE__, __LINE__, "wrapper stub output was not JSON");
  }
  if (exists_path(reads_path)) {
    size_t n = 0;
    char *data = read_file(reads_path, &n);
    if (data) {
      size_t start = 0, end = n;
      while (start < end &&
             (data[start] == ' ' || data[start] == '\n' || data[start] == '\r' ||
              data[start] == '\t'))
        start++;
      while (end > start &&
             (data[end - 1] == ' ' || data[end - 1] == '\n' || data[end - 1] == '\r' ||
              data[end - 1] == '\t'))
        end--;
      if (start == end)
        g_ptr_array_add(*reads, g_strdup(""));
      else
        for (size_t i = start; i < end;) {
          size_t stop = i;
          while (stop < end && data[stop] != '\n')
            stop++;
          g_ptr_array_add(*reads, g_strndup(data + i, stop - i));
          i = stop < end ? stop + 1 : stop;
        }
      free(data);
    }
  }
  run_free(&run);
  for (char **item = argv; *item; item++)
    g_free(*item);
  g_free(argv);
  env_free(envp);
  g_free(reads_path);
  g_free(wrapper);
  g_free(next);
  g_free(script);
  g_free(failed);
  g_free(secret);
  g_free(capture);
  g_free(pi);
  rm_tree(root);
  g_free(root);
  return ok && *result;
}

static bool env_is(Json *result, const char *key, const char *expected) {
  Json *env = result ? json_get(result, "env", 3) : NULL;
  Json *value = env ? json_get(env, key, strlen(key)) : NULL;
  return string_is(value, expected);
}

static void test_wrapper_proxy(void) {
  test_name = "rendered wrappers preserve URL spelling and combine proxy lists";
  if (!wrappers) {
    fprintf(stderr, "SKIP %s\n", test_name);
    return;
  }
  const char *urls[] = {"http://BUS.example:7420/base", ""};
  for (int public_profile = 0; public_profile < 2; public_profile++)
    for (int u = 0; u < 2; u++) {
      const char *keys[] = {"CAPTURE_PROMPTS", "PI_AGENT_BUS_URL", "NO_PROXY", "no_proxy"};
      const char *values[] = {"1", urls[u], "upper", "lower"};
      const char *args[] = {"--test-marker", "value"};
      Launch spec = {keys, values, 4, args, 2, public_profile, false, false};
      Json *result = NULL;
      GPtrArray *reads = NULL;
      if (!launch_wrapper(&spec, &result, &reads)) {
        if (result)
          json_free(result);
        if (reads)
          g_ptr_array_free(reads, true);
        continue;
      }
      CHECK(env_is(result, "PI_AGENT_BUS_URL", urls[u][0] ? urls[u] : ""));
      const char *host = urls[u][0] ? "bus.example" : "terminus";
      char *proxy = g_strdup_printf("upper,lower,%s", host);
      CHECK(env_is(result, "NO_PROXY", proxy));
      CHECK(env_is(result, "no_proxy", proxy));
      CHECK(env_is(result, "CAPTURE_BRANCH", "1"));
      Json *got_args = json_get(result, "args", 4);
      CHECK(got_args && got_args->kind == J_ARRAY && got_args->values->len >= 2);
      if (got_args && got_args->values->len >= 2) {
        CHECK(string_is(got_args->values->pdata[got_args->values->len - 2], "--test-marker"));
        CHECK(string_is(got_args->values->pdata[got_args->values->len - 1], "value"));
      }
      g_free(proxy);
      json_free(result);
      g_ptr_array_free(reads, true);
    }
  const char *keys[] = {"NO_PROXY", "no_proxy"};
  const char *values[] = {"upper", "lower"};
  Launch plain = {keys, values, 2, NULL, 0, false, false, false};
  Json *result = NULL;
  GPtrArray *reads = NULL;
  if (launch_wrapper(&plain, &result, &reads)) {
    CHECK(env_is(result, "NO_PROXY", "upper"));
    CHECK(env_is(result, "no_proxy", "lower"));
    CHECK(env_is(result, "CAPTURE_BRANCH", NULL));
    json_free(result);
  }
  if (reads)
    g_ptr_array_free(reads, true);
}

static void test_wrapper_unsupported(void) {
  test_name = "unsupported parsing disables only capture participation";
  if (!wrappers) {
    fprintf(stderr, "SKIP %s\n", test_name);
    return;
  }
  const char *urls[] = {"http://127.1:7420", "http://user@bus", "http://[::1]:7420",
                        "http://bücher.example:7420"};
  for (int u = 0; u < 4; u++)
    for (int capture = 0; capture < 2; capture++) {
      const char *keys[] = {"CAPTURE_PROMPTS", "PI_AGENT_BUS_URL"};
      const char *values[] = {capture ? "1" : "0", urls[u]};
      Launch spec = {keys, values, 2, NULL, 0, false, false, false};
      Json *result = NULL;
      GPtrArray *reads = NULL;
      if (!launch_wrapper(&spec, &result, &reads)) {
        if (result)
          json_free(result);
        if (reads)
          g_ptr_array_free(reads, true);
        continue;
      }
      CHECK(env_is(result, "PI_AGENT_BUS_URL", urls[u]));
      CHECK(env_is(result, "PI_AGENT_BUS_ENABLED", capture ? "0" : NULL));
      CHECK(env_is(result, "NO_PROXY", NULL));
      json_free(result);
      g_ptr_array_free(reads, true);
    }
  for (int capture = 0; capture < 2; capture++) {
    const char *keys[] = {"CAPTURE_PROMPTS"};
    const char *values[] = {capture ? "1" : "0"};
    Launch spec = {keys, values, 1, NULL, 0, false, true, false};
    Json *result = NULL;
    GPtrArray *reads = NULL;
    if (!launch_wrapper(&spec, &result, &reads)) {
      if (result)
        json_free(result);
      if (reads)
        g_ptr_array_free(reads, true);
      continue;
    }
    CHECK(env_is(result, "PI_AGENT_BUS_ENABLED", capture ? "0" : NULL));
    CHECK(env_is(result, "CAPTURE_BRANCH", capture ? "1" : NULL));
    json_free(result);
    g_ptr_array_free(reads, true);
  }
}

static void expect_no_token(const Launch *spec) {
  Json *result = NULL;
  GPtrArray *reads = NULL;
  if (!launch_wrapper(spec, &result, &reads)) {
    if (result)
      json_free(result);
    if (reads)
      g_ptr_array_free(reads, true);
    return;
  }
  CHECK(count_line(reads, "PI_AGENT_BUS_TOKEN") == 0);
  CHECK(env_is(result, "PI_AGENT_BUS_TOKEN", NULL));
  json_free(result);
  g_ptr_array_free(reads, true);
}

static void test_wrapper_offline(void) {
  test_name = "offline and disabled bus gates suppress bus-secret reads";
  if (!wrappers) {
    fprintf(stderr, "SKIP %s\n", test_name);
    return;
  }
  const char *offline_values[] = {"1", "true", "YES", " true ", "yes "};
  for (int capture = 0; capture < 2; capture++) {
    const char *capture_text = capture ? "1" : "0";
    const char *offline_args[] = {"--offline"};
    const char *keys_capture[] = {"CAPTURE_PROMPTS"};
    const char *values_capture[] = {capture_text};
    Launch offline = {keys_capture, values_capture, 1, offline_args, 1, false, false, false};
    expect_no_token(&offline);
    const char *enabled_keys[] = {"PI_AGENT_BUS_ENABLED", "CAPTURE_PROMPTS"};
    const char *enabled_values[] = {"0", capture_text};
    Launch enabled = {enabled_keys, enabled_values, 2, NULL, 0, false, false, false};
    expect_no_token(&enabled);
    Launch disabled = {keys_capture, values_capture, 1, NULL, 0, false, false, true};
    expect_no_token(&disabled);
    for (int i = 0; i < 5; i++) {
      const char *keys[] = {"PI_OFFLINE", "CAPTURE_PROMPTS"};
      const char *values[] = {offline_values[i], capture_text};
      Launch item = {keys, values, 2, NULL, 0, false, false, false};
      expect_no_token(&item);
    }
  }
  const char *kept[] = {"0", "false", "no", "", "t rue"};
  for (size_t i = 0; i < sizeof kept / sizeof kept[0]; i++) {
    const char *keys[] = {"PI_OFFLINE"};
    const char *values[] = {kept[i]};
    Launch spec = {keys, values, 1, NULL, 0, false, false, false};
    Json *result = NULL;
    GPtrArray *reads = NULL;
    if (!launch_wrapper(&spec, &result, &reads)) {
      if (result)
        json_free(result);
      if (reads)
        g_ptr_array_free(reads, true);
      continue;
    }
    CHECK(count_line(reads, "PI_AGENT_BUS_TOKEN") == 1);
    json_free(result);
    g_ptr_array_free(reads, true);
  }
}

static void test_wrapper_credentials(void) {
  test_name = "explicit credentials and public profile boundaries";
  if (!wrappers) {
    fprintf(stderr, "SKIP %s\n", test_name);
    return;
  }
  for (int capture = 0; capture < 2; capture++) {
    const char *capture_text = capture ? "1" : "0";
    const char *keys[] = {"CAPTURE_PROMPTS", "PI_AGENT_BUS_TOKEN", "TYPESAFE_API_KEY"};
    const char *values[] = {capture_text, "caller-token", "caller-key"};
    Launch explicit = {keys, values, 3, NULL, 0, false, false, false};
    Json *result = NULL;
    GPtrArray *reads = NULL;
    if (launch_wrapper(&explicit, &result, &reads)) {
      CHECK(reads->len == 0);
      CHECK(env_is(result, "PI_AGENT_BUS_TOKEN", "caller-token"));
      CHECK(env_is(result, "TYPESAFE_API_KEY", "caller-key"));
      json_free(result);
    }
    if (reads)
      g_ptr_array_free(reads, true);
    const char *fail_keys[] = {"CAPTURE_PROMPTS", "SECRET_STATUS"};
    const char *fail_values[] = {capture_text, "1"};
    Launch failed = {fail_keys, fail_values, 2, NULL, 0, false, false, false};
    result = NULL;
    reads = NULL;
    if (launch_wrapper(&failed, &result, &reads)) {
      CHECK(count_line(reads, "PI_AGENT_BUS_TOKEN") == 1);
      CHECK(env_is(result, "PI_AGENT_BUS_TOKEN", NULL));
      CHECK(env_is(result, "TYPESAFE_API_KEY", NULL));
      json_free(result);
    }
    if (reads)
      g_ptr_array_free(reads, true);
    const char *public_keys[] = {"CAPTURE_PROMPTS"};
    const char *public_values[] = {capture_text};
    Launch public_profile = {public_keys, public_values, 1, NULL, 0, true, false, false};
    result = NULL;
    reads = NULL;
    if (launch_wrapper(&public_profile, &result, &reads)) {
      CHECK(reads->len == 0);
      CHECK(env_is(result, "PI_AGENT_BUS_TOKEN", NULL));
      CHECK(env_is(result, "PI_AGENT_BUS_URL", NULL));
      CHECK(env_is(result, "NO_PROXY", capture ? "terminus" : NULL));
      json_free(result);
    }
    if (reads)
      g_ptr_array_free(reads, true);
  }
}

static void load_wrappers(void) {
  const char *path = getenv("PI_BUS_HOST_WRAPPERS");
  if (!path || !*path)
    return;
  size_t n = 0;
  char *data = read_file(path, &n);
  if (!data || memchr(data, 0, n)) {
    fail_msg(__FILE__, __LINE__, "PI_BUS_HOST_WRAPPERS unreadable");
    free(data);
    return;
  }
  wrappers = json_parse(data);
  if (!wrappers)
    fail_msg(__FILE__, __LINE__, "PI_BUS_HOST_WRAPPERS was not JSON");
  free(data);
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--wrapper-stub"))
    return wrapper_stub(argc, argv);
  bins = getenv("PI_CONFIG_BIN_DIR");
  failure_lib = getenv("PI_CONFIG_FAILURES");
  legacy = getenv("PI_CONFIG_LEGACY");
  python = getenv("PI_CONFIG_PYTHON");
  bus_bin = getenv("PI_BUS_HOST_BIN");
  bus_argv = getenv("PI_BUS_HOST_ARGV");
  bus_legacy = getenv("PI_BUS_HOST_LEGACY");
  bus_python = getenv("PI_BUS_HOST_PYTHON");
  if (!python || !*python)
    python = "python3";
  if (!bus_python || !*bus_python)
    bus_python = "python3";
  if (!bins || !*bins || !failure_lib || !*failure_lib || !bus_bin || !*bus_bin ||
      !bus_argv || !*bus_argv) {
    fprintf(stderr, "set native binary directory and failure library; set "
                    "PI_BUS_HOST_BIN and PI_BUS_HOST_ARGV\n");
    return 1;
  }
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0) {
    fprintf(stderr, "could not resolve check executable\n");
    return 1;
  }
  buf[n] = 0;
  self_exe = buf;
  load_wrappers();
  test_fill();
  test_retarget();
  test_unhashable();
  test_noop();
  test_migration();
  test_marker_only();
  test_marker_links();
  test_auth_seed();
  test_oauth_retain();
  test_missing_secrets();
  test_secret_interior();
  test_secret_whitespace();
  test_secrets_before_load();
  test_malformed();
  test_round_trip();
  test_real_corpus();
  test_unbounded_input();
  test_pretty_depth();
  test_negative_providers();
  test_path_links();
  test_cli();
  test_unsearchable();
  test_short_write();
  test_publish_failures();
  test_marker_failure();
  test_staged_private();
  test_bus_defaults();
  test_bus_labels();
  test_bus_ipv4();
  test_bus_ports();
  test_bus_paths();
  test_bus_ambiguous();
  test_bus_corpus();
  test_bus_invalid_utf8();
  test_wrapper_proxy();
  test_wrapper_unsupported();
  test_wrapper_offline();
  test_wrapper_credentials();
  if (wrappers)
    json_free(wrappers);
  if (failures) {
    fprintf(stderr, "%d pi-config checks failed\n", failures);
    return 1;
  }
  printf("pi-config checks passed\n");
  return 0;
}
