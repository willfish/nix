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

enum Kind { J_NULL, J_BOOL, J_INT, J_REAL, J_STRING, J_ARRAY, J_OBJECT };
struct Json {
  enum Kind kind;
  bool boolean;
  double real;
  GString *string;
  GPtrArray *values, *keys;
};

static int failures;
static const char *test_name;
static const char *bins;
static const char *failure_lib;
static const char *raw_argv;
static const char *legacy;
static const char *python;

static void fail_msg(const char *file, int line, const char *msg) {
  fprintf(stderr, "FAIL %s:%d %s: %s\n", file, line, test_name, msg);
  failures++;
}
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond))                                                               \
      fail_msg(__FILE__, __LINE__, #cond);                                    \
  } while (0)

static char *snippet(const char *text, size_t n) {
  size_t keep = n > 240 ? 240 : n;
  char *out = g_malloc(keep + 1);
  if (!out)
    return g_strdup("");
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

static char **env_with(const char *const *keys, const char *const *values,
                       int count, bool inherit) {
  GPtrArray *env = g_ptr_array_new();
  if (inherit)
    for (char **item = environ; item && *item; item++)
      g_ptr_array_add(env, g_strdup(*item));
  for (int i = 0; i < count; i++)
    env_set(env, keys[i], values[i]);
  g_ptr_array_add(env, NULL);
  char **out = g_new(char *, env->len);
  for (guint i = 0; i < env->len; i++)
    out[i] = env->pdata[i];
  g_ptr_array_free(env, false);
  return out;
}

static void env_free(char **env) {
  if (!env)
    return;
  for (char **item = env; *item; item++)
    g_free(*item);
  g_free(env);
}

static bool spawn_child(Child *child, const char *cwd, char **argv,
                        char **env) {
  int outp[2], errp[2];
  memset(child, 0, sizeof *child);
  if (pipe(outp) != 0 || pipe(errp) != 0)
    return false;
  pid_t pid = fork();
  if (pid < 0)
    return false;
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

static char *exe_path(const char *kind) {
  return g_strdup_printf("%s/hypr-%s-settings", bins, kind);
}

static char *legacy_path(const char *kind) {
  GString *out = g_string_new(legacy);
  g_string_replace(out, "{kind}", kind, 0);
  return g_string_free(out, false);
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
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
      rm_tree(child);
    else
      unlink(child);
    g_free(child);
  }
  closedir(dir);
  rmdir(path);
}

static char *make_temp(void) {
  char *path = g_strdup("/tmp/panel-settings-XXXXXX");
  if (!path || !mkdtemp(path)) {
    g_free(path);
    return NULL;
  }
  return path;
}

static bool is_temp_name(const char *name) {
  if (name[0] != '.')
    return false;
  size_t i = 1;
  if (!(isalnum((unsigned char)name[i]) || name[i] == '_'))
    return false;
  while (isalnum((unsigned char)name[i]) || name[i] == '_')
    i++;
  return strncmp(name + i, "-settings-", 10) == 0;
}

static GPtrArray *temps(const char *path) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  DIR *dir = opendir(path);
  if (!dir)
    return out;
  struct dirent *ent;
  while ((ent = readdir(dir)))
    if (is_temp_name(ent->d_name))
      g_ptr_array_add(out, g_strdup(ent->d_name));
  closedir(dir);
  return out;
}

static char *preload_value(void) {
  const char *asan = getenv("PANEL_SETTINGS_ASAN_RT");
  if (asan && *asan)
    return g_strdup_printf("%s:%s", asan, failure_lib);
  return g_strdup(failure_lib);
}

static char **fault_env(const char *mode, const char *release) {
  char *preload = preload_value();
  const char *keys[] = {"LD_PRELOAD", "PANEL_FIXTURE_FAIL",
                        "PANEL_FIXTURE_RELEASE"};
  const char *values[] = {preload, mode, release};
  char **env = env_with(keys, values, release ? 3 : 2, true);
  g_free(preload);
  return env;
}

static bool check_status(const Run *run, int status) {
  if (run->error) {
    char *text = snippet(run->err, strlen(run->err));
    char *msg = g_strdup_printf("spawn error: %s", text);
    fail_msg(__FILE__, __LINE__, msg);
    g_free(msg);
    g_free(text);
    return false;
  }
  if (run->status != status) {
    char *text = snippet(run->err, strlen(run->err));
    char *msg = g_strdup_printf("status %d != %d (%s)", run->status, status, text);
    fail_msg(__FILE__, __LINE__, msg);
    g_free(msg);
    g_free(text);
    return false;
  }
  return true;
}

