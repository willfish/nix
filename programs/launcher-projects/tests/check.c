#define _GNU_SOURCE
#include <errno.h>
#include <glib.h>
#include <fcntl.h>
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
#include <yyjson.h>

extern char **environ;
static const char *current_test;
static int failures;
static char *binary, *fixture, *fake_bin, *legacy, *python;

static void failf(int line, const char *fmt, ...) {
  fprintf(stderr, "FAIL %s:%d: ", current_test, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  failures++;
}
#define FAIL(...) failf(__LINE__, __VA_ARGS__)
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      FAIL("expected true: %s", #cond);                                        \
      return;                                                                  \
    }                                                                          \
  } while (0)

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static char *sibling(const char *env, const char *name) {
  const char *v = env ? getenv(env) : NULL;
  if (v && *v)
    return strdup(v);
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n < 0)
    return NULL;
  exe[n] = 0;
  char *slash = strrchr(exe, '/');
  if (!slash)
    return NULL;
  size_t dir = (size_t)(slash - exe), nl = strlen(name);
  char *path = malloc(dir + 1 + nl + 1);
  memcpy(path, exe, dir);
  path[dir] = '/';
  memcpy(path + dir + 1, name, nl + 1);
  return path;
}
static char *find_on_path(const char *name) {
  const char *path = getenv("PATH");
  if (!path)
    return NULL;
  char *copy = strdup(path), *save = NULL;
  for (char *part = strtok_r(copy, ":", &save); part; part = strtok_r(NULL, ":", &save)) {
    size_t pl = strlen(part), nl = strlen(name);
    char *candidate = malloc(pl + 1 + nl + 1);
    memcpy(candidate, part, pl);
    candidate[pl] = '/';
    memcpy(candidate + pl + 1, name, nl + 1);
    if (access(candidate, X_OK) == 0) {
      free(copy);
      return candidate;
    }
    free(candidate);
  }
  free(copy);
  return NULL;
}
static bool grow(char **buf, size_t *len, size_t *cap, const void *add, size_t n) {
  if (*len + n + 1 < *len)
    return false;
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1)
      next *= 2;
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
typedef struct {
  int status, signal;
  bool error;
  char *out, *err;
  size_t out_len, err_len;
} Proc;
static void proc_clear(Proc *p) {
  free(p->out);
  free(p->err);
  memset(p, 0, sizeof *p);
}
static char **env_with(const char *const *overrides) {
  size_t n = 0, m = 0;
  while (environ[n])
    n++;
  while (overrides && overrides[m])
    m++;
  char **out = calloc(n + m + 1, sizeof *out);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const char *eq = strchr(environ[i], '=');
    size_t kl = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
    bool skip = false;
    for (size_t j = 0; overrides && overrides[j]; j++) {
      const char *oq = strchr(overrides[j], '=');
      size_t ol = oq ? (size_t)(oq - overrides[j]) : strlen(overrides[j]);
      if (ol == kl && !memcmp(environ[i], overrides[j], kl)) {
        skip = true;
        break;
      }
    }
    if (!skip)
      out[k++] = strdup(environ[i]);
  }
  for (size_t j = 0; j < m; j++)
    out[k++] = strdup(overrides[j]);
  return out;
}
static void env_free(char **env) {
  if (!env)
    return;
  for (size_t i = 0; env[i]; i++)
    free(env[i]);
  free(env);
}
static Proc proc_run(const char *file, char *const *argv, const char *cwd, char **envp, int timeout_ms) {
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
    setpgid(0, 0);
    dup2(inp[0], 0);
    dup2(outp[1], 1);
    dup2(errp[1], 2);
    close(inp[0]);
    close(inp[1]);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    if (cwd && chdir(cwd) != 0)
      _exit(127);
    execvpe(file, argv, envp ? envp : environ);
    _exit(127);
  }
  setpgid(pid, pid);
  close(inp[0]);
  close(inp[1]);
  close(outp[1]);
  close(errp[1]);
  fcntl(outp[0], F_SETFL, O_NONBLOCK);
  fcntl(errp[0], F_SETFL, O_NONBLOCK);
  size_t ocap = 0, ecap = 0;
  int64_t deadline = now_ms() + timeout_ms;
  while (outp[0] >= 0 || errp[0] >= 0) {
    if (now_ms() > deadline) {
      kill(-pid, SIGKILL);
      p.error = true;
      break;
    }
    struct pollfd fds[2];
    nfds_t nfd = 0;
    int oi = -1, ei = -1;
    if (outp[0] >= 0)
      fds[nfd] = (struct pollfd){outp[0], POLLIN, 0}, oi = (int)nfd++;
    if (errp[0] >= 0)
      fds[nfd] = (struct pollfd){errp[0], POLLIN, 0}, ei = (int)nfd++;
    poll(fds, nfd, 50);
    char tmp[8192];
    if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t r = read(outp[0], tmp, sizeof tmp);
      if (r > 0) {
        if (!grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r))
          p.error = true;
      } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
        close(outp[0]);
        outp[0] = -1;
      }
    }
    if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t r = read(errp[0], tmp, sizeof tmp);
      if (r > 0) {
        if (!grow(&p.err, &p.err_len, &ecap, tmp, (size_t)r))
          p.error = true;
      } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
        close(errp[0]);
        errp[0] = -1;
      }
    }
  }
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
    p.error = true;
  } else
    p.error = true;
  if (!p.out)
    grow(&p.out, &p.out_len, &ocap, "", 0);
  if (!p.err)
    grow(&p.err, &p.err_len, &ecap, "", 0);
  return p;
}
static void make_link(const char *target, const char *path) {
  if (symlink(target, path) != 0)
    FAIL("symlink %s", path);
}
static char *path_fmt(const char *a, const char *b) {
  size_t al = strlen(a), bl = strlen(b);
  char *s = malloc(al + 1 + bl + 1);
  memcpy(s, a, al);
  s[al] = '/';
  memcpy(s + al + 1, b, bl + 1);
  return s;
}
static char *env_pair(const char *key, const char *value) {
  size_t kl = strlen(key), vl = strlen(value);
  char *s = malloc(kl + vl + 2);
  memcpy(s, key, kl);
  s[kl] = '=';
  memcpy(s + kl + 1, value, vl + 1);
  return s;
}
static bool mkdir_p(const char *path) {
  char *copy = strdup(path);
  for (char *p = copy + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
        free(copy);
        return false;
      }
      *p = '/';
    }
  }
  bool ok = mkdir(copy, 0700) == 0 || errno == EEXIST;
  free(copy);
  return ok;
}
static bool write_file(const char *path, const void *data, size_t len, mode_t mode) {
  char *dir = strdup(path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    if (!mkdir_p(dir)) {
      free(dir);
      return false;
    }
  }
  free(dir);
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
  return fchmod(fd, mode) == 0 && close(fd) == 0;
}
static char *read_file(const char *path, size_t *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL;
  char *buf = NULL;
  size_t cap = 0, n = 0;
  char tmp[4096];
  ssize_t got;
  while ((got = read(fd, tmp, sizeof tmp)) > 0) {
    if (!grow(&buf, &n, &cap, tmp, (size_t)got)) {
      free(buf);
      close(fd);
      return NULL;
    }
  }
  close(fd);
  if (got < 0) {
    free(buf);
    return NULL;
  }
  if (!buf && !grow(&buf, &n, &cap, "", 0))
    return NULL;
  if (len)
    *len = n;
  return buf;
}
static void rm_rf(const char *path) {
  char *argv[] = {"rm", "-rf", "--", (char *)path, NULL};
  pid_t pid = fork();
  if (!pid) {
    execvp(argv[0], argv);
    _exit(127);
  }
  if (pid > 0)
    waitpid(pid, NULL, 0);
}
static bool json_eq(yyjson_val *a, yyjson_val *b) {
  if (!a || !b)
    return a == b;
  if (yyjson_is_str(a) || yyjson_is_str(b))
    return yyjson_is_str(a) && yyjson_is_str(b) && yyjson_get_len(a) == yyjson_get_len(b) &&
           memcmp(yyjson_get_str(a), yyjson_get_str(b), yyjson_get_len(a)) == 0;
  if (yyjson_is_bool(a) || yyjson_is_bool(b))
    return yyjson_is_bool(a) && yyjson_is_bool(b) && yyjson_get_bool(a) == yyjson_get_bool(b);
  if (yyjson_is_null(a) || yyjson_is_null(b))
    return yyjson_is_null(a) && yyjson_is_null(b);
  if (yyjson_is_num(a) || yyjson_is_num(b)) {
    if (!yyjson_is_num(a) || !yyjson_is_num(b))
      return false;
    if (yyjson_is_int(a) && yyjson_is_int(b))
      return yyjson_get_sint(a) == yyjson_get_sint(b);
    return yyjson_get_num(a) == yyjson_get_num(b);
  }
  if (yyjson_is_arr(a) || yyjson_is_arr(b)) {
    if (!yyjson_is_arr(a) || !yyjson_is_arr(b) || yyjson_arr_size(a) != yyjson_arr_size(b))
      return false;
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(a, i, n, v) if (!json_eq(v, yyjson_arr_get(b, i))) return false;
    return true;
  }
  if (!yyjson_is_obj(a) || !yyjson_is_obj(b) || yyjson_obj_size(a) != yyjson_obj_size(b))
    return false;
  size_t i, n;
  yyjson_val *k, *v;
  yyjson_obj_foreach(a, i, n, k, v) if (!json_eq(v, yyjson_obj_getn(b, yyjson_get_str(k), yyjson_get_len(k))))
      return false;
  return true;
}
static bool str_eq(yyjson_val *v, const char *s) {
  size_t n = strlen(s);
  return v && yyjson_is_str(v) && yyjson_get_len(v) == n && memcmp(yyjson_get_str(v), s, n) == 0;
}
static char *hash_bytes(const void *path, size_t len) {
  static const unsigned char prefix[] = {'l', 'a', 'u', 'n', 'c', 'h', 'e', 'r', '-', 'p', 'r', 'o', 'j', 'e', 'c', 't', 0};
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(sum, prefix, sizeof prefix);
  g_checksum_update(sum, path, len);
  char *hex = strdup(g_checksum_get_string(sum));
  g_checksum_free(sum);
  return hex;
}
static char *hash_str(const char *path) { return hash_bytes(path, strlen(path)); }
typedef struct {
  char *root, *home, *outside, *bin, *log;
  char **env;
} Sandbox;
static Sandbox *sandbox_new(void) {
  Sandbox *s = calloc(1, sizeof *s);
  char tmpl[] = "/tmp/launcher-projects-XXXXXX";
  if (!mkdtemp(tmpl))
    return s;
  s->root = strdup(tmpl);
  s->home = path_fmt(s->root, "home");
  s->outside = path_fmt(s->root, "outside");
  s->bin = path_fmt(s->root, "bin");
  s->log = path_fmt(s->root, "log.jsonl");
  mkdir(s->home, 0700);
  mkdir(s->outside, 0700);
  mkdir(s->bin, 0700);
  char *git = path_fmt(s->outside, ".git");
  mkdir(git, 0700);
  free(git);
  write_file(path_fmt(s->outside, "secret-marker"), "private-marker", 14, 0644);
  write_file(s->log, "", 0, 0644);
  char *stub = path_fmt(s->bin, "systemd-run");
  if (symlink(fake_bin, stub) != 0)
    FAIL("symlink systemd-run");
  free(stub);
  char *path = malloc(strlen(s->bin) + 6);
  memcpy(path, "PATH=", 5);
  memcpy(path + 5, s->bin, strlen(s->bin) + 1);
  char *home = env_pair("HOME", s->home);
  char *lhome = env_pair("LAUNCHER_HOME", s->home);
  char *log = env_pair("LP_LOG", s->log);
  const char *extra[] = {home, lhome, path, log, "LP_MARKER=inherited-marker", NULL};
  s->env = env_with(extra);
  free(path);
  free(home);
  free(lhome);
  free(log);
  return s;
}
static void sandbox_free(Sandbox *s) {
  if (!s)
    return;
  if (s->root)
    rm_rf(s->root);
  env_free(s->env);
  free(s->root);
  free(s->home);
  free(s->outside);
  free(s->bin);
  free(s->log);
  free(s);
}
static Proc run_bin(Sandbox *s, char *const *args, const char *cwd, char **env) {
  size_t n = 0;
  while (args[n])
    n++;
  char **argv = calloc(n + 2, sizeof *argv);
  argv[0] = binary;
  for (size_t i = 0; i < n; i++)
    argv[i + 1] = args[i];
  Proc p = proc_run(binary, argv, cwd, env ? env : s->env, 5000);
  free(argv);
  return p;
}
static char *trim_copy(const char *text, size_t n) {
  while (n && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ' || text[n - 1] == '\t'))
    n--;
  size_t i = 0;
  while (i < n && (text[i] == '\n' || text[i] == '\r' || text[i] == ' ' || text[i] == '\t'))
    i++;
  char *s = malloc(n - i + 1);
  memcpy(s, text + i, n - i);
  s[n - i] = 0;
  return s;
}
static Proc run_fix(Sandbox *s, char *const *args) {
  size_t n = 0;
  while (args[n])
    n++;
  char **argv = calloc(n + 2, sizeof *argv);
  argv[0] = fixture;
  for (size_t i = 0; i < n; i++)
    argv[i + 1] = args[i];
  Proc p = proc_run(fixture, argv, NULL, s->env, 5000);
  free(argv);
  return p;
}
static char *repo(Sandbox *s, const char *relative, const char *marker) {
  char *p = path_fmt(s->home, relative);
  mkdir_p(p);
  if (!marker || !strcmp(marker, "dir")) {
    char *git = path_fmt(p, ".git");
    mkdir(git, 0700);
    free(git);
  } else if (!strcmp(marker, "file")) {
    char *git = path_fmt(p, ".git");
    write_file(git, "gitdir: /outside/private\n", 25, 0644);
    free(git);
  } else if (!strcmp(marker, "symlink")) {
    char *git = path_fmt(p, ".git");
    char *target = path_fmt(s->outside, ".git");
    make_link(target, git);
    free(target);
    free(git);
  }
  return p;
}
static yyjson_doc *list_doc(Sandbox *s, char *const *argv, const char *cwd, char **env, bool *ok) {
  Proc p = run_bin(s, argv, cwd, env);
  if (p.error || p.status != 0) {
    FAIL("list status %d %s", p.status, p.err ? p.err : "");
    proc_clear(&p);
    if (ok)
      *ok = false;
    return NULL;
  }
  if (legacy) {
    size_t n = 0;
    while (argv[n])
      n++;
    char **py = calloc(n + 3, sizeof *py);
    py[0] = python;
    py[1] = legacy;
    for (size_t i = 0; i < n; i++)
      py[i + 2] = argv[i];
    Proc old = proc_run(python, py, cwd, env ? env : s->env, 5000);
    free(py);
    if (old.error || old.status != 0 || old.out_len != p.out_len ||
        memcmp(old.out ? old.out : "", p.out ? p.out : "", p.out_len) != 0)
      FAIL("legacy list mismatch");
    proc_clear(&old);
  }
  yyjson_doc *doc = yyjson_read(p.out ? p.out : "", p.out_len, 0);
  if (!doc)
    FAIL("list json");
  proc_clear(&p);
  if (ok)
    *ok = doc != NULL;
  return doc;
}
static yyjson_doc *lines_doc(const char *log, size_t count) {
  for (int i = 0; i < 100; i++) {
    size_t n = 0;
    char *text = read_file(log, &n);
    size_t rows = 0;
    for (size_t at = 0; text && at < n; at++)
      if (text[at] == '\n')
        rows++;
    if (text && n && text[n - 1] != '\n')
      rows++;
    if (rows >= count) {
      yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
      yyjson_mut_val *arr = yyjson_mut_arr(doc);
      yyjson_mut_doc_set_root(doc, arr);
      size_t i0 = 0;
      while (i0 < n) {
        size_t end = i0;
        while (end < n && text[end] != '\n')
          end++;
        if (end > i0) {
          yyjson_doc *line = yyjson_read(text + i0, end - i0, 0);
          if (line) {
            yyjson_mut_arr_add_val(arr, yyjson_val_mut_copy(doc, yyjson_doc_get_root(line)));
            yyjson_doc_free(line);
          }
        }
        i0 = end + 1;
      }
      free(text);
      size_t wn = 0;
      char *out = yyjson_mut_write(doc, 0, &wn);
      yyjson_mut_doc_free(doc);
      yyjson_doc *parsed = out ? yyjson_read(out, wn, 0) : NULL;
      free(out);
      return parsed;
    }
    free(text);
    usleep(20000);
  }
  FAIL("detached command did not log");
  return NULL;
}
static void begin(const char *name) {
  current_test = name;
  printf("RUN %s\n", name);
  fflush(stdout);
}
static const char *actions[] = {"terminal", "editor", "files"};

static void test_shape(void) {
  begin("stable catalogue shape, byte order, dotfiles and opaque SHA IDs");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *dot = path_fmt(s->home, ".dotfiles");
  mkdir(dot, 0700);
  free(dot);
  free(repo(s, "Repositories/zeta", "dir"));
  free(repo(s, "Repositories/org/alpha", "dir"));
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  const char *expected[] = {".dotfiles", "Repositories/org/alpha", "Repositories/zeta"};
  yyjson_val *arr = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(arr) != 3)
    FAIL("count");
  for (size_t i = 0; i < 3 && i < yyjson_arr_size(arr); i++) {
    yyjson_val *e = yyjson_arr_get(arr, i);
    if (yyjson_obj_size(e) != 5 || !yyjson_obj_get(e, "Text") || !yyjson_obj_get(e, "Subtext") ||
        !yyjson_obj_get(e, "Value") || !yyjson_obj_get(e, "Icon") || !yyjson_obj_get(e, "Keywords"))
      FAIL("keys");
    if (!str_eq(yyjson_obj_get(e, "Subtext"), expected[i]) || !str_eq(yyjson_obj_get(e, "Icon"), "folder"))
      FAIL("subtext %zu", i);
    char *full = path_fmt(s->home, expected[i]);
    char *id = hash_str(full);
    if (!str_eq(yyjson_obj_get(e, "Value"), id) || yyjson_get_len(yyjson_obj_get(e, "Value")) != 64)
      FAIL("id %zu", i);
    free(id);
    free(full);
    const char *text = expected[i];
    const char *slash = strrchr(text, '/');
    if (!str_eq(yyjson_obj_get(e, "Text"), slash ? slash + 1 : text))
      FAIL("text %zu", i);
  }
  yyjson_val *k0 = yyjson_obj_get(yyjson_arr_get(arr, 0), "Keywords");
  yyjson_val *k1 = yyjson_obj_get(yyjson_arr_get(arr, 1), "Keywords");
  if (yyjson_arr_size(k0) != 2 || !str_eq(yyjson_arr_get(k0, 0), ".dotfiles") || !str_eq(yyjson_arr_get(k0, 1), "dotfiles") ||
      yyjson_arr_size(k1) != 3 || !str_eq(yyjson_arr_get(k1, 0), "Repositories") || !str_eq(yyjson_arr_get(k1, 1), "org") ||
      !str_eq(yyjson_arr_get(k1, 2), "alpha"))
    FAIL("keywords");
  yyjson_doc *again = list_doc(s, argv, NULL, NULL, &ok);
  if (!ok || !json_eq(yyjson_doc_get_root(doc), yyjson_doc_get_root(again)))
    FAIL("stable");
  yyjson_doc_free(again);
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_worktree(void) {
  begin("worktree marker and environment files are neither read nor executed");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *p = repo(s, "Repositories/app", "file");
  char *git = path_fmt(p, ".git");
  chmod(git, 0);
  free(git);
  char *envrc = path_fmt(p, ".envrc");
  char *body = malloc(strlen(s->root) + 32);
  memcpy(body, "printf pwned > ", 15);
  memcpy(body + 15, s->root, strlen(s->root));
  memcpy(body + 15 + strlen(s->root), "/pwned", 7);
  write_file(envrc, body, strlen(body), 0755);
  free(body);
  free(envrc);
  free(p);
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  yyjson_val *arr = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(arr) != 1 || !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 0), "Subtext"), "Repositories/app"))
    FAIL("listed");
  size_t n = 0;
  char *log = read_file(s->log, &n);
  if (!log || n != 0)
    FAIL("log");
  free(log);
  char *pwned = path_fmt(s->root, "pwned");
  if (access(pwned, F_OK) == 0)
    FAIL("executed envrc");
  free(pwned);
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_hidden(void) {
  begin("all hidden/dependency names and symlink escapes are skipped");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  const char *names[] = {".hidden", "node_modules", "bower_components", "vendor", "venv", "site-packages",
                         "__pycache__", "third_party", "third-party", "deps", "Pods", "Carthage", "elm-stuff",
                         "result", "target", "dist", "build"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    char rel[128];
    snprintf(rel, sizeof rel, "Repositories/%s/repo", names[i]);
    free(repo(s, rel, "dir"));
  }
  free(repo(s, "Repositories/marker-link", "symlink"));
  char *escape = path_fmt(s->home, "Repositories/escape");
  mkdir_p(path_fmt(s->home, "Repositories"));
  make_link(s->outside, escape);
  free(escape);
  char *loop = path_fmt(s->home, "Repositories/loop");
  char *repos = path_fmt(s->home, "Repositories");
  make_link(repos, loop);
  free(repos);
  free(loop);
  char *group = path_fmt(s->home, "Repositories/group");
  mkdir(group, 0700);
  char *hop = path_fmt(group, "hop");
  make_link(s->outside, hop);
  free(hop);
  free(group);
  free(repo(s, "Repositories/visible", "dir"));
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  yyjson_val *arr = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(arr) != 1 || !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 0), "Subtext"), "Repositories/visible"))
    FAIL("visible only");
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_roots(void) {
  begin("symlinked catalogue roots are not followed");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *dotlink = path_fmt(s->home, ".dotfiles");
  char *repolink = path_fmt(s->home, "Repositories");
  make_link(s->outside, dotlink);
  make_link(s->outside, repolink);
  free(dotlink);
  free(repolink);
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc && yyjson_arr_size(yyjson_doc_get_root(doc)) == 0);
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_markers(void) {
  begin("directory or regular file git markers only; no FIFOs or dangling links");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/file", "file"));
  free(repo(s, "Repositories/directory", "dir"));
  char *broken = repo(s, "Repositories/broken", "none");
  char *bgit = path_fmt(broken, ".git");
  char *missing = path_fmt(s->root, "missing");
  make_link(missing, bgit);
  free(missing);
  free(bgit);
  free(broken);
  char *pipe = repo(s, "Repositories/pipe", "none");
  char *pgit = path_fmt(pipe, ".git");
  char *mkfifo_bin = find_on_path("mkfifo");
  CHECK(mkfifo_bin);
  char *argv_m[] = {mkfifo_bin, pgit, NULL};
  Proc made = proc_run(mkfifo_bin, argv_m, NULL, NULL, 5000);
  if (made.error || made.status != 0)
    FAIL("mkfifo");
  proc_clear(&made);
  free(mkfifo_bin);
  free(pgit);
  free(pipe);
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  yyjson_val *arr = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(arr) != 2 || !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 0), "Text"), "directory") ||
      !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 1), "Text"), "file"))
    FAIL("markers");
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_depth(void) {
  begin("depth three and existing repository boundary");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/a/b/c", "dir"));
  free(repo(s, "Repositories/x/y/z/deep", "dir"));
  free(repo(s, "Repositories/parent", "dir"));
  free(repo(s, "Repositories/parent/child", "dir"));
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  yyjson_val *arr = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(arr) != 2 || !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 0), "Subtext"), "Repositories/a/b/c") ||
      !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 1), "Subtext"), "Repositories/parent"))
    FAIL("depth");
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_cap(void) {
  begin("200 result cap includes dotfiles and selects sorted candidates");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  mkdir(path_fmt(s->home, ".dotfiles"), 0700);
  for (int n = 200; n >= 0; n--) {
    char rel[64];
    snprintf(rel, sizeof rel, "Repositories/p%03d", n);
    free(repo(s, rel, "dir"));
  }
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  yyjson_val *arr = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(arr) != 200 || !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 0), "Subtext"), ".dotfiles") ||
      !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 199), "Subtext"), "Repositories/p198"))
    FAIL("cap");
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static yyjson_doc *discover(Sandbox *s, const char *depth, const char *entries, const char *dirents) {
  char *args[] = {"discover", s->home, (char *)depth, (char *)entries, (char *)dirents, NULL};
  Proc p = run_fix(s, args);
  if (p.error || p.status != 0) {
    FAIL("discover %d %s", p.status, p.err ? p.err : "");
    proc_clear(&p);
    return NULL;
  }
  yyjson_doc *doc = yyjson_read(p.out ? p.out : "", p.out_len, 0);
  proc_clear(&p);
  return doc;
}
static void test_stop(void) {
  begin("result cap stops recursive traversal, including zero-result limits");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/a/inner", "dir"));
  free(repo(s, "Repositories/b/inner", "dir"));
  free(repo(s, "Repositories/c/inner", "dir"));
  yyjson_doc *doc = discover(s, "3", "1", "4000");
  CHECK(doc);
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *entries = yyjson_obj_get(root, "entries");
  if (yyjson_get_uint(yyjson_obj_get(root, "scans")) != 2 || yyjson_arr_size(entries) != 1 ||
      !str_eq(yyjson_obj_get(yyjson_arr_get(entries, 0), "Subtext"), "Repositories/a/inner"))
    FAIL("stop");
  yyjson_doc_free(doc);
  doc = discover(s, "3", "0", "4000");
  CHECK(doc);
  root = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(yyjson_obj_get(root, "entries")) != 0 || yyjson_get_uint(yyjson_obj_get(root, "used")) != 0 ||
      yyjson_get_uint(yyjson_obj_get(root, "scans")) != 0)
    FAIL("zero");
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_budget(void) {
  begin("global dirent budget counts skipped names/files but excludes dot entries");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *repos = path_fmt(s->home, "Repositories");
  mkdir(repos, 0700);
  for (int i = 0; i < 12; i++) {
    char name[32];
    snprintf(name, sizeof name, ".hidden-%d", i);
    char *file = path_fmt(repos, name);
    write_file(file, "", 0, 0644);
    free(file);
  }
  free(repos);
  free(repo(s, "Repositories/project", "dir"));
  yyjson_doc *doc = discover(s, "3", "200", "3");
  CHECK(doc);
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (yyjson_get_uint(yyjson_obj_get(root, "used")) != 3 || yyjson_get_uint(yyjson_obj_get(root, "scans")) != 1 ||
      yyjson_arr_size(yyjson_obj_get(root, "entries")) > 1)
    FAIL("budget");
  yyjson_doc_free(doc);
  doc = discover(s, "3", "200", "0");
  CHECK(doc);
  root = yyjson_doc_get_root(doc);
  if (yyjson_get_uint(yyjson_obj_get(root, "used")) != 0 || yyjson_get_uint(yyjson_obj_get(root, "scans")) != 0 ||
      yyjson_arr_size(yyjson_obj_get(root, "entries")) != 0)
    FAIL("zero budget");
  yyjson_doc_free(doc);
  sandbox_free(s);
  Sandbox *plain = sandbox_new();
  CHECK(plain && plain->root);
  free(repo(plain, "Repositories/p00", "dir"));
  free(repo(plain, "Repositories/p01", "dir"));
  doc = discover(plain, "3", "200", "2");
  CHECK(doc);
  root = yyjson_doc_get_root(doc);
  if (yyjson_get_uint(yyjson_obj_get(root, "used")) != 2 || yyjson_arr_size(yyjson_obj_get(root, "entries")) != 2)
    FAIL("exact");
  yyjson_doc_free(doc);
  sandbox_free(plain);
}
static void test_readdir(void) {
  begin("directory iteration failures discard partial catalogues and prevent launches");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/app", "dir"));
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  const char *id = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"));
  char *idcopy = strdup(id);
  yyjson_doc_free(doc);
  char *fail_env = env_pair("LP_READDIR_FAIL_AFTER", "3");
  const char *extra[] = {fail_env, NULL};
  char **env = env_with(extra);
  /* env_with does not include sandbox overrides if they are only in s->env. Rebuild from s->env. */
  env_free(env);
  size_t nenv = 0;
  while (s->env[nenv])
    nenv++;
  env = calloc(nenv + 2, sizeof *env);
  for (size_t i = 0; i < nenv; i++)
    env[i] = strdup(s->env[i]);
  env[nenv] = strdup(fail_env);
  char *list_args[] = {fixture, "cli", "list", NULL};
  Proc p = proc_run(fixture, list_args, NULL, env, 5000);
  if (p.error || p.status != 1 || p.out_len != 0 || !p.err || strcmp(p.err, "unable to list\n") != 0)
    FAIL("list fail %d %s", p.status, p.err ? p.err : "");
  proc_clear(&p);
  char *open_args[] = {fixture, "cli", "open", idcopy, "files", NULL};
  p = proc_run(fixture, open_args, NULL, env, 5000);
  if (p.error || p.status != 1 || p.out_len != 0 || !p.err || strcmp(p.err, "unknown project\n") != 0)
    FAIL("open fail %d %s", p.status, p.err ? p.err : "");
  proc_clear(&p);
  char *bin_args[] = {binary, "list", NULL};
  p = proc_run(binary, bin_args, NULL, env, 5000);
  if (p.error || p.status != 0)
    FAIL("production list");
  proc_clear(&p);
  size_t n = 0;
  char *log = read_file(s->log, &n);
  if (!log || n != 0)
    FAIL("log");
  free(log);
  env_free(env);
  free(fail_env);
  free(idcopy);
  sandbox_free(s);
}
static void test_production_limit(void) {
  begin("production directory-entry limit bounds large catalogues");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *repos = path_fmt(s->home, "Repositories");
  mkdir(repos, 0700);
  for (int i = 0; i < 4001; i++) {
    char name[32];
    snprintf(name, sizeof name, ".ignored-%d", i);
    char *file = path_fmt(repos, name);
    write_file(file, "", 0, 0644);
    free(file);
  }
  free(repos);
  free(repo(s, "Repositories/project", "dir"));
  yyjson_doc *doc = discover(s, "3", "200", "4000");
  CHECK(doc);
  if (yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "used")) != 4000 ||
      yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "scans")) != 1)
    FAIL("used");
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *listed = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && listed);
  if (!json_eq(yyjson_doc_get_root(listed), yyjson_obj_get(yyjson_doc_get_root(doc), "entries")))
    FAIL("list matches discover");
  yyjson_doc_free(listed);
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_exhausted(void) {
  begin("global exhausted budget still emits already-read direct repos, not groups");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/a/inner", "dir"));
  free(repo(s, "Repositories/b", "dir"));
  yyjson_doc *doc = discover(s, "3", "200", "2");
  CHECK(doc);
  yyjson_val *entries = yyjson_obj_get(yyjson_doc_get_root(doc), "entries");
  if (yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "used")) != 2 ||
      yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "scans")) != 1 || yyjson_arr_size(entries) != 1 ||
      !str_eq(yyjson_obj_get(yyjson_arr_get(entries, 0), "Subtext"), "Repositories/b"))
    FAIL("exhausted");
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_missing(void) {
  begin("missing, unreadable and non-directory roots give empty catalogues");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc && yyjson_arr_size(yyjson_doc_get_root(doc)) == 0);
  yyjson_doc_free(doc);
  write_file(path_fmt(s->home, "Repositories"), "not a directory", 15, 0644);
  doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc && yyjson_arr_size(yyjson_doc_get_root(doc)) == 0);
  yyjson_doc_free(doc);
  unlink(path_fmt(s->home, "Repositories"));
  free(repo(s, "Repositories/no-access/child", "dir"));
  char *locked = path_fmt(s->home, "Repositories/no-access");
  chmod(locked, 0);
  doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc && yyjson_arr_size(yyjson_doc_get_root(doc)) == 0);
  chmod(locked, 0700);
  free(locked);
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_home(void) {
  begin("relative/home overrides use lexical absolute paths without symlink resolution");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/app", "dir"));
  char *other = path_fmt(s->root, "other");
  mkdir(other, 0700);
  mkdir(path_fmt(other, ".dotfiles"), 0700);
  char *args1[] = {"--home", other, "list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, args1, NULL, NULL, &ok);
  CHECK(ok && doc && str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Subtext"), ".dotfiles"));
  yyjson_doc_free(doc);
  char *eq = malloc(strlen(other) + 8);
  memcpy(eq, "--home=", 7);
  memcpy(eq + 7, other, strlen(other) + 1);
  char *args2[] = {eq, "list", NULL};
  doc = list_doc(s, args2, s->root, NULL, &ok);
  CHECK(ok && doc && str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Subtext"), ".dotfiles"));
  yyjson_doc_free(doc);
  free(eq);
  char *args3[] = {"--home", "other/../home/.", "list", NULL};
  doc = list_doc(s, args3, s->root, NULL, &ok);
  CHECK(ok && doc);
  char *app = path_fmt(s->home, "Repositories/app");
  char *id = hash_str(app);
  if (!str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"), id))
    FAIL("lexical home");
  free(id);
  yyjson_doc_free(doc);
  char *args4[] = {"--home", "", "list", NULL};
  doc = list_doc(s, args4, NULL, NULL, &ok);
  CHECK(ok && doc && str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Text"), "app"));
  yyjson_doc_free(doc);
  size_t nenv = 0;
  while (s->env[nenv])
    nenv++;
  char **env = calloc(nenv + 1, sizeof *env);
  size_t k = 0;
  for (size_t i = 0; i < nenv; i++) {
    if (!strncmp(s->env[i], "LAUNCHER_HOME=", 14))
      env[k++] = strdup("LAUNCHER_HOME=");
    else
      env[k++] = strdup(s->env[i]);
  }
  char *args5[] = {"list", NULL};
  doc = list_doc(s, args5, NULL, env, &ok);
  CHECK(ok && doc && str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Text"), "app"));
  yyjson_doc_free(doc);
  env_free(env);
  char *alias = path_fmt(s->root, "alias");
  make_link(s->home, alias);
  char *args6[] = {"--home", alias, "list", NULL};
  doc = list_doc(s, args6, NULL, NULL, &ok);
  CHECK(ok && doc);
  char *alias_app = path_fmt(alias, "Repositories/app");
  char *alias_id = hash_str(alias_app);
  char *real_id = hash_str(app);
  if (!str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"), alias_id) ||
      strcmp(alias_id, real_id) == 0)
    FAIL("alias id");
  free(alias_id);
  free(real_id);
  free(alias_app);
  free(app);
  free(alias);
  free(other);
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_slashes(void) {
  begin("two leading slashes and normalized project IDs preserve Path semantics");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/app", "dir"));
  char *one = malloc(strlen(s->home) + 2);
  one[0] = '/';
  memcpy(one + 1, s->home, strlen(s->home) + 1);
  char *args1[] = {"--home", one, "list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, args1, NULL, NULL, &ok);
  CHECK(ok && doc);
  char *joined = path_fmt(s->home, "Repositories/app");
  char *double_path = malloc(strlen(joined) + 2);
  double_path[0] = '/';
  memcpy(double_path + 1, joined, strlen(joined) + 1);
  char *id = hash_str(double_path);
  if (!str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"), id))
    FAIL("two slashes");
  free(id);
  yyjson_doc_free(doc);
  char *three = malloc(strlen(s->home) + 3);
  memcpy(three, "//", 2);
  memcpy(three + 2, s->home, strlen(s->home) + 1);
  char *args2[] = {"--home", three, "list", NULL};
  doc = list_doc(s, args2, NULL, NULL, &ok);
  CHECK(ok && doc);
  id = hash_str(joined);
  if (!str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"), id))
    FAIL("three slashes");
  free(id);
  free(joined);
  free(double_path);
  free(one);
  free(three);
  yyjson_doc_free(doc);
  const char *raws[][2] = {{"a//./b/", "a/b"}, {"a/../b", "a/../b"}, {"", "."}, {"//a///b/", "//a/b"}, {"///a", "/a"}};
  for (size_t i = 0; i < 5; i++) {
    char *args[] = {"id", (char *)raws[i][0], NULL};
    Proc p = run_fix(s, args);
    if (p.error || p.status != 0)
      FAIL("id status");
    char *got = trim_copy(p.out ? p.out : "", p.out_len);
    char *expect = hash_str(raws[i][1]);
    if (strcmp(got, expect) != 0)
      FAIL("id %s", raws[i][0]);
    free(got);
    free(expect);
    proc_clear(&p);
  }
  char *args[] = {"absolute", "../home/./", NULL};
  Proc p = run_fix(s, args);
  if (p.error || p.status != 0)
    FAIL("absolute");
  char cwd[4096];
  CHECK(getcwd(cwd, sizeof cwd));
  char *parent = strdup(cwd);
  char *slash = strrchr(parent, '/');
  if (slash && slash != parent)
    *slash = 0;
  else if (slash)
    slash[1] = 0;
  char *resolved = (*parent == '/' && parent[1] == 0) ? strdup("/home") : path_fmt(parent, "home");
  char *got = trim_copy(p.out ? p.out : "", p.out_len);
  if (strcmp(got, resolved) != 0)
    FAIL("absolute %s != %s", got, resolved);
  free(got);
  free(resolved);
  free(parent);
  proc_clear(&p);
  sandbox_free(s);
}
static int cmp_mem(const void *a, const void *b) {
  const char *x = *(char *const *)a, *y = *(char *const *)b;
  size_t nx = strlen(x), ny = strlen(y);
  int c = memcmp(x, y, nx < ny ? nx : ny);
  if (c)
    return c;
  return nx < ny ? -1 : nx > ny ? 1 : 0;
}
static void test_unicode(void) {
  begin("Unicode/control text remains JSON data, byte sorting differs from locale");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  const char *names[] = {"Ω", "é", "Z", "a", "tab\tline\n\"quote\\:,"};
  for (size_t i = 0; i < 5; i++) {
    char *rel = path_fmt("Repositories", names[i]);
    free(repo(s, rel, "dir"));
    free(rel);
  }
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  char *sorted[5];
  for (int i = 0; i < 5; i++)
    sorted[i] = (char *)names[i];
  qsort(sorted, 5, sizeof sorted[0], cmp_mem);
  yyjson_val *arr = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(arr) != 5)
    FAIL("count");
  for (size_t i = 0; i < 5 && i < yyjson_arr_size(arr); i++) {
    if (!str_eq(yyjson_obj_get(yyjson_arr_get(arr, i), "Text"), sorted[i]))
      FAIL("order %zu", i);
    char *rel = path_fmt("Repositories", sorted[i]);
    char *full = path_fmt(s->home, rel);
    char *id = hash_str(full);
    if (!str_eq(yyjson_obj_get(yyjson_arr_get(arr, i), "Value"), id))
      FAIL("hash %zu", i);
    free(id);
    free(full);
    free(rel);
  }
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_invalid(void) {
  begin("invalid filesystem UTF-8 uses question marks while hashing original bytes");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *repos = path_fmt(s->home, "Repositories");
  mkdir(repos, 0700);
  const unsigned char names[][8] = {{0x61, 0xff}, {0x61, 0xfe}, {0x62, 0xe2, 0x82}, {0x63, 0xed, 0xa0, 0x80}, {0x64, 0xf0, 0x9f, 0x98, 0x80}};
  const size_t lens[] = {2, 2, 3, 4, 5};
  for (size_t i = 0; i < 5; i++) {
    char *dir = malloc(strlen(repos) + 1 + lens[i] + 1);
    memcpy(dir, repos, strlen(repos));
    dir[strlen(repos)] = '/';
    memcpy(dir + strlen(repos) + 1, names[i], lens[i]);
    dir[strlen(repos) + 1 + lens[i]] = 0;
    mkdir(dir, 0700);
    char *git = malloc(strlen(dir) + 6);
    memcpy(git, dir, strlen(dir));
    memcpy(git + strlen(dir), "/.git", 6);
    mkdir(git, 0700);
    free(git);
    free(dir);
  }
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  const char *expect[] = {"a?", "a?", "b??", "c???", "d😀"};
  unsigned order[5] = {0, 1, 2, 3, 4};
  for (int i = 0; i < 5; i++)
    for (int j = i + 1; j < 5; j++) {
      size_t n = lens[order[i]] < lens[order[j]] ? lens[order[i]] : lens[order[j]];
      int c = memcmp(names[order[i]], names[order[j]], n);
      if (c > 0 || (c == 0 && lens[order[i]] > lens[order[j]])) {
        unsigned t = order[i];
        order[i] = order[j];
        order[j] = t;
      }
    }
  yyjson_val *arr = yyjson_doc_get_root(doc);
  if (yyjson_arr_size(arr) != 5)
    FAIL("count");
  for (size_t i = 0; i < 5 && i < yyjson_arr_size(arr); i++) {
    if (!str_eq(yyjson_obj_get(yyjson_arr_get(arr, i), "Text"), expect[i]))
      FAIL("text %zu", i);
    size_t base = strlen(repos) + 1;
    char *full = malloc(base + lens[order[i]] + 1);
    memcpy(full, repos, strlen(repos));
    full[strlen(repos)] = '/';
    memcpy(full + base, names[order[i]], lens[order[i]]);
    char *id = hash_bytes(full, base + lens[order[i]]);
    if (!str_eq(yyjson_obj_get(yyjson_arr_get(arr, i), "Value"), id))
      FAIL("hash %zu", i);
    free(id);
    free(full);
  }
  yyjson_doc_free(doc);
  free(repos);
  sandbox_free(s);
}
static void test_keywords(void) {
  begin("keyword uniqueness uses raw components, not lossy display strings");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *repos = path_fmt(s->home, "Repositories");
  mkdir(repos, 0700);
  unsigned char parent_b[] = {0x61, 0xff}, child_b[] = {0x61, 0xfe};
  char *parent = malloc(strlen(repos) + 4);
  memcpy(parent, repos, strlen(repos));
  parent[strlen(repos)] = '/';
  memcpy(parent + strlen(repos) + 1, parent_b, 2);
  parent[strlen(repos) + 3] = 0;
  mkdir(parent, 0700);
  char *child = malloc(strlen(parent) + 4);
  memcpy(child, parent, strlen(parent));
  child[strlen(parent)] = '/';
  memcpy(child + strlen(parent) + 1, child_b, 2);
  child[strlen(parent) + 3] = 0;
  mkdir(child, 0700);
  char *git = malloc(strlen(child) + 6);
  memcpy(git, child, strlen(child));
  memcpy(git + strlen(child), "/.git", 6);
  mkdir(git, 0700);
  free(git);
  free(child);
  free(parent);
  free(repos);
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  yyjson_val *keys = yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Keywords");
  if (yyjson_arr_size(keys) != 3 || !str_eq(yyjson_arr_get(keys, 0), "Repositories") ||
      !str_eq(yyjson_arr_get(keys, 1), "a?") || !str_eq(yyjson_arr_get(keys, 2), "a?"))
    FAIL("keywords");
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_dotfiles_open(void) {
  begin("dotfiles opens without a git marker");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  mkdir(path_fmt(s->home, ".dotfiles"), 0700);
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  const char *id = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"));
  char *args[] = {binary, "open", (char *)id, "files", NULL};
  Proc p = proc_run(binary, args, NULL, s->env, 5000);
  if (p.error || p.status != 0)
    FAIL("open %s", p.err ? p.err : "");
  proc_clear(&p);
  yyjson_doc_free(doc);
  yyjson_doc *lines = lines_doc(s->log, 1);
  CHECK(lines);
  yyjson_val *argsv = yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(lines), 0), "args");
  char *dot = path_fmt(s->home, ".dotfiles");
  const char *expect[] = {"--user", "--scope", "--collect", "--quiet", "--", "xdg-open", dot};
  if (yyjson_arr_size(argsv) != 7)
    FAIL("argc");
  for (size_t i = 0; i < 7 && i < yyjson_arr_size(argsv); i++)
    if (!str_eq(yyjson_arr_get(argsv, i), expect[i]))
      FAIL("arg %zu", i);
  free(dot);
  yyjson_doc_free(lines);
  sandbox_free(s);
}
static void test_actions(void) {
  begin("all launch actions detach, use argv/cwd root and silence stdio");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  const char *name = "proj; $(touch pwned) 'quote' \"dq\" --flag";
  char *rel = path_fmt("Repositories", name);
  char *path = repo(s, rel, "file");
  free(rel);
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  const char *id = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"));
  char *idcopy = strdup(id);
  yyjson_doc_free(doc);
  for (int i = 0; i < 3; i++) {
    char *args[] = {binary, "open", idcopy, (char *)actions[i], NULL};
    Proc p = proc_run(binary, args, NULL, s->env, 5000);
    if (p.error || p.status != 0 || p.out_len != 0)
      FAIL("open %s", actions[i]);
    proc_clear(&p);
    yyjson_doc *lines = lines_doc(s->log, (size_t)i + 1);
    CHECK(lines);
    yyjson_val *e = yyjson_arr_get(yyjson_doc_get_root(lines), (size_t)i);
    yyjson_val *argsv = yyjson_obj_get(e, "args");
    char *wd = malloc(strlen(path) + 22);
    memcpy(wd, "--working-directory=", 20);
    memcpy(wd + 20, path, strlen(path) + 1);
    if (!str_eq(yyjson_obj_get(e, "cwd"), "/") || yyjson_get_int(yyjson_obj_get(e, "sid")) != yyjson_get_int(yyjson_obj_get(e, "pid")) ||
        yyjson_get_int(yyjson_obj_get(e, "pgrp")) != yyjson_get_int(yyjson_obj_get(e, "pid")) ||
        !str_eq(yyjson_obj_get(e, "marker"), "inherited-marker") || yyjson_get_bool(yyjson_obj_get(e, "inherited")))
      FAIL("process %s", actions[i]);
    yyjson_val *stdio = yyjson_obj_get(e, "stdio");
    if (yyjson_arr_size(stdio) != 3 || !str_eq(yyjson_arr_get(stdio, 0), "/dev/null") ||
        !str_eq(yyjson_arr_get(stdio, 1), "/dev/null") || !str_eq(yyjson_arr_get(stdio, 2), "/dev/null"))
      FAIL("stdio");
    const char *prefix[] = {"--user", "--scope", "--collect", "--quiet", "--"};
    for (size_t k = 0; k < 5; k++)
      if (!str_eq(yyjson_arr_get(argsv, k), prefix[k]))
        FAIL("prefix");
    if (!strcmp(actions[i], "files")) {
      if (yyjson_arr_size(argsv) != 7 || !str_eq(yyjson_arr_get(argsv, 5), "xdg-open") || !str_eq(yyjson_arr_get(argsv, 6), path))
        FAIL("files argv");
    } else if (!strcmp(actions[i], "editor")) {
      if (yyjson_arr_size(argsv) != 10 || !str_eq(yyjson_arr_get(argsv, 5), "ghostty") ||
          !str_eq(yyjson_arr_get(argsv, 6), wd) || !str_eq(yyjson_arr_get(argsv, 7), "-e") ||
          !str_eq(yyjson_arr_get(argsv, 8), "nvim") || !str_eq(yyjson_arr_get(argsv, 9), "."))
        FAIL("editor argv");
    } else if (yyjson_arr_size(argsv) != 7 || !str_eq(yyjson_arr_get(argsv, 5), "ghostty") || !str_eq(yyjson_arr_get(argsv, 6), wd))
      FAIL("terminal argv");
    free(wd);
    yyjson_doc_free(lines);
  }
  char *pwned = path_fmt(s->root, "pwned");
  if (access(pwned, F_OK) == 0)
    FAIL("shell injection");
  free(pwned);
  free(path);
  free(idcopy);
  sandbox_free(s);
}
static void test_fd(void) {
  begin("descriptor inheritance is closed even when opened without CLOEXEC");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/app", "dir"));
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  const char *id = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"));
  char *args[] = {"fd-open", s->home, (char *)id, "terminal", NULL};
  Proc p = run_fix(s, args);
  if (p.error || p.status != 0)
    FAIL("fd-open %s", p.err ? p.err : "");
  proc_clear(&p);
  yyjson_doc_free(doc);
  yyjson_doc *lines = lines_doc(s->log, 1);
  CHECK(lines && !yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(lines), 0), "inherited")));
  yyjson_doc_free(lines);
  sandbox_free(s);
}
static void test_unknown(void) {
  begin("unknown/invalid IDs, invalid actions and command injection never launch");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/app", "dir"));
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  const char *id = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"));
  char *idcopy = strdup(id);
  yyjson_doc_free(doc);
  char upper[65], shortid[64], gee[65], eff[65];
  for (int i = 0; i < 64; i++) {
    char c = idcopy[i];
    upper[i] = (char)(c >= 'a' && c <= 'f' ? c - 32 : c);
    gee[i] = 'g';
    eff[i] = 'f';
  }
  upper[64] = gee[64] = eff[64] = 0;
  memset(shortid, 'a', 63);
  shortid[63] = 0;
  char *injected = malloc(strlen(idcopy) + 16);
  memcpy(injected, idcopy, strlen(idcopy));
  memcpy(injected + strlen(idcopy), ";touch marker", 14);
  char *bads[] = {"a", shortid, gee, upper, eff, s->outside, "../../etc/passwd", injected};
  for (size_t i = 0; i < sizeof bads / sizeof bads[0]; i++) {
    char *args[] = {binary, "open", bads[i], "files", NULL};
    Proc p = proc_run(binary, args, NULL, s->env, 5000);
    if (p.error || p.status != 1 || p.out_len != 0 || !p.err || strcmp(p.err, "unknown project\n") != 0)
      FAIL("bad id %s", bads[i]);
    proc_clear(&p);
  }
  const char *bad_actions[] = {"", "terminal;touch", "files extra"};
  for (size_t i = 0; i < 3; i++) {
    char *args[] = {binary, "open", idcopy, (char *)bad_actions[i], NULL};
    Proc p = proc_run(binary, args, NULL, s->env, 5000);
    if (p.error || p.status != 1 || !p.err || strcmp(p.err, "invalid action\n") != 0)
      FAIL("bad action");
    proc_clear(&p);
  }
  size_t n = 0;
  char *log = read_file(s->log, &n);
  if (!log || n != 0)
    FAIL("launched");
  free(log);
  free(injected);
  free(idcopy);
  sandbox_free(s);
}
static void test_vanished(void) {
  begin("vanished and replaced projects are rejected by fresh discovery");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *path = repo(s, "Repositories/app", "dir");
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  char *id = strdup(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value")));
  yyjson_doc_free(doc);
  char *gone = path_fmt(s->outside, "gone");
  rename(path, gone);
  char *args[] = {binary, "open", id, "files", NULL};
  Proc p = proc_run(binary, args, NULL, s->env, 5000);
  if (!p.err || strcmp(p.err, "unknown project\n") != 0)
    FAIL("vanished");
  proc_clear(&p);
  make_link(gone, path);
  p = proc_run(binary, args, NULL, s->env, 5000);
  if (!p.err || strcmp(p.err, "unknown project\n") != 0)
    FAIL("symlink replacement");
  proc_clear(&p);
  unlink(path);
  mkdir(path, 0700);
  p = proc_run(binary, args, NULL, s->env, 5000);
  if (!p.err || strcmp(p.err, "unknown project\n") != 0)
    FAIL("replaced");
  proc_clear(&p);
  size_t n = 0;
  char *log = read_file(s->log, &n);
  if (!log || n != 0)
    FAIL("launched");
  free(log);
  free(gone);
  free(path);
  free(id);
  sandbox_free(s);
}
static void test_launchable(void) {
  begin("launchable rechecks each component, marker, depth and root boundary");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  char *p = repo(s, "Repositories/a/b/c", "dir");
  char *args[] = {"launchable", s->home, p, NULL};
  Proc r = run_fix(s, args);
  char *got = trim_copy(r.out ? r.out : "", r.out_len);
  if (r.error || r.status != 0 || strcmp(got, "true") != 0)
    FAIL("good");
  free(got);
  proc_clear(&r);
  char *repos = path_fmt(s->home, "Repositories");
  char *extra = path_fmt(s->home, "Repositories-extra/a");
  char *dotdot = malloc(strlen(s->home) + 32);
  memcpy(dotdot, s->home, strlen(s->home));
  memcpy(dotdot + strlen(s->home), "/Repositories/a/../a/b/c", 25);
  char *bads[] = {path_fmt(s->home, "Repositories/a"), strdup(s->home), strdup(s->outside), strdup(repos), extra, dotdot};
  for (size_t i = 0; i < 6; i++) {
    char *a[] = {"launchable", s->home, bads[i], NULL};
    Proc pbad = run_fix(s, a);
    char *text = trim_copy(pbad.out ? pbad.out : "", pbad.out_len);
    if (pbad.error || pbad.status != 0 || strcmp(text, "false") != 0)
      FAIL("bad %zu", i);
    free(text);
    proc_clear(&pbad);
    free(bads[i]);
  }
  free(repo(s, "Repositories/a/b/c/d", "dir"));
  char *deep = path_fmt(p, "d");
  char *a[] = {"launchable", s->home, deep, NULL};
  Proc pd = run_fix(s, a);
  got = trim_copy(pd.out ? pd.out : "", pd.out_len);
  if (strcmp(got, "false") != 0)
    FAIL("deep");
  free(got);
  proc_clear(&pd);
  free(deep);
  free(repo(s, "Repositories/.hidden", "dir"));
  char *hidden = path_fmt(s->home, "Repositories/.hidden");
  char *ah[] = {"launchable", s->home, hidden, NULL};
  Proc ph = run_fix(s, ah);
  got = trim_copy(ph.out ? ph.out : "", ph.out_len);
  if (strcmp(got, "false") != 0)
    FAIL("hidden");
  free(got);
  proc_clear(&ph);
  free(hidden);
  char *moved = path_fmt(s->outside, "a");
  rename(path_fmt(s->home, "Repositories/a"), moved);
  char *alink = path_fmt(s->home, "Repositories/a");
  make_link(moved, alink);
  free(alink);
  char *ag[] = {"launchable", s->home, p, NULL};
  Proc pg = run_fix(s, ag);
  got = trim_copy(pg.out ? pg.out : "", pg.out_len);
  if (strcmp(got, "false") != 0)
    FAIL("replaced root");
  free(got);
  proc_clear(&pg);
  free(moved);
  free(repos);
  free(p);
  sandbox_free(s);
}
static void test_spawn(void) {
  begin("spawn failure stays path-free");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/app", "dir"));
  unlink(path_fmt(s->bin, "systemd-run"));
  char *argv[] = {"list", NULL};
  bool ok = false;
  yyjson_doc *doc = list_doc(s, argv, NULL, NULL, &ok);
  CHECK(ok && doc);
  const char *id = yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "Value"));
  char *args[] = {binary, "open", (char *)id, "terminal", NULL};
  Proc p = proc_run(binary, args, NULL, s->env, 5000);
  if (p.error || p.status != 1 || p.out_len != 0 || !p.err || strcmp(p.err, "unable to launch\n") != 0)
    FAIL("spawn %d %s", p.status, p.err ? p.err : "");
  proc_clear(&p);
  yyjson_doc_free(doc);
  sandbox_free(s);
}
static void test_cli(void) {
  begin("CLI argument/help statuses and override abbreviations");
  Sandbox *s = sandbox_new();
  CHECK(s && s->root);
  free(repo(s, "Repositories/app", "dir"));
  char *ho = malloc(strlen(s->home) + 6);
  memcpy(ho, "--ho=", 5);
  memcpy(ho + 5, s->home, strlen(s->home) + 1);
  const char *fixed[][5] = {{NULL}, {"--help"}, {"list", "--help"}, {"open", "--help"}, {"unknown", "--help"},
                            {"list", "extra"}, {"list", "--home", s->home}, {"--home"}, {"--home", "--bad", "list"},
                            {"--bad", "list"}, {"open", "x"}, {"open", "a", "b", "c"}, {"--", "list"}, {"list", "--"},
                            {"open", "--", "a", "b"}, {"open", "-1", "terminal"}, {"open", "a", "--bad"}, {ho, "list"},
                            {"--hom", s->home, "list"}, {"--h", "list"}};
  const int codes[] = {2, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 2, 1, 1, 2, 0, 0, 2};
  for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
    char *args[6] = {binary, NULL, NULL, NULL, NULL, NULL};
    size_t n = 0;
    if (i == 0) {
      args[0] = binary;
    } else {
      for (size_t k = 0; fixed[i][k]; k++)
        args[++n] = (char *)fixed[i][k];
    }
    Proc p = proc_run(binary, args, NULL, s->env, 5000);
    if (p.error || p.status != codes[i])
      FAIL("status %zu got %d want %d %s", i, p.status, codes[i], p.err ? p.err : "");
    if (legacy && python) {
      char *py[8] = {python, legacy, NULL, NULL, NULL, NULL, NULL, NULL};
      size_t pn = 2;
      for (size_t k = 1; args[k]; k++)
        py[pn++] = args[k];
      Proc old = proc_run(python, py, NULL, s->env, 5000);
      if (old.status != p.status)
        FAIL("legacy status %zu", i);
      proc_clear(&old);
    }
    proc_clear(&p);
  }
  free(ho);
  sandbox_free(s);
}

int main(void) {
  binary = sibling("LAUNCHER_PROJECTS_BIN", "launcher-projects");
  fixture = sibling("LAUNCHER_PROJECTS_FIXTURE", "projects-fixture");
  fake_bin = sibling(NULL, "projects-fake");
  const char *leg = getenv("LAUNCHER_PROJECTS_LEGACY");
  legacy = leg && *leg ? strdup(leg) : NULL;
  if (legacy) {
    const char *py = getenv("LAUNCHER_PROJECTS_PYTHON");
    python = py && *py ? strdup(py) : find_on_path("python3");
    if (!python) {
      fprintf(stderr, "LAUNCHER_PROJECTS_PYTHON required with LAUNCHER_PROJECTS_LEGACY\n");
      return 1;
    }
  }
  if (!binary || !fixture || !fake_bin || access(binary, X_OK) || access(fixture, X_OK) || access(fake_bin, X_OK)) {
    fprintf(stderr, "Set LAUNCHER_PROJECTS_BIN and LAUNCHER_PROJECTS_FIXTURE\n");
    return 1;
  }
  test_shape();
  test_worktree();
  test_hidden();
  test_roots();
  test_markers();
  test_depth();
  test_cap();
  test_stop();
  test_budget();
  test_readdir();
  test_production_limit();
  test_exhausted();
  test_missing();
  test_home();
  test_slashes();
  test_unicode();
  test_invalid();
  test_keywords();
  test_dotfiles_open();
  test_actions();
  test_fd();
  test_unknown();
  test_vanished();
  test_launchable();
  test_spawn();
  test_cli();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("ok");
  return 0;
}
