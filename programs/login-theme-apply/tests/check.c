#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <regex.h>
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
#include <glib.h>
#include <yyjson.h>

static int failures;
static const char *store = "/nix/store/disposable-greeter-";
static char *fallback_asset;

static void fail_msg(const char *file, int line, const char *fmt, ...) {
  fprintf(stderr, "FAIL %s:%d: ", file, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  failures++;
}
#define FAIL(...) fail_msg(__FILE__, __LINE__, __VA_ARGS__)
#define EXPECT(cond, ...)                                                      \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(__VA_ARGS__);                                                       \
  } while (0)

static char *join_path(const char *a, const char *b) {
  return g_build_filename(a, b, NULL);
}

static void write_file(const char *path, const void *data, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror(path);
    exit(1);
  }
  size_t off = 0;
  const char *p = data;
  while (off < n) {
    ssize_t wrote = write(fd, p + off, n - off);
    if (wrote < 0) {
      if (errno == EINTR)
        continue;
      perror("write");
      exit(1);
    }
    off += (size_t)wrote;
  }
  close(fd);
}

static void write_str(const char *path, const char *text) {
  write_file(path, text, strlen(text));
}

static bool exists_path(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static bool contains_mem(const char *hay, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (!m || n < m)
    return false;
  for (size_t i = 0; i + m <= n; i++)
    if (!memcmp(hay + i, needle, m))
      return true;
  return false;
}

static void rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st) < 0)
    return;
  if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
    DIR *dir = opendir(path);
    if (dir) {
      struct dirent *ent;
      while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
          continue;
        char *child = join_path(path, ent->d_name);
        rm_rf(child);
        g_free(child);
      }
      closedir(dir);
    }
    rmdir(path);
  } else
    unlink(path);
}

static char *temp_dir(const char *prefix) {
  char *tmpl = g_strdup_printf("%s/greeter-select-XXXXXX", prefix);
  if (!mkdtemp(tmpl)) {
    perror("mkdtemp");
    g_free(tmpl);
    return NULL;
  }
  return tmpl;
}

typedef struct {
  char *selection_dir, *selection_name, *runtime_dir, *fallback;
} Manifest;

static Manifest setup_manifest(const char *root) {
  Manifest m = {0};
  m.selection_dir = join_path(root, "selection");
  m.runtime_dir = join_path(root, "runtime");
  m.selection_name = g_strdup("william");
  m.fallback = g_strdup("dark");
  mkdir(m.selection_dir, 0755);
  mkdir(m.runtime_dir, 0755);
  chmod(m.runtime_dir, 0755);
  return m;
}

static void manifest_free(Manifest *m) {
  g_free(m->selection_dir);
  g_free(m->selection_name);
  g_free(m->runtime_dir);
  g_free(m->fallback);
}

static char *quote_len(const char *s, size_t n) {
  GString *out = g_string_new("\"");
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      g_string_append_c(out, '\\');
      g_string_append_c(out, (char)c);
    } else if (c < 0x20)
      g_string_append_printf(out, "\\u%04x", c);
    else
      g_string_append_c(out, (char)c);
  }
  g_string_append_c(out, '"');
  return g_string_free(out, FALSE);
}

static char *quote(const char *s) { return quote_len(s, strlen(s)); }

static char *manifest_text(const Manifest *m, const char *dark_json,
                           const char *light_json) {
  char *fb = quote(m->fallback);
  char *dir = quote(m->selection_dir);
  char *name = quote(m->selection_name);
  char *runtime = quote(m->runtime_dir);
  char *text = g_strdup_printf(
      "{\"fallback\":%s,\"selectionDir\":%s,\"selectionName\":%s,"
      "\"runtimeDir\":%s,\"themes\":{\"dark\":%s,\"light\":%s}}",
      fb, dir, name, runtime, dark_json, light_json);
  g_free(fb);
  g_free(dir);
  g_free(name);
  g_free(runtime);
  return text;
}

static char *object_path(const char *path) {
  char *q = quote(path);
  char *text = g_strdup_printf("{\"path\":%s}", q);
  g_free(q);
  return text;
}

static char *save_manifest(const char *root, const char *text) {
  char *file = join_path(root, "manifest.json");
  write_str(file, text);
  return file;
}

typedef struct {
  int status;
  char *out, *err;
  size_t out_len, err_len;
  bool error, timed_out, running;
  pid_t pid;
  int out_fd, err_fd;
} Result;

static void result_free(Result *r) {
  g_free(r->out);
  g_free(r->err);
  r->out = r->err = NULL;
}

static void append_buf(char **buf, size_t *len, size_t *cap, const char *data,
                       size_t n) {
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (*len + n + 1 > next)
      next *= 2;
    *buf = g_realloc(*buf, next);
    *cap = next;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
  (*buf)[*len] = 0;
}

static void read_ready(int fd, char **buf, size_t *len, size_t *cap) {
  char tmp[4096];
  for (;;) {
    ssize_t got = read(fd, tmp, sizeof tmp);
    if (got < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN)
        return;
      return;
    }
    if (got == 0)
      return;
    append_buf(buf, len, cap, tmp, (size_t)got);
  }
}

static char **copy_env(void) {
  size_t n = 0;
  while (environ[n])
    n++;
  char **env = g_new(char *, n + 1);
  for (size_t i = 0; i < n; i++)
    env[i] = g_strdup(environ[i]);
  env[n] = NULL;
  return env;
}

static void env_set(char ***env, const char *key, const char *value) {
  size_t klen = strlen(key), count = 0;
  while ((*env)[count])
    count++;
  for (size_t i = 0; i < count; i++) {
    if (!strncmp((*env)[i], key, klen) && (*env)[i][klen] == '=') {
      g_free((*env)[i]);
      if (!value) {
        memmove(*env + i, *env + i + 1, (count - i) * sizeof **env);
        return;
      }
      (*env)[i] = g_strdup_printf("%s=%s", key, value);
      return;
    }
  }
  if (!value)
    return;
  *env = g_renew(char *, *env, count + 2);
  (*env)[count] = g_strdup_printf("%s=%s", key, value);
  (*env)[count + 1] = NULL;
}

static void free_env(char **env) {
  for (size_t i = 0; env[i]; i++)
    g_free(env[i]);
  g_free(env);
}

static void free_argv(char **argv) {
  for (int i = 0; argv[i]; i++)
    g_free(argv[i]);
  g_free(argv);
}

static Result start_proc(char **argv, char **env, const char *cwd) {
  Result r = {.status = -1, .out_fd = -1, .err_fd = -1};
  int out[2], err[2];
  if (pipe(out) < 0 || pipe(err) < 0) {
    r.error = true;
    return r;
  }
  pid_t pid = fork();
  if (pid < 0) {
    r.error = true;
    return r;
  }
  if (pid == 0) {
    dup2(out[1], STDOUT_FILENO);
    dup2(err[1], STDERR_FILENO);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
    close(out[0]);
    close(out[1]);
    close(err[0]);
    close(err[1]);
    if (cwd && chdir(cwd) != 0)
      _exit(127);
    execvpe(argv[0], argv, env);
    _exit(127);
  }
  close(out[1]);
  close(err[1]);
  fcntl(out[0], F_SETFL, O_NONBLOCK);
  fcntl(err[0], F_SETFL, O_NONBLOCK);
  r.pid = pid;
  r.out_fd = out[0];
  r.err_fd = err[0];
  r.running = true;
  r.out = g_strdup("");
  r.err = g_strdup("");
  return r;
}