static Run command_kind(const char *kind, const char *dest, char **payload,
                        int payload_n, bool old, const char *cwd, char **env) {
  char *program = old ? g_strdup(python) : exe_path(kind);
  char *script = old ? legacy_path(kind) : NULL;
  int argc = (old ? 2 : 1) + 1 + payload_n;
  char **argv = g_new0(char *, (guint)argc + 1);
  int at = 0;
  argv[at++] = program;
  if (old)
    argv[at++] = script;
  argv[at++] = (char *)dest;
  for (int i = 0; i < payload_n; i++)
    argv[at++] = payload[i];
  Run run = run_argv(cwd, argv, env, 15000);
  g_free(argv);
  g_free(program);
  g_free(script);
  return run;
}

static void compare_input(const char *input, int status) {
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      return;
    char *native = g_build_filename(root, "native", "out", NULL);
    char **payload = (char *[]){(char *)input};
    Run run = command_kind(kinds[k], native, payload, 1, false, NULL, NULL);
    check_status(&run, status);
    if (legacy) {
      char *old = g_build_filename(root, "legacy", "out", NULL);
      Run other = command_kind(kinds[k], old, payload, 1, true, NULL, NULL);
      check_status(&other, status);
      bool native_exists = exists_path(native);
      CHECK(native_exists == exists_path(old));
      if (native_exists) {
        size_t an = 0, bn = 0;
        char *a = read_file(native, &an);
        char *b = read_file(old, &bn);
        CHECK(a && b && an == bn && !memcmp(a, b, an));
        free(a);
        free(b);
      }
      g_free(old);
      run_free(&other);
    }
    run_free(&run);
    g_free(native);
    rm_tree(root);
    g_free(root);
  }
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

static char *bracket_payload(int depth, const char *leaf) {
  size_t leaf_len = strlen(leaf);
  size_t n = 6 + (size_t)depth * 2 + leaf_len;
  char *out = malloc(n + 1);
  if (!out)
    return NULL;
  memcpy(out, "{\"x\":", 5);
  size_t at = 5;
  for (int i = 0; i < depth; i++)
    out[at++] = '[';
  memcpy(out + at, leaf, leaf_len);
  at += leaf_len;
  for (int i = 0; i < depth; i++)
    out[at++] = ']';
  out[at++] = '}';
  out[at] = 0;
  return out;
}

static Json *object_get(Json *object, const char *key) {
  if (!object || object->kind != J_OBJECT)
    return NULL;
  size_t n = strlen(key);
  for (guint i = 0; i < object->keys->len; i++) {
    GString *name = object->keys->pdata[i];
    if (name->len == n && !memcmp(name->str, key, n))
      return object->values->pdata[i];
  }
  return NULL;
}

static void test_nested(void) {
  test_name = "both public families accept complete nested objects";
  const char *inputs[] = {
      "{}",
      "{\"enabled\":true,\"nested\":{\"x\":[1,false,null,\"text\"]}}",
      " { \"weather\": {\"city\":\"London\",\"latitude\":51.5074}, \"colors\": [\"#fff\", \"dark\"] } ",
      "{\"quote\":\"\\\"\\\\/\\b\\f\\n\\r\\t\",\"nul\":\"\\u0000\",\"del\":\"\\u007f\"}",
  };
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++)
    compare_input(inputs[i], 0);
}

static void test_duplicates(void) {
  test_name = "duplicate keys keep first insertion order and last value";
  const char *inputs[] = {
      "{\"a\":1,\"b\":2,\"a\":3}",
      "{\"x\":{\"a\":1,\"a\":2},\"a\":[{\"z\":1,\"z\":2}]}",
      "{\"x\\u0000\":1,\"x\":2,\"x\\u0000\":3}",
      "{\"😀\":1,\"\\ud83d\\ude00\":2}",
  };
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++)
    compare_input(inputs[i], 0);
}

static void test_unicode(void) {
  test_name = "Unicode astral characters and lone surrogates";
  const char *inputs[] = {
      "{\"text\":\"π🙂中é\\u001f\"}",
      "{\"x\":\"\\ud800\",\"y\":\"\\udfff\",\"z\":\"\\ud800x\\udc00\"}",
      "{\"x\":\"\\ud800\\udc00\\udbff\\udfff\"}",
      "{\"x\":\"\\ud800\\u0041\\udc00\",\"y\":\"\\uFEFF\"}",
      "{\"x\":\"\xe2\x80\xa8\xe2\x80\xa9\xc2\x85\"}",
  };
  for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++)
    compare_input(inputs[i], 0);
}

static void test_integers(void) {
  test_name = "arbitrary integers and the decimal-digit conversion limit";
  const char *numbers[] = {"-0", "18446744073709551615", "-9223372036854775809"};
  for (size_t i = 0; i < 3; i++) {
    char *payload = g_strdup_printf("{\"x\":%s}", numbers[i]);
    compare_input(payload, 0);
    g_free(payload);
  }
  char *two_hundred = repeat_text("{\"x\":1", "0", 200, "}");
  char *four_three = repeat_text("{\"x\":", "1", 4300, "}");
  char *over = repeat_text("{\"x\":", "1", 4301, "}");
  CHECK(two_hundred && four_three && over);
  if (two_hundred)
    compare_input(two_hundred, 0);
  if (four_three)
    compare_input(four_three, 0);
  if (over)
    compare_input(over, 1);
  free(two_hundred);
  free(four_three);
  free(over);
}

static void test_reals(void) {
  test_name = "real formatting preserves signed zero and exponent thresholds";
  const char *numbers[] = {
      "1.0",
      "-0.0",
      "-0e100",
      "0.1",
      "0.2",
      "1.2345678901234567",
      "1.0000000000000002",
      "1000000000000000.0",
      "10000000000000000.0",
      "1e-4",
      "1e-5",
      "0.00009999999999999999",
      "1e23",
      "1e100",
      "1e-300",
      "2.2250738585072014e-308",
      "5e-324",
      "1.7976931348623157e308",
      "-1e999",
      "1e999",
      "-1e-999",
      "1.234567890123456789e20",
  };
  GString *payload = g_string_new("{\"x\":[");
  for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; i++) {
    if (i)
      g_string_append_c(payload, ',');
    g_string_append(payload, numbers[i]);
  }
  g_string_append(payload, "]}");
  compare_input(payload->str, 0);
  g_string_free(payload, true);
}

static uint32_t lcg(uint32_t *seed) {
  *seed = *seed * 1664525u + 1013904223u;
  return *seed;
}

static void test_double_corpus(void) {
  test_name = "deterministic IEEE double corpus";
  uint32_t seed = 17;
  GString *batch = g_string_new("{\"x\":[");
  int in_batch = 0;
  for (int i = 0; i < 1000; i++) {
    uint32_t lo = lcg(&seed), hi = lcg(&seed);
    uint64_t bits = (uint64_t)lo | ((uint64_t)hi << 32);
    double value;
    memcpy(&value, &bits, sizeof value);
    if (!isfinite(value))
      continue;
    char text[128];
    g_ascii_formatd(text, sizeof text, "%.17g", value);
    if (in_batch)
      g_string_append_c(batch, ',');
    g_string_append(batch, text);
    in_batch++;
    if (in_batch == 40) {
      g_string_append(batch, "]}");
      compare_input(batch->str, 0);
      g_string_truncate(batch, 0);
      g_string_append(batch, "{\"x\":[");
      in_batch = 0;
    }
  }
  if (in_batch) {
    g_string_append(batch, "]}");
    compare_input(batch->str, 0);
  }
  g_string_free(batch, true);
}

static void test_nan(void) {
  test_name = "NaN and infinities remain supported";
  compare_input("{\"x\":[NaN,Infinity,-Infinity,1e999,-1e999]}", 0);
  const char *bad[] = {"nan", "NAN", "Inf", "inf", "infinity", "-NaN",
                       "+Infinity"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char *payload = g_strdup_printf("{\"x\":%s}", bad[i]);
    compare_input(payload, 1);
    g_free(payload);
  }
}