static void finish_proc(Result *r, int timeout_ms) {
  if (!r->running)
    return;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  size_t out_cap = 1, err_cap = 1;
  for (;;) {
    struct pollfd pf[2] = {{r->out_fd, POLLIN, 0}, {r->err_fd, POLLIN, 0}};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                        (now.tv_nsec - start.tv_nsec) / 1000000);
    int remain = timeout_ms - elapsed;
    if (remain < 0)
      remain = 0;
    int ready = poll(pf, 2, remain);
    if (ready > 0) {
      if (pf[0].revents)
        read_ready(r->out_fd, &r->out, &r->out_len, &out_cap);
      if (pf[1].revents)
        read_ready(r->err_fd, &r->err, &r->err_len, &err_cap);
    }
    int status = 0;
    pid_t got = waitpid(r->pid, &status, WNOHANG);
    if (got == r->pid) {
      read_ready(r->out_fd, &r->out, &r->out_len, &out_cap);
      read_ready(r->err_fd, &r->err, &r->err_len, &err_cap);
      r->status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      r->running = false;
      break;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                    (now.tv_nsec - start.tv_nsec) / 1000000);
    if (elapsed >= timeout_ms) {
      kill(r->pid, SIGKILL);
      waitpid(r->pid, &status, 0);
      r->timed_out = true;
      r->error = true;
      r->running = false;
      break;
    }
  }
  close(r->out_fd);
  close(r->err_fd);
  r->out_fd = r->err_fd = -1;
}

static bool legacy_enabled(void) { return getenv("GREETER_SELECT_LEGACY") != NULL; }

static char **program_argv(bool old, const char **args, int nargs) {
  char **argv = g_new0(char *, (size_t)nargs + 4);
  int n = 0;
  if (old) {
    const char *python = getenv("GREETER_SELECT_PYTHON");
    argv[n++] = g_strdup(python && *python ? python : "python3");
    argv[n++] = g_strdup(getenv("GREETER_SELECT_LEGACY"));
  } else
    argv[n++] = g_strdup(getenv("GREETER_SELECT_BIN"));
  for (int i = 0; i < nargs; i++)
    argv[n++] = g_strdup(args[i]);
  argv[n] = NULL;
  return argv;
}

static char *preload_path(void) {
  const char *asan = getenv("GREETER_SELECT_ASAN_RT");
  const char *lib = getenv("GREETER_SELECT_FAILURES");
  if (asan && *asan)
    return g_strdup_printf("%s:%s", asan, lib);
  return g_strdup(lib);
}

static bool sanitizer_clean(const Result *r) {
  return !contains_mem(r->err, r->err_len, "ERROR: AddressSanitizer") &&
         !contains_mem(r->err, r->err_len, "ERROR: LeakSanitizer") &&
         !contains_mem(r->err, r->err_len, "runtime error:") &&
         !contains_mem(r->err, r->err_len, "DEADLYSIGNAL");
}

static Result launch(const char *file, bool old, char **env, const char *cwd) {
  const char *args[] = {file};
  char **argv = program_argv(old, args, 1);
  char **owned = env ? env : copy_env();
  Result r = start_proc(argv, owned, cwd);
  finish_proc(&r, 10000);
  if (r.error)
    FAIL("spawn error");
  EXPECT(sanitizer_clean(&r), "sanitizer output: %s", r.err);
  free_argv(argv);
  if (!env)
    free_env(owned);
  return r;
}

static char **fault_env(const char *mode, const char **keys, const char **vals,
                        int n) {
  char **env = copy_env();
  char *preload = preload_path();
  env_set(&env, "LD_PRELOAD", preload);
  g_free(preload);
  env_set(&env, "GREETER_FIXTURE_FAIL", mode ? mode : "");
  for (int i = 0; i < n; i++)
    env_set(&env, keys[i], vals[i]);
  return env;
}

static int temp_count(const char *runtime) {
  DIR *dir = opendir(runtime);
  if (!dir)
    return -1;
  int n = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)))
    if (!strncmp(ent->d_name, ".theme-", 7))
      n++;
  closedir(dir);
  return n;
}

static bool link_eq(const char *path, const void *data, size_t n) {
  char buf[8192];
  ssize_t got = readlink(path, buf, sizeof buf);
  return got == (ssize_t)n && !memcmp(buf, data, n);
}

static bool link_str(const char *path, const char *text) {
  return link_eq(path, text, strlen(text));
}

static char *standard_manifest(const Manifest *m) {
  char *dark = object_path(fallback_asset);
  char *light_path = g_strconcat(store, "light", NULL);
  char *light = object_path(light_path);
  char *text = manifest_text(m, dark, light);
  g_free(dark);
  g_free(light);
  g_free(light_path);
  return text;
}

static void check_selection(const void *data, size_t len, bool absent,
                            const char *expected) {
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_dir("/tmp");
    if (!root)
      return;
    Manifest m = setup_manifest(root);
    if (!absent) {
      char *leaf = join_path(m.selection_dir, "william");
      write_file(leaf, data, len);
      g_free(leaf);
    }
    char *text = standard_manifest(&m);
    char *file = save_manifest(root, text);
    Result r = launch(file, old, NULL, NULL);
    EXPECT(r.status == 0, "selection status %d %s", r.status, r.err);
    if (!r.status) {
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(link_str(out, expected), "link %s", expected);
      EXPECT(temp_count(m.runtime_dir) == 0, "temps");
      g_free(out);
    }
    result_free(&r);
    g_free(text);
    g_free(file);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
}

static void test_allowlist(void) {
  char *light = g_strconcat(store, "light", NULL);
  check_selection("light\n", 6, false, light);
  check_selection("light", 5, false, light);
  check_selection(NULL, 0, true, fallback_asset);
  check_selection("unknown\n", 8, false, fallback_asset);
  g_free(light);
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_dir("/tmp");
    Manifest m = setup_manifest(root);
    char *dark = object_path(fallback_asset);
    char *text = manifest_text(&m, dark, "null");
    char *leaf = join_path(m.selection_dir, "william");
    write_str(leaf, "light");
    char *file = save_manifest(root, text);
    Result r = launch(file, old, NULL, NULL);
    EXPECT(r.status == 0, "null entry");
    char *out = join_path(m.runtime_dir, "omarchy");
    EXPECT(link_str(out, fallback_asset), "null falls back");
    result_free(&r);
    g_free(out);
    g_free(leaf);
    g_free(file);
    g_free(text);
    g_free(dark);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
}