static void test_status_classes(void) {
  test_name = "valid nonobjects status 2 and malformed status 1";
  const char *nonobjects[] = {"null", "true", "false", "0", "1.2", "\"x\"",
                              "[]", "[{}]"};
  for (size_t i = 0; i < sizeof nonobjects / sizeof nonobjects[0]; i++)
    compare_input(nonobjects[i], 2);
  const char *bad[] = {"",
                       " ",
                       "\ufeff{}",
                       "{",
                       "{\"x\":1,}",
                       "{\"x\":01}",
                       "{\"x\":1.}",
                       "{\"x\":.1}",
                       "{\"x\":1e}",
                       "{\"x\":+1}",
                       "{\"x\":\"\\q\"}",
                       "{\"x\":\"line\nbreak\"}",
                       "/*x*/{}",
                       "{} trailing",
                       "{\"x\" 1}",
                       "{false:1}"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    compare_input(bad[i], 1);
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      return;
    char *dest = g_build_filename(root, "missing", "out", NULL);
    char **payload = (char *[]){"bad"};
    Run run = command_kind(kinds[k], dest, payload, 1, false, NULL, NULL);
    check_status(&run, 1);
    CHECK(!exists_path(g_build_filename(root, "missing", NULL)));
    char *missing = g_build_filename(root, "missing", NULL);
    CHECK(!exists_path(missing));
    g_free(missing);
    run_free(&run);
    g_free(dest);
    rm_tree(root);
    g_free(root);
  }
}

static void test_character_bound(void) {
  test_name = "8000-character boundary counts Unicode code points";
  const char *texts[] = {"a", "π", "🙂"};
  for (size_t i = 0; i < 3; i++) {
    char *payload = repeat_text("{\"x\":\"", texts[i], 7992, "\"}");
    CHECK(payload);
    if (!payload)
      return;
    compare_input(payload, 0);
    char *over = g_strdup_printf("%s ", payload);
    compare_input(over, 2);
    g_free(over);
    free(payload);
  }
  const char *kinds[] = {"calendar", "weather"};
  char *too_long = repeat_text("{\"x\":\"", "a", 7993, "\"}");
  CHECK(too_long);
  for (int k = 0; k < 2 && too_long; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      break;
    char *dest = g_build_filename(root, "missing", "out", NULL);
    Run empty = command_kind(kinds[k], dest, NULL, 0, false, NULL, NULL);
    check_status(&empty, 2);
    char *two[] = {"{}", "{}"};
    Run extra = command_kind(kinds[k], dest, two, 2, false, NULL, NULL);
    check_status(&extra, 2);
    char *long_args[] = {too_long};
    Run huge = command_kind(kinds[k], dest, long_args, 1, false, NULL, NULL);
    check_status(&huge, 2);
    char *missing = g_build_filename(root, "missing", NULL);
    CHECK(!exists_path(missing));
    g_free(missing);
    run_free(&empty);
    run_free(&extra);
    run_free(&huge);
    g_free(dest);
    rm_tree(root);
    g_free(root);
  }
  free(too_long);
}

static void test_depth(void) {
  test_name = "serialization depth failures retain created parents";
  const int depths[] = {996, 997, 995, 996, 995, 996, 3900};
  const char *leaves[] = {"0", "0", "[]", "[]", "0.5", "0.5", "0"};
  const int statuses[] = {0, 1, 0, 1, 0, 1, 1};
  const char *kinds[] = {"calendar", "weather"};
  for (int c = 0; c < 7; c++)
    for (int k = 0; k < 2; k++) {
      char *root = make_temp();
      char *payload = bracket_payload(depths[c], leaves[c]);
      CHECK(root && payload);
      if (!root || !payload) {
        free(payload);
        g_free(root);
        return;
      }
      char *dest = g_build_filename(root, "native", "out", NULL);
      char *args[] = {payload};
      Run run = command_kind(kinds[k], dest, args, 1, false, NULL, NULL);
      check_status(&run, statuses[c]);
      char *parent = g_build_filename(root, "native", NULL);
      CHECK(exists_path(parent));
      GPtrArray *names = temps(parent);
      CHECK(names->len == 0);
      g_ptr_array_free(names, true);
      if (legacy) {
        char *old = g_build_filename(root, "legacy", "out", NULL);
        Run other = command_kind(kinds[k], old, args, 1, true, NULL, NULL);
        check_status(&other, statuses[c]);
        char *old_parent = g_build_filename(root, "legacy", NULL);
        CHECK(exists_path(old_parent));
        if (!statuses[c]) {
          size_t an = 0, bn = 0;
          char *a = read_file(dest, &an);
          char *b = read_file(old, &bn);
          CHECK(a && b && an == bn && !memcmp(a, b, an));
          free(a);
          free(b);
        }
        g_free(old_parent);
        g_free(old);
        run_free(&other);
      }
      run_free(&run);
      g_free(parent);
      g_free(dest);
      free(payload);
      rm_tree(root);
      g_free(root);
    }
}

static void test_encoding_preserves(void) {
  test_name = "encoding failure preserves an existing destination";
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    char *payload = bracket_payload(997, "0");
    CHECK(root && payload);
    if (!root || !payload) {
      free(payload);
      g_free(root);
      return;
    }
    char *dest = g_build_filename(root, "out", NULL);
    CHECK(write_bytes(dest, "old", 3));
    struct stat before;
    CHECK(stat(dest, &before) == 0);
    char *args[] = {payload};
    Run run = command_kind(kinds[k], dest, args, 1, false, NULL, NULL);
    check_status(&run, 1);
    size_t n = 0;
    char *data = read_file(dest, &n);
    CHECK(data && n == 3 && !memcmp(data, "old", 3));
    free(data);
    struct stat after;
    CHECK(stat(dest, &after) == 0 && after.st_ino == before.st_ino);
    GPtrArray *names = temps(root);
    CHECK(names->len == 0);
    g_ptr_array_free(names, true);
    run_free(&run);
    g_free(dest);
    free(payload);
    rm_tree(root);
    g_free(root);
  }
}

static mode_t mode_bits(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return st.st_mode & 0777;
}

static void test_directory_modes(void) {
  test_name = "new leaf directories are private and existing modes remain";
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      return;
    char *dest = g_build_filename(root, "one", "two", "settings.json", NULL);
    char *args[] = {"{}"};
    Run run = command_kind(kinds[k], dest, args, 1, false, NULL, NULL);
    check_status(&run, 0);
    char *leaf = g_build_filename(root, "one", "two", NULL);
    CHECK(mode_bits(leaf) == 0700);
    CHECK(mode_bits(dest) == 0600);
    if (legacy) {
      char *old = g_build_filename(root, "old", "one", "two", "settings.json", NULL);
      Run other = command_kind(kinds[k], old, args, 1, true, NULL, NULL);
      check_status(&other, 0);
      char *native_mid = g_build_filename(root, "one", NULL);
      char *old_mid = g_build_filename(root, "old", "one", NULL);
      CHECK(mode_bits(native_mid) == mode_bits(old_mid));
      g_free(native_mid);
      g_free(old_mid);
      g_free(old);
      run_free(&other);
    }
    char *parent = g_build_filename(root, "existing", NULL);
    CHECK(mkdir(parent, 0755) == 0);
    CHECK(chmod(parent, 0755) == 0);
    char *out = g_build_filename(parent, "out", NULL);
    Run kept = command_kind(kinds[k], out, args, 1, false, NULL, NULL);
    check_status(&kept, 0);
    CHECK(mode_bits(parent) == 0755);
    run_free(&run);
    run_free(&kept);
    g_free(out);
    g_free(parent);
    g_free(leaf);
    g_free(dest);
    rm_tree(root);
    g_free(root);
  }
}

static void test_replacement(void) {
  test_name = "replacement changes inode and never edits an old link target";
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      return;
    char *target = g_build_filename(root, "old", NULL);
    char *dest = g_build_filename(root, "out", NULL);
    CHECK(write_bytes(target, "keep", 4));
    CHECK(symlink(target, dest) == 0);
    char *args[] = {"{\"x\":1}"};
    Run run = command_kind(kinds[k], dest, args, 1, false, NULL, NULL);
    check_status(&run, 0);
    struct stat link_st;
    CHECK(lstat(dest, &link_st) == 0 && !S_ISLNK(link_st.st_mode));
    size_t n = 0;
    char *kept = read_file(target, &n);
    CHECK(kept && n == 4 && !memcmp(kept, "keep", 4));
    free(kept);
    char *hard = g_build_filename(root, "hard", NULL);
    CHECK(link(target, hard) == 0);
    char *empty[] = {"{}"};
    Run hard_run = command_kind(kinds[k], hard, empty, 1, false, NULL, NULL);
    check_status(&hard_run, 0);
    kept = read_file(target, &n);
    CHECK(kept && n == 4 && !memcmp(kept, "keep", 4));
    free(kept);
    struct stat before;
    CHECK(stat(dest, &before) == 0);
    char *next[] = {"{\"x\":2}"};
    Run again = command_kind(kinds[k], dest, next, 1, false, NULL, NULL);
    check_status(&again, 0);
    struct stat after;
    CHECK(stat(dest, &after) == 0);
    CHECK(after.st_ino != before.st_ino);
    CHECK((after.st_mode & 0777) == 0600);
    run_free(&run);
    run_free(&hard_run);
    run_free(&again);
    g_free(hard);
    g_free(dest);
    g_free(target);
    rm_tree(root);
    g_free(root);
  }
}