static void test_id_shape(void) {
  const char *bad[] = {"",      "\n",         "LIGHT",     " light",
                       "light ", "light\r\n",  "light\n\n", "light\nother",
                       "-light", "light-",     "light--name", "light_name",
                       "light.name", "lïght", "١",        "ｌｉｇｈｔ"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    check_selection(bad[i], strlen(bad[i]), false, fallback_asset);
  char nul[] = {'l', 'i', 'g', 'h', 't', 0};
  check_selection(nul, sizeof nul, false, fallback_asset);
  unsigned char raw[] = {255, 10};
  check_selection(raw, sizeof raw, false, fallback_asset);
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_dir("/tmp");
    Manifest m = setup_manifest(root);
    char *valid = g_strconcat(store, "valid", NULL);
    char *dark = object_path(fallback_asset);
    char *extra = object_path(valid);
    char *light = object_path(g_strconcat(store, "light", NULL));
    char *fb = quote(m.fallback);
    char *dir = quote(m.selection_dir);
    char *name = quote(m.selection_name);
    char *runtime = quote(m.runtime_dir);
    char *text = g_strdup_printf(
        "{\"fallback\":%s,\"selectionDir\":%s,\"selectionName\":%s,"
        "\"runtimeDir\":%s,\"themes\":{\"dark\":%s,\"light\":%s,"
        "\"a-0-xyz9\":%s}}",
        fb, dir, name, runtime, dark, light, extra);
    char *leaf = join_path(m.selection_dir, "william");
    write_str(leaf, "a-0-xyz9\n");
    char *file = save_manifest(root, text);
    Result r = launch(file, old, NULL, NULL);
    EXPECT(r.status == 0, "valid id %s", r.err);
    char *out = join_path(m.runtime_dir, "omarchy");
    EXPECT(link_str(out, valid), "valid asset");
    result_free(&r);
    g_free(out);
    g_free(leaf);
    g_free(file);
    g_free(text);
    g_free(runtime);
    g_free(name);
    g_free(dir);
    g_free(fb);
    g_free(light);
    g_free(extra);
    g_free(dark);
    g_free(valid);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
}

static void test_bound(void) {
  const int rows[][3] = {{64, 0, 1}, {63, 1, 1}, {64, 1, 0}, {65, 0, 0},
                         {10000, 0, 0}};
  int passes = legacy_enabled() ? 2 : 1;
  for (size_t row = 0; row < sizeof rows / sizeof rows[0]; row++) {
    for (int old = 0; old < passes; old++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      int length = rows[row][0], newline = rows[row][1], selected = rows[row][2];
      char *id = g_malloc((size_t)length + 1);
      memset(id, 'a', (size_t)length);
      id[length] = 0;
      char *long_path = g_strconcat(store, "long", NULL);
      char *dark = object_path(fallback_asset);
      char *light = object_path(g_strconcat(store, "light", NULL));
      char *extra = object_path(long_path);
      char *quoted_id = quote(id);
      char *fb = quote(m.fallback);
      char *dir = quote(m.selection_dir);
      char *name = quote(m.selection_name);
      char *runtime = quote(m.runtime_dir);
      char *text = g_strdup_printf(
          "{\"fallback\":%s,\"selectionDir\":%s,\"selectionName\":%s,"
          "\"runtimeDir\":%s,\"themes\":{\"dark\":%s,\"light\":%s,%s:%s}}",
          fb, dir, name, runtime, dark, light, quoted_id, extra);
      char *leaf = join_path(m.selection_dir, "william");
      GString *body = g_string_new(id);
      if (newline)
        g_string_append_c(body, '\n');
      write_file(leaf, body->str, body->len);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 0, "bound %d", length);
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(link_str(out, selected ? long_path : fallback_asset),
             "bound selected %d", length);
      result_free(&r);
      g_string_free(body, TRUE);
      g_free(out);
      g_free(leaf);
      g_free(file);
      g_free(text);
      g_free(runtime);
      g_free(name);
      g_free(dir);
      g_free(fb);
      g_free(quoted_id);
      g_free(extra);
      g_free(light);
      g_free(dark);
      g_free(long_path);
      g_free(id);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_symlink_selection(void) {
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (int parent = 0; parent < 2; parent++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *other = join_path(m.selection_dir, "other");
      write_str(other, "light");
      if (parent) {
        char *leaf = join_path(m.selection_dir, "william");
        write_str(leaf, "light");
        g_free(leaf);
        g_free(m.selection_dir);
        m.selection_dir = join_path(root, "alias");
        char *selection = join_path(root, "selection");
        EXPECT(symlink(selection, m.selection_dir) == 0, "create selection alias");
        g_free(selection);
      } else {
        char *leaf = join_path(m.selection_dir, "william");
        EXPECT(symlink("other", leaf) == 0, "create selection symlink");
        g_free(leaf);
      }
      char *text = standard_manifest(&m);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 0, "symlink selection");
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(link_str(out, fallback_asset), "symlink fallback");
      result_free(&r);
      g_free(out);
      g_free(file);
      g_free(text);
      g_free(other);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_nonregular(void) {
  const char *modes[] = {"directory", "fifo", "missing-parent", "file-parent"};
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (size_t i = 0; i < 4; i++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *leaf = join_path(m.selection_dir, "william");
      if (!strcmp(modes[i], "directory"))
        mkdir(leaf, 0755);
      if (!strcmp(modes[i], "fifo"))
        EXPECT(mkfifo(leaf, 0644) == 0, "mkfifo");
      if (!strcmp(modes[i], "missing-parent")) {
        g_free(m.selection_dir);
        m.selection_dir = join_path(root, "missing");
      }
      if (!strcmp(modes[i], "file-parent")) {
        char *file = join_path(root, "file");
        write_str(file, "light");
        g_free(m.selection_dir);
        m.selection_dir = file;
      }
      char *text = standard_manifest(&m);
      char *saved = save_manifest(root, text);
      Result r = launch(saved, old, NULL, NULL);
      EXPECT(r.status == 0, "nonregular %s", modes[i]);
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(link_str(out, fallback_asset), "nonregular fallback %s", modes[i]);
      result_free(&r);
      g_free(out);
      g_free(saved);
      g_free(text);
      g_free(leaf);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_relative_names(void) {
  const char *names[] = {"nested/theme", "../external", "absolute"};
  int passes = legacy_enabled() ? 2 : 1;
  char *light = g_strconcat(store, "light", NULL);
  for (int old = 0; old < passes; old++) {
    for (int i = 0; i < 3; i++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *nested = join_path(m.selection_dir, "nested");
      mkdir(nested, 0755);
      char *actual = !strcmp(names[i], "absolute")
                         ? join_path(root, "absolute")
                         : join_path(m.selection_dir, names[i]);
      write_str(actual, "light");
      g_free(m.selection_name);
      m.selection_name =
          !strcmp(names[i], "absolute") ? g_strdup(actual) : g_strdup(names[i]);
      char *text = standard_manifest(&m);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 0, "relative %s %s", names[i], r.err);
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(link_str(out, light), "relative link %s", names[i]);
      result_free(&r);
      g_free(out);
      g_free(file);
      g_free(text);
      g_free(actual);
      g_free(nested);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
  g_free(light);
}

static void test_absent_fallback(void) {
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (int selected = 1; selected >= 0; selected--) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      g_free(m.fallback);
      m.fallback = g_strdup("missing");
      char *leaf = join_path(m.selection_dir, "william");
      write_str(leaf, selected ? "light" : "unknown");
      char *text = standard_manifest(&m);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == (selected ? 0 : 1), "fallback needed %d", selected);
      if (!selected) {
        EXPECT(contains_mem(r.err, r.err_len,
                            "greeter fallback is not in the allowlist"),
               "fallback message");
        char *out = join_path(m.runtime_dir, "omarchy");
        EXPECT(!exists_path(out), "no publication");
        g_free(out);
      }
      result_free(&r);
      g_free(file);
      g_free(text);
      g_free(leaf);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_store_reject(void) {
  const char *targets[] = {"null", "false", "17", "{}", "\"/tmp/theme\"",
                           "\"/nix/store\"", "\"/nix/storehouse/theme\"",
                           "\"/nix/store/../evil\"", "\"/nix/store/a/../b\"",
                           "\"/nix/store/a/..\"", "\"/nix/store/a\\n\"",
                           "\"/nix/store/a\\u0000b\""};
  int passes = legacy_enabled() ? 2 : 1;
  for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
    for (int old = 0; old < passes; old++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *dark = g_strdup_printf("{\"path\":%s}", targets[i]);
      char *light = object_path(g_strconcat(store, "light", NULL));
      char *text = manifest_text(&m, dark, light);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 1, "reject %s", targets[i]);
      EXPECT(contains_mem(r.err, r.err_len, "refusing non-store theme asset"),
             "reject message %s", targets[i]);
      EXPECT(temp_count(m.runtime_dir) == 0, "reject empty");
      result_free(&r);
      g_free(file);
      g_free(text);
      g_free(light);
      g_free(dark);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_store_spellings(void) {
  const char *targets[] = {"/nix/store/",     "/nix/store/a/./b",
                           "/nix/store/a//b", "/nix/store/a/..b",
                           "/nix/store/a\rb", "/nix/store/a/尾🙂",
                           "/nix/store/a/"};
  int passes = legacy_enabled() ? 2 : 1;
  for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
    for (int old = 0; old < passes; old++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *dark = object_path(targets[i]);
      char *light = object_path(g_strconcat(store, "light", NULL));
      char *text = manifest_text(&m, dark, light);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 0, "spelling %s %s", targets[i], r.err);
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(link_str(out, targets[i]), "spelling link");
      result_free(&r);
      g_free(out);
      g_free(file);
      g_free(text);
      g_free(light);
      g_free(dark);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_malformed_selected(void) {
  const char *entries[] = {"false", "[]", "17", "{}", "{\"path\":\"/tmp/bad\"}"};
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *dark = object_path(fallback_asset);
      char *text = manifest_text(&m, dark, entries[i]);
      char *file = save_manifest(root, text);
      char *leaf = join_path(m.selection_dir, "william");
      write_str(leaf, "light");
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 1, "malformed %s", entries[i]);
      EXPECT(temp_count(m.runtime_dir) == 0, "malformed empty");
      result_free(&r);
      write_str(leaf, "dark");
      r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 0, "unused malformed %s", entries[i]);
      result_free(&r);
      g_free(leaf);
      g_free(file);
      g_free(text);
      g_free(dark);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_runtime_rejected(void) {
  const char *modes[] = {"symlink", "file", "missing", "group", "world"};
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (size_t i = 0; i < 5; i++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      if (!strcmp(modes[i], "symlink") || !strcmp(modes[i], "file") ||
          !strcmp(modes[i], "missing")) {
        rm_rf(m.runtime_dir);
        if (!strcmp(modes[i], "symlink"))
          EXPECT(symlink(m.selection_dir, m.runtime_dir) == 0, "create runtime symlink");
        if (!strcmp(modes[i], "file"))
          write_str(m.runtime_dir, "keep");
      } else
        chmod(m.runtime_dir, !strcmp(modes[i], "group") ? 0775 : 0757);
      char *text = standard_manifest(&m);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 1, "runtime %s", modes[i]);
      if (strcmp(modes[i], "missing"))
        EXPECT(contains_mem(r.err, r.err_len, "refusing ") &&
                   (contains_mem(r.err, r.err_len, "unowned runtime directory") ||
                    contains_mem(r.err, r.err_len, "writable runtime directory")),
               "runtime message %s", modes[i]);
      if (!strcmp(modes[i], "file")) {
        size_t n = 0;
        char *data = NULL;
        int fd = open(m.runtime_dir, O_RDONLY);
        EXPECT(fd >= 0, "kept file");
        if (fd >= 0) {
          char buf[8];
          n = (size_t)read(fd, buf, sizeof buf);
          data = g_strndup(buf, n);
          close(fd);
        }
        EXPECT(data && !strcmp(data, "keep"), "file retained");
        g_free(data);
      }
      result_free(&r);
      g_free(file);
      g_free(text);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_owner(void) {
  char *root = temp_dir("/tmp");
  Manifest m = setup_manifest(root);
  char *text = standard_manifest(&m);
  char *file = save_manifest(root, text);
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char **env = fault_env("uid", NULL, NULL, 0);
    Result r = launch(file, old, env, NULL);
    EXPECT(r.status == 1, "uid %s", r.err);
    EXPECT(contains_mem(r.err, r.err_len, "refusing unowned runtime directory"),
           "uid message");
    EXPECT(temp_count(m.runtime_dir) == 0, "uid empty");
    result_free(&r);
    free_env(env);
  }
  g_free(file);
  g_free(text);
  manifest_free(&m);
  rm_rf(root);
  g_free(root);
}

static void test_permissions(void) {
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_dir("/tmp");
    Manifest m = setup_manifest(root);
    chmod(m.runtime_dir, 0750);
    char *ancestor = join_path(root, "ancestor");
    EXPECT(symlink(root, ancestor) == 0, "create ancestor symlink");
    g_free(m.runtime_dir);
    m.runtime_dir = join_path(ancestor, "runtime");
    char *text = standard_manifest(&m);
    char *file = save_manifest(root, text);
    Result r = launch(file, old, NULL, NULL);
    EXPECT(r.status == 0, "ancestor %s", r.err);
    struct stat st;
    EXPECT(stat(m.runtime_dir, &st) == 0 && (st.st_mode & 0777) == 0750,
           "mode retained");
    char *out = join_path(m.runtime_dir, "omarchy");
    EXPECT(link_str(out, fallback_asset), "ancestor link");
    result_free(&r);
    g_free(out);
    g_free(file);
    g_free(text);
    g_free(ancestor);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
}

static void test_atomic(void) {
  const char *types[] = {"symlink", "file"};
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (int i = 0; i < 2; i++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *out = join_path(m.runtime_dir, "omarchy");
      char *other = join_path(root, "other");
      write_str(other, "keep");
      if (!strcmp(types[i], "symlink"))
        EXPECT(symlink(other, out) == 0, "create previous selection");
      else
        write_str(out, "old");
      char *text = standard_manifest(&m);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 0, "atomic %s", types[i]);
      struct stat st;
      EXPECT(lstat(out, &st) == 0 && S_ISLNK(st.st_mode), "replaced link");
      EXPECT(link_str(out, fallback_asset), "new target");
      char buf[8];
      int fd = open(other, O_RDONLY);
      ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
      if (fd >= 0)
        close(fd);
      EXPECT(n == 4 && !memcmp(buf, "keep", 4), "target kept");
      EXPECT(temp_count(m.runtime_dir) == 0, "atomic temps");
      result_free(&r);
      g_free(file);
      g_free(text);
      g_free(other);
      g_free(out);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_output_dir(void) {
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_dir("/tmp");
    Manifest m = setup_manifest(root);
    char *out = join_path(m.runtime_dir, "omarchy");
    mkdir(out, 0755);
    char *keep = join_path(out, "keep");
    write_str(keep, "old");
    char *text = standard_manifest(&m);
    char *file = save_manifest(root, text);
    Result r = launch(file, old, NULL, NULL);
    EXPECT(r.status == 1, "dir rename");
    char buf[8];
    int fd = open(keep, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
    if (fd >= 0)
      close(fd);
    EXPECT(n == 3 && !memcmp(buf, "old", 3), "dir kept");
    EXPECT(temp_count(m.runtime_dir) == 0, "dir temps");
    result_free(&r);
    g_free(file);
    g_free(text);
    g_free(keep);
    g_free(out);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
}

static void test_duplicates(void) {
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_dir("/tmp");
    Manifest m = setup_manifest(root);
    char *text = standard_manifest(&m);
    char *dup = g_strdup(text);
    char *fb = strstr(dup, "\"fallback\":\"dark\"");
    EXPECT(fb, "fallback token");
    if (fb) {
      GString *rewritten = g_string_new_len(dup, (gssize)(fb - dup));
      g_string_append(rewritten, "\"fallback\":\"wrong\",\"fallback\":\"dark\"");
      g_string_append(rewritten, fb + strlen("\"fallback\":\"dark\""));
      g_free(dup);
      dup = g_string_free(rewritten, FALSE);
    }
    char *path_token = g_strdup_printf("\"path\":\"%s\"", fallback_asset);
    char *at = strstr(dup, path_token);
    EXPECT(at, "path token");
    if (at) {
      GString *rewritten = g_string_new_len(dup, (gssize)(at - dup));
      g_string_append_printf(rewritten, "\"path\":\"/tmp/wrong\",\"path\":\"%s\"",
                             fallback_asset);
      g_string_append(rewritten, at + strlen(path_token));
      g_free(dup);
      dup = g_string_free(rewritten, FALSE);
    }
    g_free(path_token);
    char *file = save_manifest(root, dup);
    Result r = launch(file, old, NULL, NULL);
    EXPECT(r.status == 0, "duplicate %s", r.err);
    char *out = join_path(m.runtime_dir, "omarchy");
    EXPECT(link_str(out, fallback_asset), "duplicate link");
    result_free(&r);
    g_free(out);
    g_free(file);
    g_free(dup);
    g_free(text);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
  const char *keys[] = {"nul\\u0000key", "尾🙂", "\\udcff"};
  for (int k = 0; k < 3; k++) {
    for (int old = 0; old < passes; old++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *dir = quote(m.selection_dir);
      char *name = quote(m.selection_name);
      char *runtime = quote(m.runtime_dir);
      char *dark = object_path(fallback_asset);
      char *light = object_path(g_strconcat(store, "light", NULL));
      char *asset = g_strconcat(store, "key", NULL);
      char *text = g_strdup_printf(
          "{\"fallback\":\"%s\",\"selectionDir\":%s,\"selectionName\":%s,"
          "\"runtimeDir\":%s,\"themes\":{\"dark\":%s,\"light\":%s,\"%s\":"
          "{\"path\":\"%s\"}}}",
          keys[k], dir, name, runtime, dark, light, keys[k], asset);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 0, "key %s %s", keys[k], r.err);
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(link_str(out, asset), "key link");
      result_free(&r);
      g_free(out);
      g_free(file);
      g_free(text);
      g_free(asset);
      g_free(light);
      g_free(dark);
      g_free(runtime);
      g_free(name);
      g_free(dir);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_syntax(void) {
  const char *texts[] = {"\ufeff{}", "{} trailing", "{/*comment*/}",
                         "{\"a\":1,}", "{\"a\":\"\\q\"}", "{\"a\":\"raw\nline\"}",
                         "null", "[]", "{}"};
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(symlink("/old", out) == 0, "create previous selection");
      char *file = save_manifest(root, texts[i]);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 1, "syntax %zu", i);
      EXPECT(link_str(out, "/old"), "syntax unchanged");
      result_free(&r);
      g_free(file);
      g_free(out);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
  for (int old = 0; old < passes; old++) {
    char *root = temp_dir("/tmp");
    Manifest m = setup_manifest(root);
    char *text = standard_manifest(&m);
    char *file = save_manifest(root, text);
    unsigned char bad = 0xff;
    write_file(file, &bad, 1);
    Result r = launch(file, old, NULL, NULL);
    EXPECT(r.status == 1, "invalid utf8");
    EXPECT(temp_count(m.runtime_dir) == 0, "utf8 empty");
    result_free(&r);
    g_free(file);
    g_free(text);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
}

static void test_unused_tokens(void) {
  const char *ok[] = {"NaN", "Infinity", "-Infinity", "1e999", "-1e999", NULL,
                      "\"\\ud800\"", "\"\\ud800\\udc80\"",
                      "\"\\ud800\\u0041\\udcff\"", "\"\\\\ud800\""};
  char *hundred = g_strnfill(100, '1');
  const char *bad[] = {"Inf", "nan", "NAN", "infinity", "-NaN", "01", "+1",
                       NULL};
  char *huge = g_strnfill(4301, '1');
  int passes = legacy_enabled() ? 2 : 1;
  for (int group = 0; group < 2; group++) {
    const char **tokens = group ? bad : ok;
    int count = group ? 8 : 10;
    for (int t = 0; t < count; t++) {
      const char *token = tokens[t];
      if (!token)
        token = group ? huge : hundred;
      for (int old = 0; old < passes; old++) {
        char *root = temp_dir("/tmp");
        Manifest m = setup_manifest(root);
        char *text = standard_manifest(&m);
        char *at = strstr(text, "\"fallback\"");
        EXPECT(at, "fallback marker");
        GString *body = g_string_new("{\"unused\":");
        g_string_append(body, token);
        g_string_append(body, ",");
        if (at)
          g_string_append(body, at);
        char *file = save_manifest(root, body->str);
        Result r = launch(file, old, NULL, NULL);
        EXPECT(r.status == (group ? 1 : 0), "token %s status %d", token,
               r.status);
        if (group)
          EXPECT(temp_count(m.runtime_dir) == 0, "bad token empty");
        result_free(&r);
        g_string_free(body, TRUE);
        g_free(file);
        g_free(text);
        manifest_free(&m);
        rm_rf(root);
        g_free(root);
      }
    }
  }
  g_free(hundred);
  g_free(huge);
}

static void test_surrogates(void) {
  const char *suffixes[] = {"\\udcff", "\\ud800", "\\udc00"};
  int expect[] = {0, 1, 1};
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (int i = 0; i < 3; i++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *dir = quote(m.selection_dir);
      char *name = quote(m.selection_name);
      char *runtime = quote(m.runtime_dir);
      char *light = object_path(g_strconcat(store, "light", NULL));
      char *text = g_strdup_printf(
          "{\"fallback\":\"dark\",\"selectionDir\":%s,\"selectionName\":%s,"
          "\"runtimeDir\":%s,\"themes\":{\"dark\":{\"path\":\"%s%s\"},"
          "\"light\":%s}}",
          dir, name, runtime, store, suffixes[i], light);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == expect[i], "surrogate %s %s", suffixes[i], r.err);
      if (!r.status) {
        char *out = join_path(m.runtime_dir, "omarchy");
        char *want = g_malloc(strlen(store) + 1);
        memcpy(want, store, strlen(store));
        want[strlen(store)] = (char)255;
        EXPECT(link_eq(out, want, strlen(store) + 1), "surrogate bytes");
        g_free(want);
        g_free(out);
      }
      EXPECT(temp_count(m.runtime_dir) == 0, "surrogate temps");
      result_free(&r);
      g_free(file);
      g_free(text);
      g_free(light);
      g_free(runtime);
      g_free(name);
      g_free(dir);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_nul_fields(void) {
  const char *fields[] = {"selectionDir", "selectionName", "runtimeDir"};
  int passes = legacy_enabled() ? 2 : 1;
  for (int f = 0; f < 3; f++) {
    for (int old = 0; old < passes; old++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *dir = quote(m.selection_dir);
      char *name = quote(m.selection_name);
      char *runtime = quote(m.runtime_dir);
      char *dark = object_path(fallback_asset);
      char *light = object_path(g_strconcat(store, "light", NULL));
      const char *dir_json = dir, *name_json = name, *runtime_json = runtime;
      char *extra = NULL;
      if (f == 0)
        extra = g_strdup_printf("%s\\u0000suffix\"", g_strndup(dir, strlen(dir) - 1));
      if (f == 1)
        extra = g_strdup_printf("%s\\u0000suffix\"", g_strndup(name, strlen(name) - 1));
      if (f == 2)
        extra = g_strdup_printf("%s\\u0000suffix\"",
                                g_strndup(runtime, strlen(runtime) - 1));
      if (f == 0)
        dir_json = extra;
      if (f == 1)
        name_json = extra;
      if (f == 2)
        runtime_json = extra;
      char *text = g_strdup_printf(
          "{\"fallback\":\"dark\",\"selectionDir\":%s,\"selectionName\":%s,"
          "\"runtimeDir\":%s,\"themes\":{\"dark\":%s,\"light\":%s}}",
          dir_json, name_json, runtime_json, dark, light);
      char *file = save_manifest(root, text);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 1, "nul %s", fields[f]);
      char *real = join_path(root, "runtime");
      EXPECT(temp_count(real) == 0, "nul runtime");
      result_free(&r);
      g_free(real);
      g_free(file);
      g_free(text);
      g_free(extra);
      g_free(light);
      g_free(dark);
      g_free(runtime);
      g_free(name);
      g_free(dir);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

static void test_read_faults(void) {
  const char *modes[] = {"stat", "read", "close"};
  for (int i = 0; i < 3; i++) {
    char *root = temp_dir("/tmp");
    Manifest m = setup_manifest(root);
    char *leaf = join_path(m.selection_dir, "william");
    write_str(leaf, "light");
    char *text = standard_manifest(&m);
    char *file = save_manifest(root, text);
    const char *keys[] = {"GREETER_FIXTURE_SELECTION"};
    const char *vals[] = {leaf};
    char **env = fault_env(modes[i], keys, vals, 1);
    Result r = launch(file, false, env, NULL);
    EXPECT(r.status == 1, "fault %s %s", modes[i], r.err);
    EXPECT(temp_count(m.runtime_dir) == 0, "fault empty");
    result_free(&r);
    free_env(env);
    g_free(file);
    g_free(text);
    g_free(leaf);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
}

static void test_short_read(void) {
  char *root = temp_dir("/tmp");
  Manifest m = setup_manifest(root);
  char *leaf = join_path(m.selection_dir, "william");
  write_str(leaf, "light");
  char *short_path = g_strconcat(store, "short", NULL);
  char *dark = object_path(fallback_asset);
  char *light = object_path(g_strconcat(store, "light", NULL));
  char *extra = object_path(short_path);
  char *fb = quote(m.fallback);
  char *dir = quote(m.selection_dir);
  char *name = quote(m.selection_name);
  char *runtime = quote(m.runtime_dir);
  char *text = g_strdup_printf(
      "{\"fallback\":%s,\"selectionDir\":%s,\"selectionName\":%s,"
      "\"runtimeDir\":%s,\"themes\":{\"dark\":%s,\"light\":%s,\"li\":%s}}",
      fb, dir, name, runtime, dark, light, extra);
  char *file = save_manifest(root, text);
  const char *keys[] = {"GREETER_FIXTURE_SELECTION"};
  const char *vals[] = {leaf};
  char **env = fault_env("short-read", keys, vals, 1);
  Result r = launch(file, false, env, NULL);
  EXPECT(r.status == 0, "short read %s", r.err);
  char *out = join_path(m.runtime_dir, "omarchy");
  EXPECT(link_str(out, short_path), "short link");
  result_free(&r);
  free_env(env);
  g_free(out);
  g_free(file);
  g_free(text);
  g_free(runtime);
  g_free(name);
  g_free(dir);
  g_free(fb);
  g_free(extra);
  g_free(light);
  g_free(dark);
  g_free(short_path);
  g_free(leaf);
  manifest_free(&m);
  rm_rf(root);
  g_free(root);
}

static void test_publish_faults(void) {
  const char *modes[] = {"create", "symlink", "rename"};
  for (int i = 0; i < 3; i++) {
    char *root = temp_dir("/tmp");
    Manifest m = setup_manifest(root);
    char *out = join_path(m.runtime_dir, "omarchy");
    EXPECT(symlink("/old", out) == 0, "create previous selection");
    char *existing = join_path(m.runtime_dir, ".theme-ownedx");
    if (!strcmp(modes[i], "create")) {
      mkdir(existing, 0755);
      char *keep = join_path(existing, "keep");
      write_str(keep, "unowned");
      g_free(keep);
    }
    char *text = standard_manifest(&m);
    char *file = save_manifest(root, text);
    char **env = fault_env(modes[i], NULL, NULL, 0);
    Result r = launch(file, false, env, NULL);
    EXPECT(r.status == 1, "publish %s", modes[i]);
    EXPECT(link_str(out, "/old"), "old retained");
    if (!strcmp(modes[i], "create")) {
      char *keep = join_path(existing, "keep");
      char buf[16];
      int fd = open(keep, O_RDONLY);
      ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
      if (fd >= 0)
        close(fd);
      EXPECT(n == 7 && !memcmp(buf, "unowned", 7), "unowned kept");
      g_free(keep);
    } else
      EXPECT(temp_count(m.runtime_dir) == 0, "publish temps");
    result_free(&r);
    free_env(env);
    g_free(file);
    g_free(text);
    g_free(existing);
    g_free(out);
    manifest_free(&m);
    rm_rf(root);
    g_free(root);
  }
}

static void test_cleanup_failure(void) {
  char *root = temp_dir("/tmp");
  Manifest m = setup_manifest(root);
  char *out = join_path(m.runtime_dir, "omarchy");
  EXPECT(symlink("/old", out) == 0, "create previous selection");
  char *text = standard_manifest(&m);
  char *file = save_manifest(root, text);
  char **env = fault_env("rmdir", NULL, NULL, 0);
  Result r = launch(file, false, env, NULL);
  EXPECT(r.status == 1, "rmdir %s", r.err);
  EXPECT(link_str(out, fallback_asset), "link kept");
  EXPECT(temp_count(m.runtime_dir) == 1, "temp remains");
  DIR *dir = opendir(m.runtime_dir);
  struct dirent *ent;
  char *temp = NULL;
  while (dir && (ent = readdir(dir)))
    if (!strncmp(ent->d_name, ".theme-", 7))
      temp = join_path(m.runtime_dir, ent->d_name);
  if (dir)
    closedir(dir);
  EXPECT(temp && temp_count(temp) == 0, "temp empty");
  struct stat st;
  EXPECT(temp && stat(temp, &st) == 0 && (st.st_mode & 0777) == 0700,
         "temp mode");
  result_free(&r);
  free_env(env);
  g_free(temp);
  g_free(file);
  g_free(text);
  g_free(out);
  manifest_free(&m);
  rm_rf(root);
  g_free(root);
}

static char *first_temp(const char *runtime) {
  DIR *dir = opendir(runtime);
  if (!dir)
    return NULL;
  struct dirent *ent;
  char *found = NULL;
  while ((ent = readdir(dir)))
    if (!strncmp(ent->d_name, ".theme-", 7)) {
      found = g_strdup(ent->d_name);
      break;
    }
  closedir(dir);
  return found;
}

static void test_publication_wait(void) {
  char *root = temp_dir("/tmp");
  Manifest m = setup_manifest(root);
  char *out = join_path(m.runtime_dir, "omarchy");
  EXPECT(symlink("/old", out) == 0, "create previous selection");
  char *release = join_path(root, "release");
  char *text = standard_manifest(&m);
  char *file = save_manifest(root, text);
  const char *keys[] = {"GREETER_FIXTURE_RELEASE"};
  const char *vals[] = {release};
  char **env = fault_env("", keys, vals, 1);
  const char *args[] = {file};
  char **argv = program_argv(false, args, 1);
  Result child = start_proc(argv, env, NULL);
  char *directory = NULL;
  for (int i = 0; i < 400 && !directory; i++) {
    directory = first_temp(m.runtime_dir);
    if (!directory) {
      struct timespec pause = {.tv_nsec = 5 * 1000000L};
      nanosleep(&pause, NULL);
    }
  }
  EXPECT(directory, "private dir appeared");
  if (directory) {
    char *full = join_path(m.runtime_dir, directory);
    struct stat st;
    EXPECT(stat(full, &st) == 0 && (st.st_mode & 0777) == 0700, "private mode");
    EXPECT(link_str(out, "/old"), "old during wait");
    char *inner = join_path(full, "omarchy");
    EXPECT(link_str(inner, fallback_asset), "staged link");
    g_free(inner);
    g_free(full);
  }
  write_str(release, "go");
  finish_proc(&child, 10000);
  EXPECT(!child.error && child.status == 0, "wait status %d %s", child.status,
         child.err);
  EXPECT(link_str(out, fallback_asset), "published");
  EXPECT(temp_count(m.runtime_dir) == 0, "wait cleaned");
  result_free(&child);
  free_argv(argv);
  free_env(env);
  g_free(directory);
  g_free(file);
  g_free(text);
  g_free(release);
  g_free(out);
  manifest_free(&m);
  rm_rf(root);
  g_free(root);
}

static char *hex_of(const void *data, size_t n) {
  static const char hex[] = "0123456789abcdef";
  char *out = g_malloc(n * 2 + 1);
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hex[p[i] >> 4];
    out[i * 2 + 1] = hex[p[i] & 15];
  }
  out[n * 2] = 0;
  return out;
}

static void test_helpers(void) {
  struct {
    const char *mode;
    const char *value;
    size_t len;
    bool expect;
  } items[] = {
      {"id", "a", 1, true},
      {"id", "a-9\n", 4, true},
      {"id", "a\n\n", 3, false},
      {"id", "a\0b", 3, false},
      {"id", "a--b", 4, false},
      {"id", NULL, 100, true},
      {"store", NULL, 0, true},
      {"store", "/nix/store/", 11, true},
      {"store", "/nix/store/a/../b", 17, false},
      {"store", "/nix/store/a/..b", 16, true},
      {"store", "/nix/store/a\0b", 14, false},
  };
  char *hundred = g_strnfill(100, 'a');
  for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
    const char *value = items[i].value;
    size_t len = items[i].len;
    if (!strcmp(items[i].mode, "id") && !value) {
      value = hundred;
      len = 100;
    }
    if (!strcmp(items[i].mode, "store") && !value) {
      value = store;
      len = strlen(store);
    }
    char *hex = hex_of(value, len);
    char **argv = g_new0(char *, 4);
    argv[0] = g_strdup(getenv("GREETER_SELECT_FIXTURE"));
    argv[1] = g_strdup(items[i].mode);
    argv[2] = hex;
    char **env = copy_env();
    Result r = start_proc(argv, env, NULL);
    finish_proc(&r, 10000);
    EXPECT(r.status == 0, "helper %s", items[i].mode);
    while (r.out_len && (r.out[r.out_len - 1] == '\n' || r.out[r.out_len - 1] == '\r'))
      r.out[--r.out_len] = 0;
    EXPECT(r.out && !strcmp(r.out, items[i].expect ? "true" : "false"),
           "helper value %s", items[i].mode);
    result_free(&r);
    free_env(env);
    free_argv(argv);
  }
  g_free(hundred);
  char *root = temp_dir("/tmp");
  Manifest m = setup_manifest(root);
  char *leaf = join_path(m.selection_dir, "william");
  write_str(leaf, "light\n");
  char **argv = g_new0(char *, 5);
  argv[0] = g_strdup(getenv("GREETER_SELECT_FIXTURE"));
  argv[1] = g_strdup("read");
  argv[2] = g_strdup(m.selection_dir);
  argv[3] = g_strdup("william");
  char **env = copy_env();
  Result r = start_proc(argv, env, NULL);
  finish_proc(&r, 10000);
  EXPECT(r.status == 0, "read helper %s", r.err);
  EXPECT(r.out_len == 6 && !memcmp(r.out, "light\n", 6), "read bytes");
  result_free(&r);
  free_env(env);
  free_argv(argv);
  g_free(leaf);
  manifest_free(&m);
  rm_rf(root);
  g_free(root);
}

static void test_raw_argv(void) {
  char *root = temp_dir("/tmp");
  Manifest m = setup_manifest(root);
  char *text = standard_manifest(&m);
  GString *file = g_string_new(root);
  g_string_append(file, "/manifest-");
  g_string_append_c(file, (char)255);
  write_str(file->str, text);
  char *hex = hex_of(file->str, file->len);
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char **argv = g_new0(char *, 6);
    argv[0] = g_strdup(getenv("GREETER_SELECT_FIXTURE"));
    argv[1] = g_strdup("raw");
    argv[2] = g_strdup(hex);
    if (old) {
      const char *python = getenv("GREETER_SELECT_PYTHON");
      argv[3] = g_strdup(python && *python ? python : "python3");
      argv[4] = g_strdup(getenv("GREETER_SELECT_LEGACY"));
    } else
      argv[3] = g_strdup(getenv("GREETER_SELECT_BIN"));
    char **env = copy_env();
    Result r = start_proc(argv, env, NULL);
    finish_proc(&r, 10000);
    EXPECT(r.status == 0, "raw %s", r.err);
    char *out = join_path(m.runtime_dir, "omarchy");
    EXPECT(link_str(out, fallback_asset), "raw link");
    g_free(out);
    result_free(&r);
    free_env(env);
    free_argv(argv);
  }
  g_free(hex);
  g_string_free(file, TRUE);
  g_free(text);
  manifest_free(&m);
  rm_rf(root);
  g_free(root);
}

static bool arr_has(yyjson_val *arr, const char *needle) {
  if (!arr || !yyjson_is_arr(arr))
    return false;
  size_t i, n;
  yyjson_val *item;
  yyjson_arr_foreach(arr, i, n, item) if (yyjson_is_str(item) &&
                                          !strcmp(yyjson_get_str(item), needle))
      return true;
  return false;
}

static void test_generated(void) {
  const char *path = getenv("GREETER_SELECT_MANIFEST");
  if (!path) {
    fprintf(stderr, "SKIP generated catalogue\n");
    return;
  }
  size_t n = 0;
  gchar *text = NULL;
  if (!g_file_get_contents(path, &text, &n, NULL)) {
    FAIL("manifest read");
    return;
  }
  yyjson_doc *doc = yyjson_read(text, n, 0);
  g_free(text);
  EXPECT(doc, "manifest json");
  if (!doc)
    return;
  yyjson_val *themes = yyjson_obj_get(yyjson_doc_get_root(doc), "themes");
  EXPECT(themes && yyjson_obj_size(themes) > 0, "themes");
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_dir("/tmp");
    Manifest local = setup_manifest(root);
    yyjson_mut_doc *mut = yyjson_doc_mut_copy(doc, NULL);
    yyjson_mut_val *root_val = yyjson_mut_doc_get_root(mut);
    yyjson_mut_obj_remove_key(root_val, "selectionDir");
    yyjson_mut_obj_remove_key(root_val, "selectionName");
    yyjson_mut_obj_remove_key(root_val, "runtimeDir");
    yyjson_mut_obj_add_strcpy(mut, root_val, "selectionDir", local.selection_dir);
    yyjson_mut_obj_add_strcpy(mut, root_val, "selectionName", local.selection_name);
    yyjson_mut_obj_add_strcpy(mut, root_val, "runtimeDir", local.runtime_dir);
    size_t written = 0;
    char *body = yyjson_mut_write(mut, 0, &written);
    yyjson_mut_doc_free(mut);
    char *file = save_manifest(root, body);
    free(body);
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(themes, i, count, key, value) {
      const char *id = yyjson_get_str(key);
      size_t id_len = yyjson_get_len(key);
      char *leaf = join_path(local.selection_dir, local.selection_name);
      GString *sel = g_string_new_len(id, (gssize)id_len);
      g_string_append_c(sel, '\n');
      write_file(leaf, sel->str, sel->len);
      g_string_free(sel, TRUE);
      Result r = launch(file, old, NULL, NULL);
      EXPECT(r.status == 0, "generated %s %s", id, r.err);
      yyjson_val *asset = yyjson_obj_get(value, "path");
      char *out = join_path(local.runtime_dir, "omarchy");
      EXPECT(asset &&
                 link_eq(out, yyjson_get_str(asset), yyjson_get_len(asset)),
             "generated link %s", id);
      EXPECT(temp_count(local.runtime_dir) == 0, "generated temps");
      result_free(&r);
      g_free(out);
      g_free(leaf);
    }
    g_free(file);
    manifest_free(&local);
    rm_rf(root);
    g_free(root);
  }
  yyjson_doc_free(doc);
}

static void test_wiring(void) {
  const char *path = getenv("GREETER_SELECT_WIRING");
  if (!path) {
    fprintf(stderr, "SKIP wiring\n");
    return;
  }
  gchar *text = NULL;
  if (!g_file_get_contents(path, &text, NULL, NULL)) {
    FAIL("wiring read");
    return;
  }
  yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
  g_free(text);
  EXPECT(doc, "wiring json");
  if (!doc)
    return;
  yyjson_val *root = yyjson_doc_get_root(doc);
  EXPECT(yyjson_is_null(yyjson_obj_get(root, "terminus")), "terminus");
  const char *hosts[] = {"foundation", "andromeda", "starfish"};
  regex_t exec_re, python_re;
  EXPECT(regcomp(&exec_re,
                 "/bin/greeter-select /nix/store/[^ ]+-sddm-themes\\.json$",
                 REG_EXTENDED | REG_NOSUB) == 0,
         "exec regex");
  EXPECT(regcomp(&python_re, "python", REG_EXTENDED | REG_NOSUB) == 0,
         "python regex");
  for (int i = 0; i < 3; i++) {
    yyjson_val *c = yyjson_obj_get(root, hosts[i]);
    yyjson_val *s = yyjson_obj_get(c, "serviceConfig");
    yyjson_val *sddm = yyjson_obj_get(c, "sddm");
    EXPECT(yyjson_is_bool(yyjson_obj_get(c, "autoLogin")) &&
               !yyjson_get_bool(yyjson_obj_get(c, "autoLogin")),
           "autologin");
    EXPECT(!yyjson_obj_get(sddm, "Autologin"), "sddm autologin");
    EXPECT(yyjson_is_str(yyjson_obj_get(yyjson_obj_get(sddm, "General"),
                                        "GreeterEnvironment")) &&
               !strcmp(yyjson_get_str(yyjson_obj_get(
                           yyjson_obj_get(sddm, "General"), "GreeterEnvironment")),
                       "QML_DISABLE_DISK_CACHE=1"),
           "qml");
    EXPECT(yyjson_is_str(yyjson_obj_get(s, "Type")) &&
               !strcmp(yyjson_get_str(yyjson_obj_get(s, "Type")), "oneshot"),
           "type");
    EXPECT(yyjson_is_str(yyjson_obj_get(s, "User")) &&
               !strcmp(yyjson_get_str(yyjson_obj_get(s, "User")), "root"),
           "user");
    EXPECT(yyjson_is_str(yyjson_obj_get(s, "UMask")) &&
               !strcmp(yyjson_get_str(yyjson_obj_get(s, "UMask")), "0022"),
           "umask");
    EXPECT(yyjson_get_bool(yyjson_obj_get(s, "NoNewPrivileges")), "privs");
    EXPECT(yyjson_get_bool(yyjson_obj_get(s, "PrivateTmp")), "tmp");
    EXPECT(yyjson_get_bool(yyjson_obj_get(s, "ProtectHome")), "home");
    EXPECT(yyjson_is_str(yyjson_obj_get(s, "ProtectSystem")) &&
               !strcmp(yyjson_get_str(yyjson_obj_get(s, "ProtectSystem")),
                       "strict"),
           "system");
    yyjson_val *rw = yyjson_obj_get(s, "ReadWritePaths");
    EXPECT(yyjson_arr_size(rw) == 1 &&
               !strcmp(yyjson_get_str(yyjson_arr_get(rw, 0)), "/run/desktop-login"),
           "rw");
    yyjson_val *af = yyjson_obj_get(s, "RestrictAddressFamilies");
    EXPECT(yyjson_arr_size(af) == 1 &&
               !strcmp(yyjson_get_str(yyjson_arr_get(af, 0)), "AF_UNIX"),
           "af");
    const char *exec = yyjson_get_str(yyjson_obj_get(s, "ExecStart"));
    EXPECT(exec && regexec(&exec_re, exec, 0, NULL, 0) == 0, "exec");
    EXPECT(exec && regexec(&python_re, exec, 0, NULL, 0) != 0, "no python");
    EXPECT(arr_has(yyjson_obj_get(c, "after"), "systemd-tmpfiles-setup.service"),
           "after");
    EXPECT(arr_has(yyjson_obj_get(c, "before"), "display-manager.service"),
           "before");
    EXPECT(yyjson_is_str(yyjson_obj_get(c, "requiresMountsFor")) &&
               !strcmp(yyjson_get_str(yyjson_obj_get(c, "requiresMountsFor")),
                       "/var/lib/desktop-theme"),
           "mounts");
    yyjson_val *path = yyjson_obj_get(c, "path");
    EXPECT(yyjson_is_str(yyjson_obj_get(path, "PathChanged")) &&
               !strcmp(yyjson_get_str(yyjson_obj_get(path, "PathChanged")),
                       "/var/lib/desktop-theme/william"),
           "path changed");
    EXPECT(yyjson_is_str(yyjson_obj_get(path, "Unit")) &&
               !strcmp(yyjson_get_str(yyjson_obj_get(path, "Unit")),
                       "desktop-login-theme.service"),
           "path unit");
    yyjson_val *dm = yyjson_obj_get(c, "displayManager");
    EXPECT(arr_has(yyjson_obj_get(dm, "requires"), "desktop-login-theme.service"),
           "dm requires");
    EXPECT(arr_has(yyjson_obj_get(dm, "after"), "desktop-login-theme.service"),
           "dm after");
    yyjson_val *tmp = yyjson_obj_get(c, "tmpfiles");
    yyjson_val *run = yyjson_obj_get(yyjson_obj_get(tmp, "/run/desktop-login"), "d");
    yyjson_val *var = yyjson_obj_get(yyjson_obj_get(tmp, "/var/lib/desktop-theme"), "d");
    yyjson_val *sel =
        yyjson_obj_get(yyjson_obj_get(tmp, "/var/lib/desktop-theme/william"), "f");
    EXPECT(run && !strcmp(yyjson_get_str(yyjson_obj_get(run, "user")), "root"),
           "run user");
    EXPECT(run && !strcmp(yyjson_get_str(yyjson_obj_get(run, "mode")), "0755"),
           "run mode");
    EXPECT(var && !strcmp(yyjson_get_str(yyjson_obj_get(var, "user")), "root"),
           "var user");
    EXPECT(sel && !strcmp(yyjson_get_str(yyjson_obj_get(sel, "user")), "william"),
           "sel user");
    EXPECT(sel && !strcmp(yyjson_get_str(yyjson_obj_get(sel, "mode")), "0644"),
           "sel mode");
  }
  regfree(&exec_re);
  regfree(&python_re);
  yyjson_doc_free(doc);
}

static void test_cli(void) {
  const char *bad[][3] = {{NULL}, {"one", "two"}, {"--unknown"},
                          {"--help=bad"}, {"--h=x", "-h"}, {"-h=foo", "-h"}};
  int bad_n[] = {0, 2, 1, 1, 2, 2};
  const char *good[][3] = {{"-h"},     {"--help"}, {"--h"},
                           {"one", "two", "-h"}, {"-hh"}, {"-hfoo"},
                           {"--unknown", "-h"}};
  int good_n[] = {1, 1, 1, 3, 1, 1, 2};
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (int i = 0; i < 6; i++) {
      char **argv = program_argv(old, bad[i], bad_n[i]);
      char **env = copy_env();
      Result r = start_proc(argv, env, NULL);
      finish_proc(&r, 10000);
      EXPECT(r.status == 2, "cli bad %d", i);
      result_free(&r);
      free_env(env);
      free_argv(argv);
    }
    for (int i = 0; i < 7; i++) {
      char **argv = program_argv(old, good[i], good_n[i]);
      char **env = copy_env();
      Result r = start_proc(argv, env, NULL);
      finish_proc(&r, 10000);
      EXPECT(r.status == 0, "cli help %d", i);
      EXPECT(contains_mem(r.out, r.out_len, "manifest"), "help text");
      result_free(&r);
      free_env(env);
      free_argv(argv);
    }
  }
  const char *names[] = {"--help", "-1", "-.5", "-١"};
  for (int old = 0; old < passes; old++) {
    for (int i = 0; i < 4; i++) {
      char *root = temp_dir("/tmp");
      Manifest m = setup_manifest(root);
      char *text = standard_manifest(&m);
      char *file = join_path(root, names[i]);
      write_str(file, text);
      const char *args[3];
      int n = 0;
      if (!strcmp(names[i], "--help"))
        args[n++] = "--";
      args[n++] = names[i];
      char **argv = program_argv(old, args, n);
      char **env = copy_env();
      Result r = start_proc(argv, env, root);
      finish_proc(&r, 10000);
      EXPECT(r.status == 0, "filename %s %s", names[i], r.err);
      char *out = join_path(m.runtime_dir, "omarchy");
      EXPECT(link_str(out, fallback_asset), "filename link");
      result_free(&r);
      free_env(env);
      free_argv(argv);
      g_free(out);
      g_free(file);
      g_free(text);
      manifest_free(&m);
      rm_rf(root);
      g_free(root);
    }
  }
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  if (!getenv("GREETER_SELECT_BIN") || !getenv("GREETER_SELECT_FIXTURE") ||
      !getenv("GREETER_SELECT_FAILURES")) {
    fprintf(stderr, "Set binary, fixture and failure-library paths\n");
    return 1;
  }
  fallback_asset = g_strconcat(store, "fallback", NULL);
  test_allowlist();
  test_id_shape();
  test_bound();
  test_symlink_selection();
  test_nonregular();
  test_relative_names();
  test_absent_fallback();
  test_store_reject();
  test_store_spellings();
  test_malformed_selected();
  test_runtime_rejected();
  test_owner();
  test_permissions();
  test_atomic();
  test_output_dir();
  test_duplicates();
  test_syntax();
  test_unused_tokens();
  test_surrogates();
  test_nul_fields();
  test_read_faults();
  test_short_read();
  test_publish_faults();
  test_cleanup_failure();
  test_publication_wait();
  test_helpers();
  test_raw_argv();
  test_generated();
  test_wiring();
  test_cli();
  g_free(fallback_asset);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