static void test_lexical_paths(void) {
  test_name = "parent links and lexical dot paths";
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      return;
    char *actual = g_build_filename(root, "actual", NULL);
    char *link_path = g_build_filename(root, "link", NULL);
    CHECK(mkdir(actual, 0700) == 0);
    CHECK(symlink(actual, link_path) == 0);
    char *via = g_build_filename(root, "link", "out", NULL);
    char *args[] = {"{}"};
    Run run = command_kind(kinds[k], via, args, 1, false, NULL, NULL);
    check_status(&run, 0);
    char *actual_out = g_build_filename(actual, "out", NULL);
    size_t n = 0;
    char *data = read_file(actual_out, &n);
    CHECK(data && n == 3 && !memcmp(data, "{}\n", 3));
    free(data);
    char *dotdot = g_strdup_printf("%s/missing/../out", root);
    Run created = command_kind(kinds[k], dotdot, args, 1, false, NULL, NULL);
    check_status(&created, 0);
    char *missing = g_build_filename(root, "missing", NULL);
    CHECK(exists_path(missing));
    char *root_out = g_build_filename(root, "out", NULL);
    data = read_file(root_out, &n);
    CHECK(data && n == 3 && !memcmp(data, "{}\n", 3));
    free(data);
    char *dots = g_strdup_printf("%s/./actual//other", root);
    Run other = command_kind(kinds[k], dots, args, 1, false, NULL, NULL);
    check_status(&other, 0);
    run_free(&run);
    run_free(&created);
    run_free(&other);
    g_free(dots);
    g_free(root_out);
    g_free(missing);
    g_free(dotdot);
    g_free(actual_out);
    g_free(via);
    g_free(link_path);
    g_free(actual);
    rm_tree(root);
    g_free(root);
  }
}

static void test_destination_errors(void) {
  test_name = "basename-only destination and non-directory parents";
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      return;
    char *args[] = {"{}"};
    Run bare = command_kind(kinds[k], "settings", args, 1, false, root, NULL);
    check_status(&bare, 1);
    char *settings = g_build_filename(root, "settings", NULL);
    CHECK(!exists_path(settings));
    char *dir = g_build_filename(root, "dir", NULL);
    CHECK(mkdir(dir, 0700) == 0);
    Run directory = command_kind(kinds[k], dir, args, 1, false, NULL, NULL);
    check_status(&directory, 1);
    GPtrArray *names = temps(root);
    CHECK(names->len == 0);
    g_ptr_array_free(names, true);
    char *file = g_build_filename(root, "file", NULL);
    CHECK(write_bytes(file, "keep", 4));
    char *nested = g_build_filename(root, "file", "out", NULL);
    Run blocked = command_kind(kinds[k], nested, args, 1, false, NULL, NULL);
    check_status(&blocked, 1);
    size_t n = 0;
    char *data = read_file(file, &n);
    CHECK(data && n == 4 && !memcmp(data, "keep", 4));
    free(data);
    run_free(&bare);
    run_free(&directory);
    run_free(&blocked);
    g_free(nested);
    g_free(file);
    g_free(dir);
    g_free(settings);
    rm_tree(root);
    g_free(root);
  }
}

static void test_failures(void) {
  test_name = "failed write chmod close and rename preserve the destination";
  const char *kinds[] = {"calendar", "weather"};
  const char *modes[] = {"write", "chmod", "close", "rename"};
  for (int k = 0; k < 2; k++)
    for (int m = 0; m < 4; m++) {
      char *root = make_temp();
      CHECK(root);
      if (!root)
        return;
      char *dest = g_build_filename(root, "out", NULL);
      CHECK(write_bytes(dest, "old", 3));
      struct stat before;
      CHECK(stat(dest, &before) == 0);
      char **env = fault_env(modes[m], NULL);
      char *args[] = {"{\"x\":1}"};
      Run run = command_kind(kinds[k], dest, args, 1, false, NULL, env);
      check_status(&run, 1);
      size_t n = 0;
      char *data = read_file(dest, &n);
      CHECK(data && n == 3 && !memcmp(data, "old", 3));
      free(data);
      struct stat after;
      CHECK(stat(dest, &after) == 0 && after.st_ino == before.st_ino);
      GPtrArray *names = temps(root);
      CHECK(names->len == 0);
      g_ptr_array_free(names, true);
      env_free(env);
      run_free(&run);
      g_free(dest);
      rm_tree(root);
      g_free(root);
    }
}

static void test_cleanup(void) {
  test_name = "cleanup touches only a successfully created temporary";
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      return;
    char *existing =
        g_strdup_printf("%s/.%s-settings-ownedx", root, kinds[k]);
    CHECK(write_bytes(existing, "unowned", 7));
    char **env = fault_env("create", NULL);
    char *dest = g_build_filename(root, "out", NULL);
    char *args[] = {"{}"};
    Run failed = command_kind(kinds[k], dest, args, 1, false, NULL, env);
    check_status(&failed, 1);
    size_t n = 0;
    char *data = read_file(existing, &n);
    CHECK(data && n == 7 && !memcmp(data, "unowned", 7));
    free(data);
    CHECK(unlink(existing) == 0);
    env_free(env);
    env = fault_env("post-rename", NULL);
    Run published = command_kind(kinds[k], dest, args, 1, false, NULL, env);
    check_status(&published, 0);
    GPtrArray *names = temps(root);
    CHECK(names->len == 1);
    if (names->len == 1) {
      char *temp = g_build_filename(root, names->pdata[0], NULL);
      data = read_file(temp, &n);
      CHECK(data && n == 5 && !memcmp(data, "other", 5));
      free(data);
      g_free(temp);
    }
    data = read_file(dest, &n);
    CHECK(data && n == 3 && !memcmp(data, "{}\n", 3));
    free(data);
    g_ptr_array_free(names, true);
    env_free(env);
    run_free(&failed);
    run_free(&published);
    g_free(dest);
    g_free(existing);
    rm_tree(root);
    g_free(root);
  }
}

static void test_short_write(void) {
  test_name = "short writes are completed";
  const char *kinds[] = {"calendar", "weather"};
  char *payload = repeat_text("{\"x\":\"", "text", 100, "\"}");
  char *expected = repeat_text("", "text", 100, "");
  CHECK(payload && expected);
  if (!payload || !expected) {
    free(payload);
    free(expected);
    return;
  }
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      break;
    char *dest = g_build_filename(root, "out", NULL);
    char **env = fault_env("short-write", NULL);
    char *args[] = {payload};
    Run run = command_kind(kinds[k], dest, args, 1, false, NULL, env);
    check_status(&run, 0);
    size_t n = 0;
    char *data = read_file(dest, &n);
    CHECK(data);
    if (data) {
      Json *parsed = settings_parse(data);
      Json *value = object_get(parsed, "x");
      CHECK(value && value->kind == J_STRING &&
            value->string->len == strlen(expected) &&
            !memcmp(value->string->str, expected, value->string->len));
      settings_free(parsed);
      free(data);
    }
    env_free(env);
    run_free(&run);
    g_free(dest);
    rm_tree(root);
    g_free(root);
  }
  free(payload);
  free(expected);
}

static void test_publication(void) {
  test_name = "temporary stays private until atomic publication";
  const char *kinds[] = {"calendar", "weather"};
  for (int k = 0; k < 2; k++) {
    char *root = make_temp();
    CHECK(root);
    if (!root)
      return;
    char *dest = g_build_filename(root, "out", NULL);
    char *release = g_build_filename(root, "release", NULL);
    CHECK(write_bytes(dest, "old", 3));
    char **env = fault_env("", release);
    char *program = exe_path(kinds[k]);
    char *argv[] = {program, dest, "{\"x\":1}", NULL};
    Child child;
    CHECK(spawn_child(&child, NULL, argv, env));
    if (!child.pid) {
      env_free(env);
      g_free(program);
      g_free(release);
      g_free(dest);
      rm_tree(root);
      g_free(root);
      return;
    }
    char *temporary = NULL;
    for (int i = 0; i < 400 && !temporary; i++) {
      child_pump(&child);
      GPtrArray *names = temps(root);
      if (names->len)
        temporary = g_strdup(names->pdata[0]);
      g_ptr_array_free(names, true);
      if (!temporary)
        usleep(5000);
    }
    CHECK(temporary);
    if (temporary) {
      char *temp_path = g_build_filename(root, temporary, NULL);
      CHECK(mode_bits(temp_path) == 0600);
      g_free(temp_path);
    }
    size_t n = 0;
    char *data = read_file(dest, &n);
    CHECK(data && n == 3 && !memcmp(data, "old", 3));
    free(data);
    CHECK(write_bytes(release, "go", 2));
    bool error = false;
    int status = wait_child(&child, 15000, &error);
    CHECK(!error && status == 0);
    data = read_file(dest, &n);
    CHECK(data && n == 9 && !memcmp(data, "{\"x\": 1}\n", 9));
    free(data);
    GPtrArray *names = temps(root);
    CHECK(names->len == 0);
    g_ptr_array_free(names, true);
    g_free(temporary);
    env_free(env);
    g_free(program);
    g_free(release);
    g_free(dest);
    rm_tree(root);
    g_free(root);
  }
}

static void test_raw_argv(void) {
  test_name = "raw invalid UTF-8 argv uses surrogateescape";
  const char *kinds[] = {"calendar", "weather"};
  const char *modes[] = {"bad-byte", "surrogate", "bad-destination"};
  for (int k = 0; k < 2; k++)
    for (int m = 0; m < 3; m++) {
      char *root = make_temp();
      CHECK(root);
      if (!root)
        return;
      char *native = g_build_filename(root, "native", NULL);
      char *program = exe_path(kinds[k]);
      char *argv[] = {(char *)raw_argv, (char *)modes[m], native, program, NULL};
      Run run = run_argv(NULL, argv, NULL, 15000);
      check_status(&run, 0);
      if (legacy) {
        char *old = g_build_filename(root, "legacy", NULL);
        char *script = legacy_path(kinds[k]);
        char *old_argv[] = {(char *)raw_argv, (char *)modes[m], old,
                            (char *)python, script, NULL};
        Run other = run_argv(NULL, old_argv, NULL, 15000);
        check_status(&other, 0);
        size_t an = 0, bn = 0;
        char *native_path = native;
        char *old_path = old;
        char *native_owned = NULL, *old_owned = NULL;
        if (!strcmp(modes[m], "bad-destination")) {
          size_t nn = strlen(native), on = strlen(old);
          native_owned = malloc(nn + 16);
          old_owned = malloc(on + 16);
          CHECK(native_owned && old_owned);
          if (native_owned && old_owned) {
            memcpy(native_owned, native, nn);
            native_owned[nn] = '-';
            native_owned[nn + 1] = (char)255;
            memcpy(native_owned + nn + 2, "/settings.json", 15);
            memcpy(old_owned, old, on);
            old_owned[on] = '-';
            old_owned[on + 1] = (char)255;
            memcpy(old_owned + on + 2, "/settings.json", 15);
            native_path = native_owned;
            old_path = old_owned;
          }
        }
        char *a = read_file(native_path, &an);
        char *b = read_file(old_path, &bn);
        CHECK(a && b && an == bn && !memcmp(a, b, an));
        free(a);
        free(b);
        free(native_owned);
        free(old_owned);
        g_free(script);
        g_free(old);
        run_free(&other);
      }
      run_free(&run);
      g_free(program);
      g_free(native);
      rm_tree(root);
      g_free(root);
    }
}

int main(void) {
  bins = getenv("PANEL_SETTINGS_BIN_DIR");
  failure_lib = getenv("PANEL_SETTINGS_FAILURES");
  raw_argv = getenv("PANEL_SETTINGS_ARGV");
  legacy = getenv("PANEL_SETTINGS_LEGACY");
  python = getenv("PANEL_SETTINGS_PYTHON");
  if (!python || !*python)
    python = "python3";
  if (!bins || !*bins || !failure_lib || !*failure_lib || !raw_argv ||
      !*raw_argv) {
    fprintf(stderr,
            "Set panel binary, failure library and argv fixture paths\n");
    return 1;
  }
  test_nested();
  test_duplicates();
  test_unicode();
  test_integers();
  test_reals();
  test_double_corpus();
  test_nan();
  test_status_classes();
  test_character_bound();
  test_depth();
  test_encoding_preserves();
  test_directory_modes();
  test_replacement();
  test_lexical_paths();
  test_destination_errors();
  test_failures();
  test_cleanup();
  test_short_write();
  test_publication();
  test_raw_argv();
  if (failures) {
    fprintf(stderr, "%d panel-settings checks failed\n", failures);
    return 1;
  }
  printf("panel-settings checks passed\n");
  return 0;
}
