#define _GNU_SOURCE
#include <errno.h>
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
static char *binary, *fixture, *fake_bin;

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
  size_t dir = (size_t)(slash - exe);
  size_t nl = strlen(name);
  char *path = malloc(dir + 1 + nl + 1);
  if (!path)
    return NULL;
  memcpy(path, exe, dir);
  path[dir] = '/';
  memcpy(path + dir + 1, name, nl + 1);
  return path;
}
static bool grow(char **buf, size_t *len, size_t *cap, const void *add, size_t n) {
  if (*len + n + 1 < *len)
    return false;
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1) {
      if (next > (SIZE_MAX / 2))
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
  if (!out)
    return NULL;
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
static Proc proc_run(const char *file, char *const *argv, const char *cwd,
                     const void *input, size_t input_len, char **envp, int timeout_ms) {
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
  close(outp[1]);
  close(errp[1]);
  fcntl(inp[1], F_SETFL, O_NONBLOCK);
  fcntl(outp[0], F_SETFL, O_NONBLOCK);
  fcntl(errp[0], F_SETFL, O_NONBLOCK);
  size_t in_off = 0, ocap = 0, ecap = 0;
  int64_t deadline = now_ms() + timeout_ms;
  while (outp[0] >= 0 || errp[0] >= 0 || inp[1] >= 0) {
    if (now_ms() > deadline) {
      kill(-pid, SIGKILL);
      p.error = true;
      break;
    }
    struct pollfd fds[3];
    nfds_t nfd = 0;
    int ii = -1, oi = -1, ei = -1;
    if (inp[1] >= 0)
      fds[nfd] = (struct pollfd){inp[1], POLLOUT, 0}, ii = (int)nfd++;
    if (outp[0] >= 0)
      fds[nfd] = (struct pollfd){outp[0], POLLIN, 0}, oi = (int)nfd++;
    if (errp[0] >= 0)
      fds[nfd] = (struct pollfd){errp[0], POLLIN, 0}, ei = (int)nfd++;
    poll(fds, nfd, 50);
    if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
      if (input && in_off < input_len) {
        ssize_t w = write(inp[1], (const char *)input + in_off, input_len - in_off);
        if (w > 0)
          in_off += (size_t)w;
        else if (w < 0 && errno != EAGAIN && errno != EINTR)
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
    p.error = true;
  } else
    p.error = true;
  if (!p.out)
    grow(&p.out, &p.out_len, &ocap, "", 0);
  if (!p.err)
    grow(&p.err, &p.err_len, &ecap, "", 0);
  return p;
}
static bool write_file(const char *path, const void *data, size_t len, mode_t mode) {
  char *dir = strrchr(path, '/') ? strdup(path) : NULL;
  if (dir) {
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
      *slash = 0;
      char *partial = strdup(dir);
      for (char *p = partial + 1; *p; p++) {
        if (*p == '/') {
          *p = 0;
          mkdir(partial, 0700);
          *p = '/';
        }
      }
      mkdir(partial, 0700);
      free(partial);
    }
    free(dir);
  }
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
  bool ok = fchmod(fd, mode) == 0 && close(fd) == 0;
  return ok;
}
static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  char *buf = NULL;
  size_t cap = 0, n = 0, got;
  char tmp[4096];
  while ((got = fread(tmp, 1, sizeof tmp, f))) {
    if (!grow(&buf, &n, &cap, tmp, got)) {
      free(buf);
      fclose(f);
      return NULL;
    }
  }
  fclose(f);
  if (!buf && !grow(&buf, &n, &cap, "", 0))
    return NULL;
  if (len)
    *len = n;
  return buf;
}
static bool json_eq(yyjson_val *a, yyjson_val *b);
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
  yyjson_obj_foreach(a, i, n, k, v) {
    yyjson_val *u = yyjson_obj_getn(b, yyjson_get_str(k), yyjson_get_len(k));
    if (!json_eq(v, u))
      return false;
  }
  return true;
}
static bool str_eq(yyjson_val *v, const char *s) {
  size_t n = strlen(s);
  return yyjson_is_str(v) && yyjson_get_len(v) == n && memcmp(yyjson_get_str(v), s, n) == 0;
}
static char *mut_write(yyjson_mut_doc *doc, size_t *len) {
  return yyjson_mut_write(doc, YYJSON_WRITE_ESCAPE_UNICODE, len);
}
static yyjson_doc *call_doc(const char *op, const char *json, size_t len, char **env) {
  char *argv[] = {fixture, (char *)op, NULL};
  Proc p = proc_run(fixture, argv, NULL, json, len, env, 5000);
  if (p.error || p.status != 0) {
    FAIL("%s status %d err %s", op, p.status, p.err ? p.err : "");
    proc_clear(&p);
    return NULL;
  }
  yyjson_doc *doc = yyjson_read(p.out ? p.out : "", p.out_len, 0);
  if (!doc)
    FAIL("%s invalid json: %s", op, p.out ? p.out : "");
  proc_clear(&p);
  return doc;
}
static yyjson_doc *call_mut(const char *op, yyjson_mut_doc *doc, char **env) {
  size_t n = 0;
  char *json = mut_write(doc, &n);
  yyjson_mut_doc_free(doc);
  if (!json) {
    FAIL("serialize %s", op);
    return NULL;
  }
  yyjson_doc *out = call_doc(op, json, n, env);
  free(json);
  return out;
}
static void add_item(yyjson_mut_doc *doc, yyjson_mut_val *arr, const char *id, size_t id_len,
                     const char *reason, const char *url) {
  yyjson_mut_val *o = yyjson_mut_obj(doc);
  if (id)
    yyjson_mut_obj_add_strncpy(doc, o, "id", id, id_len);
  if (reason)
    yyjson_mut_obj_add_strcpy(doc, o, "reason", reason);
  yyjson_mut_val *repo = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, repo, "full_name", "acme/widgets");
  yyjson_mut_obj_add_val(doc, o, "repository", repo);
  yyjson_mut_val *subject = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, subject, "title", "Fix the parser");
  if (!url)
    url = "https://api.github.com/repos/acme/widgets/pulls/4";
  yyjson_mut_obj_add_strcpy(doc, subject, "url", url);
  yyjson_mut_obj_add_val(doc, o, "subject", subject);
  yyjson_mut_arr_add_val(arr, o);
}
static yyjson_mut_doc *plan_doc(yyjson_mut_val *items, yyjson_mut_doc *doc, yyjson_mut_val *state) {
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_val(doc, root, "items", items);
  yyjson_mut_obj_add_val(doc, root, "state", state);
  return doc;
}
static yyjson_mut_val *seeded(yyjson_mut_doc *doc, bool seeded, const char *const *seen, size_t n) {
  yyjson_mut_val *state = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_bool(doc, state, "seeded", seeded);
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  for (size_t i = 0; i < n; i++)
    yyjson_mut_arr_add_strcpy(doc, arr, seen[i]);
  yyjson_mut_obj_add_val(doc, state, "seen", arr);
  return state;
}
static size_t utf8(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}
static const uint32_t spaces[] = {9,  10, 11, 12, 13, 28, 29, 30, 31, 32, 0x85, 0xa0, 0x1680,
                                  0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007,
                                  0x2008, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000};
typedef struct {
  char *root, *commands, *log, *database, *state;
  char **env;
} Sandbox;
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
static char *path_fmt(const char *root, const char *rel) {
  size_t rl = strlen(root), el = strlen(rel);
  char *s = malloc(rl + 1 + el + 1);
  if (!s)
    return NULL;
  memcpy(s, root, rl);
  s[rl] = '/';
  memcpy(s + rl + 1, rel, el + 1);
  return s;
}
static char *env_pair(const char *key, const char *value) {
  size_t kl = strlen(key), vl = strlen(value);
  char *s = malloc(kl + 1 + vl + 1);
  if (!s)
    return NULL;
  memcpy(s, key, kl);
  s[kl] = '=';
  memcpy(s + kl + 1, value, vl + 1);
  return s;
}
static Sandbox *sandbox_new(const char *db_json) {
  Sandbox *s = calloc(1, sizeof *s);
  char tmpl[] = "/tmp/github-watch-XXXXXX";
  if (!mkdtemp(tmpl))
    return s;
  s->root = strdup(tmpl);
  s->commands = path_fmt(s->root, "bin");
  mkdir(s->commands, 0700);
  s->log = path_fmt(s->root, "calls.jsonl");
  s->database = path_fmt(s->root, "db.json");
  s->state = path_fmt(s->root, ".local/state/github-notifications/seen.json");
  write_file(s->database, db_json, strlen(db_json), 0600);
  const char *names[] = {"gh", "notify-send", "xdg-open"};
  for (size_t i = 0; i < 3; i++) {
    char *link = malloc(strlen(s->commands) + 1 + strlen(names[i]) + 1);
    memcpy(link, s->commands, strlen(s->commands));
    link[strlen(s->commands)] = '/';
    memcpy(link + strlen(s->commands) + 1, names[i], strlen(names[i]) + 1);
    if (symlink(fake_bin, link) != 0)
      FAIL("symlink %s", names[i]);
    free(link);
  }
  const char *path = getenv("PATH");
  char *pathv = malloc(strlen(s->commands) + 1 + (path ? strlen(path) : 0) + 6);
  memcpy(pathv, "PATH=", 5);
  memcpy(pathv + 5, s->commands, strlen(s->commands));
  size_t at = 5 + strlen(s->commands);
  pathv[at++] = ':';
  if (path)
    memcpy(pathv + at, path, strlen(path) + 1);
  else
    pathv[at] = 0;
  char *home = env_pair("HOME", s->root);
  char *ignored = path_fmt(s->root, "ignored");
  char *xdg = env_pair("XDG_STATE_HOME", ignored);
  free(ignored);
  char *db = env_pair("FAKE_DB", s->database);
  char *log = env_pair("FAKE_LOG", s->log);
  char *state = env_pair("FAKE_STATE", s->state);
  const char *extra[] = {home, xdg, pathv, db, log, state, NULL};
  s->env = env_with(extra);
  free(home);
  free(xdg);
  free(pathv);
  free(db);
  free(log);
  free(state);
  return s;
}
static void sandbox_seed(Sandbox *s, const char *text) {
  write_file(s->state, text, strlen(text), 0600);
}
static void sandbox_free(Sandbox *s) {
  if (!s)
    return;
  if (s->root)
    rm_rf(s->root);
  env_free(s->env);
  free(s->root);
  free(s->commands);
  free(s->log);
  free(s->database);
  free(s->state);
  free(s);
}
static yyjson_doc *calls_doc(Sandbox *s) {
  size_t n = 0;
  char *text = read_file(s->log, &n);
  if (!text)
    return yyjson_read("[]", 2, 0);
  while (n && (text[n - 1] == '\n' || text[n - 1] == ' '))
    text[--n] = 0;
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  yyjson_mut_doc_set_root(doc, arr);
  size_t i = 0;
  while (i < n) {
    size_t end = i;
    while (end < n && text[end] != '\n')
      end++;
    yyjson_doc *line = yyjson_read(text + i, end - i, 0);
    if (line) {
      yyjson_mut_arr_add_val(arr, yyjson_val_mut_copy(doc, yyjson_doc_get_root(line)));
      yyjson_doc_free(line);
    }
    i = end + 1;
  }
  free(text);
  size_t wn = 0;
  char *out = yyjson_mut_write(doc, 0, &wn);
  yyjson_mut_doc_free(doc);
  yyjson_doc *parsed = out ? yyjson_read(out, wn, 0) : NULL;
  free(out);
  return parsed;
}
static Proc run_watch(char **env) {
  char *argv[] = {binary, NULL};
  return proc_run(binary, argv, NULL, "", 0, env, 5000);
}

static void begin(const char *name) {
  current_test = name;
  printf("RUN %s\n", name);
  fflush(stdout);
}
static void test_pages(void) {
  begin("API issues, pull requests, commits, discussions and comment fallback become GitHub pages");
  const char *pairs[][2] = {
      {"repos/acme/widgets/issues/12", "acme/widgets/issues/12"},
      {"repos/acme/widgets/pulls/4", "acme/widgets/pull/4"},
      {"repos/acme/widgets/issues/12/comments/9", "acme/widgets/issues/12"},
      {"repos/acme/widgets/commits/abc", "acme/widgets/commit/abc"},
      {"repos/acme/widgets/discussions/7", "acme/widgets/discussions/7"},
      {"/repos//acme/widgets/pulls/4?token=discard#fragment", "acme/widgets/pull/4"}};
  for (size_t i = 0; i < 6; i++) {
    char *input = malloc(strlen(pairs[i][0]) + 32);
    memcpy(input, "https://api.github.com/", 23);
    memcpy(input + 23, pairs[i][0], strlen(pairs[i][0]) + 1);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_strcpy(doc, input));
    yyjson_doc *got = call_mut("url", doc, NULL);
    free(input);
    CHECK(got);
    char *expect = malloc(strlen(pairs[i][1]) + 20);
    memcpy(expect, "https://github.com/", 19);
    memcpy(expect + 19, pairs[i][1], strlen(pairs[i][1]) + 1);
    if (!str_eq(yyjson_doc_get_root(got), expect))
      FAIL("url %s", pairs[i][0]);
    free(expect);
    yyjson_doc_free(got);
    if (failures)
      return;
  }
  const char *more[][2] = {
      {"HTTPS://github.com/acme/widgets/issues/1?query=discard#fragment",
       "https://github.com/acme/widgets/issues/1"},
      {" \nhttps://api.github.com/repos/acme/widgets/pulls/4\t",
       "https://github.com/acme/widgets/pull/4"}};
  for (size_t i = 0; i < 2; i++) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_strcpy(doc, more[i][0]));
    yyjson_doc *got = call_mut("url", doc, NULL);
    CHECK(got);
    if (!str_eq(yyjson_doc_get_root(got), more[i][1]))
      FAIL("url extra %zu", i);
    yyjson_doc_free(got);
  }
}
static void test_reject(void) {
  begin("URL trust rejects credentials, explicit ports, wrong hosts/schemes, host roots and numberless comment APIs");
  const char *urls[] = {"",
                        "http://github.com/r",
                        "https://github.com",
                        "https://github.com/",
                        "https://github.com:443/r",
                        "https://u@github.com/r",
                        "https://GitHub.com/r",
                        "https://github.com.evil/r",
                        "https://api.github.com/repos/a/b/issues/comments/1",
                        "https://api.github.com/repos/a/b/issues",
                        "https://api.github.com/repos/a/b/pulls",
                        "https://api.github.com/repos/a/b/releases/1"};
  for (size_t i = 0; i < sizeof urls / sizeof urls[0]; i++) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_strcpy(doc, urls[i]));
    yyjson_doc *got = call_mut("url", doc, NULL);
    CHECK(got);
    if (!str_eq(yyjson_doc_get_root(got), ""))
      FAIL("accepted %s", urls[i]);
    yyjson_doc_free(got);
  }
}
static void test_labels(void) {
  begin("all reason labels and unknown reasons remain stable");
  const char *reasons[][2] = {{"assign", "Assigned"},
                              {"author", "Update"},
                              {"comment", "Comment"},
                              {"ci_activity", "CI"},
                              {"invitation", "Invitation"},
                              {"manual", "Subscribed"},
                              {"mention", "Mention"},
                              {"review_requested", "Review requested"},
                              {"security_alert", "Security alert"},
                              {"state_change", "State change"},
                              {"subscribed", "Subscribed"},
                              {"team_mention", "Team mention"},
                              {"other", "Notification"}};
  for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    add_item(doc, items, "1", 1, reasons[i][0], NULL);
    yyjson_doc *got = call_mut("plan", plan_doc(items, doc, seeded(doc, true, NULL, 0)), NULL);
    CHECK(got);
    yyjson_val *ann = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(got), "announcements"), 0);
    const char *suffix = ": Fix the parser\nhttps://github.com/acme/widgets/pull/4";
    size_t sl = strlen(reasons[i][1]), su = strlen(suffix);
    char *body = malloc(sl + su + 1);
    memcpy(body, reasons[i][1], sl);
    memcpy(body + sl, suffix, su + 1);
    if (!str_eq(yyjson_obj_get(ann, "body"), body))
      FAIL("label %s", reasons[i][0]);
    free(body);
    yyjson_doc_free(got);
  }
}
static void test_first(void) {
  begin("first poll gives only the backlog summary, seeds empty polls, and replaces an unseeded old seen set");
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *items = yyjson_mut_arr(doc);
  add_item(doc, items, "2", 1, "review_requested", NULL);
  add_item(doc, items, "1", 1, "review_requested", NULL);
  add_item(doc, items, "1", 1, "review_requested", NULL);
  add_item(doc, items, "", 0, "review_requested", NULL);
  const char *stale[] = {"stale"};
  yyjson_doc *got = call_mut("plan", plan_doc(items, doc, seeded(doc, false, stale, 1)), NULL);
  CHECK(got);
  yyjson_mut_doc *expect = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(expect);
  yyjson_mut_doc_set_root(expect, root);
  yyjson_mut_val *anns = yyjson_mut_arr(expect);
  yyjson_mut_val *ann = yyjson_mut_obj(expect);
  yyjson_mut_obj_add_strcpy(expect, ann, "summary", "GitHub");
  yyjson_mut_obj_add_strcpy(expect, ann, "body", "3 unread notifications already waiting");
  yyjson_mut_arr_add_val(anns, ann);
  yyjson_mut_obj_add_val(expect, root, "announcements", anns);
  yyjson_mut_val *seen = yyjson_mut_arr(expect);
  yyjson_mut_arr_add_strcpy(expect, seen, "1");
  yyjson_mut_arr_add_strcpy(expect, seen, "2");
  yyjson_mut_obj_add_val(expect, root, "seen", seen);
  yyjson_mut_obj_add_bool(expect, root, "seeded", true);
  yyjson_doc *ed = yyjson_mut_doc_imut_copy(expect, NULL);
  yyjson_mut_doc_free(expect);
  if (!json_eq(yyjson_doc_get_root(got), yyjson_doc_get_root(ed)))
    FAIL("backlog plan");
  yyjson_doc_free(ed);
  yyjson_doc_free(got);
  doc = yyjson_mut_doc_new(NULL);
  got = call_mut("plan", plan_doc(yyjson_mut_arr(doc), doc, seeded(doc, false, NULL, 0)), NULL);
  CHECK(got);
  expect = yyjson_mut_doc_new(NULL);
  root = yyjson_mut_obj(expect);
  yyjson_mut_doc_set_root(expect, root);
  yyjson_mut_obj_add_val(expect, root, "announcements", yyjson_mut_arr(expect));
  yyjson_mut_obj_add_val(expect, root, "seen", yyjson_mut_arr(expect));
  yyjson_mut_obj_add_bool(expect, root, "seeded", true);
  ed = yyjson_mut_doc_imut_copy(expect, NULL);
  yyjson_mut_doc_free(expect);
  if (!json_eq(yyjson_doc_get_root(got), yyjson_doc_get_root(ed)))
    FAIL("empty seed");
  yyjson_doc_free(ed);
  yyjson_doc_free(got);
}
static void test_dedup(void) {
  begin("deduplication retains disappearing IDs and preserves response order rather than sorting alerts");
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *items = yyjson_mut_arr(doc);
  add_item(doc, items, "z", 1, "review_requested", NULL);
  add_item(doc, items, "old", 3, "review_requested", NULL);
  add_item(doc, items, "a", 1, "review_requested", NULL);
  const char *seen0[] = {"old", "gone"};
  yyjson_doc *got = call_mut("plan", plan_doc(items, doc, seeded(doc, true, seen0, 2)), NULL);
  CHECK(got);
  yyjson_val *root = yyjson_doc_get_root(got);
  if (yyjson_arr_size(yyjson_obj_get(root, "announcements")) != 2)
    FAIL("announcement count");
  const char *expect_seen[] = {"a", "gone", "old", "z"};
  yyjson_val *seen = yyjson_obj_get(root, "seen");
  if (yyjson_arr_size(seen) != 4)
    FAIL("seen count");
  for (size_t i = 0; i < 4; i++)
    if (!str_eq(yyjson_arr_get(seen, i), expect_seen[i]))
      FAIL("seen %zu", i);
  yyjson_mut_doc *again = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *items2 = yyjson_mut_arr(again);
  add_item(again, items2, "a", 1, "review_requested", NULL);
  add_item(again, items2, "z", 1, "review_requested", NULL);
  yyjson_mut_val *state = yyjson_val_mut_copy(again, root);
  yyjson_doc_free(got);
  yyjson_mut_val *wrap = yyjson_mut_obj(again);
  yyjson_mut_doc_set_root(again, wrap);
  yyjson_mut_obj_add_val(again, wrap, "items", items2);
  yyjson_mut_obj_add_val(again, wrap, "state", state);
  got = call_mut("plan", again, NULL);
  CHECK(got);
  if (yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(got), "announcements")) != 0)
    FAIL("repeat announced");
  yyjson_doc_free(got);
}
static void test_overflow(void) {
  begin("eight alerts plus the overflow summary mark every fresh and duplicate thread seen");
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *items = yyjson_mut_arr(doc);
  for (int i = 0; i < 11; i++) {
    char id[16];
    int n = snprintf(id, sizeof id, "%d", i);
    add_item(doc, items, id, (size_t)n, "review_requested", NULL);
  }
  yyjson_doc *got = call_mut("plan", plan_doc(items, doc, seeded(doc, true, NULL, 0)), NULL);
  CHECK(got);
  yyjson_val *anns = yyjson_obj_get(yyjson_doc_get_root(got), "announcements");
  if (yyjson_arr_size(anns) != 9 || !str_eq(yyjson_obj_get(yyjson_arr_get(anns, 8), "body"), "3 more new notifications") ||
      yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(got), "seen")) != 11)
    FAIL("overflow");
  yyjson_doc_free(got);
  doc = yyjson_mut_doc_new(NULL);
  items = yyjson_mut_arr(doc);
  for (int i = 0; i < 9; i++)
    add_item(doc, items, "same", 4, "review_requested", NULL);
  got = call_mut("plan", plan_doc(items, doc, seeded(doc, true, NULL, 0)), NULL);
  CHECK(got);
  if (yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(got), "announcements")) != 9 ||
      yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(got), "seen")) != 1 ||
      !str_eq(yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(got), "seen"), 0), "same"))
    FAIL("duplicate overflow");
  yyjson_doc_free(got);
}
static void test_fallback(void) {
  begin("fallback subjects, missing IDs, numeric IDs and latest-comment URLs preserve alert fields");
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *items = yyjson_mut_arr(doc);
  yyjson_mut_val *a = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_uint(doc, a, "id", 12);
  yyjson_mut_val *subject = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, subject, "latest_comment_url",
                            "https://api.github.com/repos/acme/widgets/issues/12/comments/9");
  yyjson_mut_obj_add_val(doc, a, "subject", subject);
  yyjson_mut_arr_add_val(items, a);
  yyjson_mut_val *b = yyjson_mut_obj(doc);
  yyjson_mut_val *subject2 = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, subject2, "title", "ignored");
  yyjson_mut_obj_add_val(doc, b, "subject", subject2);
  yyjson_mut_arr_add_val(items, b);
  yyjson_doc *got = call_mut("plan", plan_doc(items, doc, seeded(doc, true, NULL, 0)), NULL);
  CHECK(got);
  yyjson_val *root = yyjson_doc_get_root(got);
  if (yyjson_arr_size(yyjson_obj_get(root, "seen")) != 1 || !str_eq(yyjson_arr_get(yyjson_obj_get(root, "seen"), 0), "12"))
    FAIL("numeric seen");
  yyjson_mut_doc *expect = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *ann = yyjson_mut_obj(expect);
  yyjson_mut_obj_add_strcpy(expect, ann, "summary", "GitHub");
  yyjson_mut_obj_add_strcpy(expect, ann, "body",
                            "Notification: GitHub notification\nhttps://github.com/acme/widgets/issues/12");
  yyjson_mut_obj_add_strcpy(expect, ann, "url", "https://github.com/acme/widgets/issues/12");
  yyjson_mut_val *arr = yyjson_mut_arr(expect);
  yyjson_mut_arr_add_val(arr, ann);
  yyjson_mut_doc_set_root(expect, arr);
  yyjson_doc *ed = yyjson_mut_doc_imut_copy(expect, NULL);
  yyjson_mut_doc_free(expect);
  if (!json_eq(yyjson_obj_get(root, "announcements"), yyjson_doc_get_root(ed)))
    FAIL("fallback announcement");
  yyjson_doc_free(ed);
  yyjson_doc_free(got);
}
static void test_nul_ids(void) {
  begin("seen identity strings include Unicode and embedded NULs without truncating or merging");
  char a[] = {'a', 0, 'c'};
  char b[] = {'a', 0, 'b'};
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *items = yyjson_mut_arr(doc);
  add_item(doc, items, a, 3, "review_requested", NULL);
  add_item(doc, items, "数学", strlen("数学"), "review_requested", NULL);
  yyjson_mut_val *state = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_bool(doc, state, "seeded", true);
  yyjson_mut_val *seen_in = yyjson_mut_arr(doc);
  yyjson_mut_arr_add_strncpy(doc, seen_in, b, 3);
  yyjson_mut_obj_add_val(doc, state, "seen", seen_in);
  yyjson_doc *got = call_mut("plan", plan_doc(items, doc, state), NULL);
  CHECK(got);
  yyjson_val *root = yyjson_doc_get_root(got);
  if (yyjson_arr_size(yyjson_obj_get(root, "announcements")) != 2)
    FAIL("nul announcements");
  yyjson_val *seen = yyjson_obj_get(root, "seen");
  if (yyjson_arr_size(seen) != 3 || yyjson_get_len(yyjson_arr_get(seen, 0)) != 3 ||
      memcmp(yyjson_get_str(yyjson_arr_get(seen, 0)), b, 3) != 0 ||
      yyjson_get_len(yyjson_arr_get(seen, 1)) != 3 ||
      memcmp(yyjson_get_str(yyjson_arr_get(seen, 1)), a, 3) != 0 ||
      !str_eq(yyjson_arr_get(seen, 2), "数学"))
    FAIL("nul seen");
  yyjson_doc_free(got);
}
static void test_command(void) {
  begin("notify-send argv retains the desktop entry, icon, priority, expiry, separator and Open action");
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *items = yyjson_mut_arr(doc);
  add_item(doc, items, "1", 1, "review_requested", NULL);
  yyjson_doc *planned = call_mut("plan", plan_doc(items, doc, seeded(doc, true, NULL, 0)), NULL);
  CHECK(planned);
  yyjson_val *notice = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(planned), "announcements"), 0);
  yyjson_mut_doc *cmd = yyjson_mut_doc_new(NULL);
  yyjson_mut_doc_set_root(cmd, yyjson_val_mut_copy(cmd, notice));
  yyjson_doc_free(planned);
  yyjson_doc *got = call_mut("command", cmd, NULL);
  CHECK(got);
  const char *expect[] = {"notify-send", "-a", "GitHub", "-i", "github", "-u", "normal", "-t", "30000", "-h",
                          "string:desktop-entry:github-notifications", "-A", "open=Open", "--", "acme/widgets",
                          "Review requested: Fix the parser\nhttps://github.com/acme/widgets/pull/4"};
  yyjson_val *arr = yyjson_doc_get_root(got);
  if (yyjson_arr_size(arr) != sizeof expect / sizeof expect[0])
    FAIL("argv length %zu", yyjson_arr_size(arr));
  for (size_t i = 0; i < sizeof expect / sizeof expect[0] && i < yyjson_arr_size(arr); i++)
    if (!str_eq(yyjson_arr_get(arr, i), expect[i]))
      FAIL("argv %zu", i);
  yyjson_doc_free(got);
  yyjson_mut_doc *unsafe = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *u = yyjson_mut_obj(unsafe);
  yyjson_mut_doc_set_root(unsafe, u);
  yyjson_mut_obj_add_strcpy(unsafe, u, "summary", "--unsafe");
  yyjson_mut_obj_add_strcpy(unsafe, u, "body", "$(not a shell)");
  yyjson_mut_obj_add_strcpy(unsafe, u, "url", "");
  got = call_mut("command", unsafe, NULL);
  CHECK(got);
  arr = yyjson_doc_get_root(got);
  size_t n = yyjson_arr_size(arr);
  if (n < 3 || !str_eq(yyjson_arr_get(arr, n - 3), "--") || !str_eq(yyjson_arr_get(arr, n - 2), "--unsafe") ||
      !str_eq(yyjson_arr_get(arr, n - 1), "$(not a shell)"))
    FAIL("separator");
  yyjson_doc_free(got);
}
static void test_open(void) {
  begin("only the trimmed Open action launches a browser, including Unicode whitespace");
  const char *db = "{\"items\":[]}";
  Sandbox *s = sandbox_new(db);
  CHECK(s && s->root);
  const char *url = "https://github.com/a/b/issues/1";
  size_t outputs = 1 + sizeof spaces / sizeof spaces[0];
  for (size_t i = 0; i < outputs; i++) {
    char text[64];
    size_t n = 0;
    if (i == 0) {
      memcpy(text, "open\n", 5);
      n = 5;
    } else {
      char enc[4];
      size_t el = utf8(spaces[i - 1], enc);
      memcpy(text + n, enc, el);
      n += el;
      memcpy(text + n, "open", 4);
      n += 4;
      memcpy(text + n, enc, el);
      n += el;
    }
    text[n] = 0;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strncpy(doc, root, "output", text, n);
    yyjson_mut_obj_add_strcpy(doc, root, "url", url);
    yyjson_doc *got = call_mut("open", doc, s->env);
    CHECK(got);
    if (!yyjson_get_bool(yyjson_doc_get_root(got)))
      FAIL("open whitespace %zu", i);
    yyjson_doc_free(got);
  }
  const char *bad[] = {"", "closed", "Open", "open something", "op\ven", "\u200bopen\u200b"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "output", bad[i]);
    yyjson_mut_obj_add_strcpy(doc, root, "url", url);
    yyjson_doc *got = call_mut("open", doc, s->env);
    CHECK(got);
    if (yyjson_get_bool(yyjson_doc_get_root(got)))
      FAIL("opened %s", bad[i]);
    yyjson_doc_free(got);
  }
  yyjson_mut_doc *empty = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(empty);
  yyjson_mut_doc_set_root(empty, root);
  yyjson_mut_obj_add_strcpy(empty, root, "output", "open");
  yyjson_mut_obj_add_strcpy(empty, root, "url", "");
  yyjson_doc *got = call_mut("open", empty, s->env);
  CHECK(got);
  if (yyjson_get_bool(yyjson_doc_get_root(got)))
    FAIL("opened empty url");
  yyjson_doc_free(got);
  yyjson_doc *calls = calls_doc(s);
  CHECK(calls);
  if (yyjson_arr_size(yyjson_doc_get_root(calls)) != outputs)
    FAIL("open calls %zu", yyjson_arr_size(yyjson_doc_get_root(calls)));
  for (size_t i = 0; i < outputs && i < yyjson_arr_size(yyjson_doc_get_root(calls)); i++) {
    yyjson_val *args = yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(calls), i), "args");
    if (yyjson_arr_size(args) != 1 || !str_eq(yyjson_arr_get(args, 0), url))
      FAIL("open argv %zu", i);
  }
  yyjson_doc_free(calls);
  sandbox_free(s);
}
static void test_capture(void) {
  begin("subprocess capture suppresses diagnostics, reports failure, handles absent commands and kills timeouts");
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_val *argv = yyjson_mut_arr(doc);
  yyjson_mut_arr_add_strcpy(doc, argv, "sh");
  yyjson_mut_arr_add_strcpy(doc, argv, "-c");
  yyjson_mut_arr_add_strcpy(doc, argv, "printf data; printf secret >&2; exit 4");
  yyjson_mut_obj_add_val(doc, root, "argv", argv);
  yyjson_mut_obj_add_uint(doc, root, "timeout_ms", 1000);
  yyjson_doc *got = call_mut("capture", doc, NULL);
  CHECK(got);
  yyjson_val *v = yyjson_doc_get_root(got);
  if (!yyjson_get_bool(yyjson_obj_get(v, "started")) || yyjson_get_bool(yyjson_obj_get(v, "ok")) ||
      yyjson_get_bool(yyjson_obj_get(v, "timed_out")) || !str_eq(yyjson_obj_get(v, "output"), "data"))
    FAIL("capture failure");
  yyjson_doc_free(got);
  doc = yyjson_mut_doc_new(NULL);
  root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  argv = yyjson_mut_arr(doc);
  yyjson_mut_arr_add_strcpy(doc, argv, "no-such-github-watch-fixture-command");
  yyjson_mut_obj_add_val(doc, root, "argv", argv);
  yyjson_mut_obj_add_uint(doc, root, "timeout_ms", 1000);
  got = call_mut("capture", doc, NULL);
  CHECK(got);
  if (yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(got), "started")))
    FAIL("absent command started");
  yyjson_doc_free(got);
  int64_t started = now_ms();
  doc = yyjson_mut_doc_new(NULL);
  root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  argv = yyjson_mut_arr(doc);
  yyjson_mut_arr_add_strcpy(doc, argv, "sh");
  yyjson_mut_arr_add_strcpy(doc, argv, "-c");
  yyjson_mut_arr_add_strcpy(doc, argv, "exec sleep 10");
  yyjson_mut_obj_add_val(doc, root, "argv", argv);
  yyjson_mut_obj_add_uint(doc, root, "timeout_ms", 100);
  got = call_mut("capture", doc, NULL);
  CHECK(got);
  v = yyjson_doc_get_root(got);
  if (!yyjson_get_bool(yyjson_obj_get(v, "timed_out")) || yyjson_get_bool(yyjson_obj_get(v, "ok")) ||
      now_ms() - started >= 2000)
    FAIL("timeout");
  yyjson_doc_free(got);
}
static bool cli_ok(Proc *p) {
  return !p->error && p->status == 0 && p->signal == 0 && p->out_len == 0 && p->err_len == 0;
}
static void test_cli_first(void) {
  begin("CLI first poll uses only the read-only gh command, saves sorted state and exits without opening");
  const char *db =
      "{\"items\":[{\"id\":\"2\",\"reason\":\"review_requested\",\"repository\":{\"full_name\":\"acme/widgets\"},"
      "\"subject\":{\"title\":\"Fix the parser\",\"url\":\"https://api.github.com/repos/acme/widgets/pulls/4\"}},"
      "{\"id\":\"1\",\"reason\":\"review_requested\",\"repository\":{\"full_name\":\"acme/widgets\"},"
      "\"subject\":{\"title\":\"Fix the parser\",\"url\":\"https://api.github.com/repos/acme/widgets/pulls/4\"}}],"
      "\"action\":\"open\"}";
  Sandbox *s = sandbox_new(db);
  CHECK(s && s->root);
  Proc p = run_watch(s->env);
  if (!cli_ok(&p))
    FAIL("cli %d %s", p.status, p.err ? p.err : "");
  proc_clear(&p);
  yyjson_doc *calls = calls_doc(s);
  CHECK(calls);
  yyjson_val *arr = yyjson_doc_get_root(calls);
  if (yyjson_arr_size(arr) != 2 || !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 0), "kind"), "gh") ||
      !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 1), "kind"), "notify-send"))
    FAIL("kinds");
  yyjson_val *args = yyjson_obj_get(yyjson_arr_get(arr, 0), "args");
  if (yyjson_arr_size(args) != 3 || !str_eq(yyjson_arr_get(args, 0), "api") ||
      !str_eq(yyjson_arr_get(args, 1), "--paginate") || !str_eq(yyjson_arr_get(args, 2), "notifications"))
    FAIL("gh argv");
  yyjson_val *nargs = yyjson_obj_get(yyjson_arr_get(arr, 1), "args");
  if (!str_eq(yyjson_arr_get(nargs, yyjson_arr_size(nargs) - 1), "2 unread notifications already waiting"))
    FAIL("summary");
  yyjson_doc_free(calls);
  size_t n = 0;
  char *state = read_file(s->state, &n);
  yyjson_doc *saved = state ? yyjson_read(state, n, 0) : NULL;
  CHECK(saved);
  yyjson_mut_doc *expect = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(expect);
  yyjson_mut_doc_set_root(expect, root);
  yyjson_mut_obj_add_bool(expect, root, "seeded", true);
  yyjson_mut_val *seen = yyjson_mut_arr(expect);
  yyjson_mut_arr_add_strcpy(expect, seen, "1");
  yyjson_mut_arr_add_strcpy(expect, seen, "2");
  yyjson_mut_obj_add_val(expect, root, "seen", seen);
  yyjson_doc *ed = yyjson_mut_doc_imut_copy(expect, NULL);
  yyjson_mut_doc_free(expect);
  if (!json_eq(yyjson_doc_get_root(saved), yyjson_doc_get_root(ed)))
    FAIL("saved state");
  yyjson_doc_free(ed);
  yyjson_doc_free(saved);
  free(state);
  char *tmp = path_fmt(s->root, ".local/state/github-notifications/seen.tmp");
  if (access(tmp, F_OK) == 0)
    FAIL("temp state remains");
  free(tmp);
  sandbox_free(s);
}
static void test_cli_join(void) {
  begin("CLI joins notification actions after saving state, ignores notify exit status, and never invokes a shell");
  const char *db =
      "{\"items\":[{\"id\":\"1\",\"reason\":\"review_requested\",\"repository\":{\"full_name\":\"acme/widgets\"},"
      "\"subject\":{\"title\":\"Fix the parser\",\"url\":\"https://api.github.com/repos/acme/widgets/pulls/4\"}},"
      "{\"id\":\"2\",\"reason\":\"review_requested\",\"repository\":{\"full_name\":\"acme/widgets\"},"
      "\"subject\":{\"title\":\"Fix the parser\",\"url\":\"https://api.github.com/repos/acme/widgets/pulls/4\"}}],"
      "\"action\":\"open\\n\",\"waitForState\":true,\"notifyExit\":4}";
  Sandbox *s = sandbox_new(db);
  CHECK(s && s->root);
  sandbox_seed(s, "{\"seeded\":true,\"seen\":[\"gone\"]}");
  Proc p = run_watch(s->env);
  if (!cli_ok(&p))
    FAIL("cli %d %s", p.status, p.err ? p.err : "");
  proc_clear(&p);
  yyjson_doc *calls = calls_doc(s);
  CHECK(calls);
  size_t notify = 0, open = 0, before = 0;
  size_t i, n;
  yyjson_val *row;
  yyjson_arr_foreach(yyjson_doc_get_root(calls), i, n, row) {
    if (str_eq(yyjson_obj_get(row, "kind"), "notify-send"))
      notify++;
    if (str_eq(yyjson_obj_get(row, "kind"), "xdg-open")) {
      open++;
      yyjson_val *args = yyjson_obj_get(row, "args");
      if (yyjson_arr_size(args) != 1 || !str_eq(yyjson_arr_get(args, 0), "https://github.com/acme/widgets/pull/4"))
        FAIL("open args");
    }
    if (str_eq(yyjson_obj_get(row, "kind"), "state-before-action")) {
      before++;
      yyjson_val *seen = yyjson_obj_get(row, "seen");
      if (yyjson_arr_size(seen) != 3 || !str_eq(yyjson_arr_get(seen, 0), "1") ||
          !str_eq(yyjson_arr_get(seen, 1), "2") || !str_eq(yyjson_arr_get(seen, 2), "gone"))
        FAIL("seen before action");
    }
  }
  if (notify != 2 || open != 2 || before != 2)
    FAIL("counts notify %zu open %zu before %zu", notify, open, before);
  yyjson_doc_free(calls);
  char *ignored = path_fmt(s->root, "ignored/github-notifications/seen.json");
  if (access(ignored, F_OK) == 0)
    FAIL("xdg state used");
  free(ignored);
  sandbox_free(s);
}
static void test_cli_repeat(void) {
  begin("CLI repeats exit promptly without notifications or browser processes");
  const char *db =
      "{\"items\":[{\"id\":\"1\",\"reason\":\"review_requested\",\"repository\":{\"full_name\":\"acme/widgets\"},"
      "\"subject\":{\"title\":\"Fix the parser\",\"url\":\"https://api.github.com/repos/acme/widgets/pulls/4\"}}],"
      "\"action\":\"closed\"}";
  Sandbox *s = sandbox_new(db);
  CHECK(s && s->root);
  sandbox_seed(s, "{\"seeded\":true,\"seen\":[\"1\"]}");
  Proc p = run_watch(s->env);
  if (p.error || p.status != 0)
    FAIL("status");
  proc_clear(&p);
  yyjson_doc *calls = calls_doc(s);
  CHECK(calls);
  yyjson_val *arr = yyjson_doc_get_root(calls);
  if (yyjson_arr_size(arr) != 1 || !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 0), "kind"), "gh"))
    FAIL("only gh");
  yyjson_doc_free(calls);
  sandbox_free(s);
}
static void test_cli_errors(void) {
  begin("CLI gh errors, empty output, malformed JSON and concatenated pages never change state or show alerts");
  const char *dbs[] = {"{\"ghExit\":1,\"ghError\":true}", "{\"raw\":\"\"}", "{\"raw\":\"{bad\"}",
                       "{\"raw\":\"[]\\n[]\"}"};
  const char *old = "{\"seeded\":true, \"seen\":[\"old\"]}\n";
  for (size_t i = 0; i < 4; i++) {
    Sandbox *s = sandbox_new(dbs[i]);
    CHECK(s && s->root);
    sandbox_seed(s, old);
    Proc p = run_watch(s->env);
    if (!cli_ok(&p))
      FAIL("cli %zu", i);
    proc_clear(&p);
    size_t n = 0;
    char *text = read_file(s->state, &n);
    if (!text || n != strlen(old) || memcmp(text, old, n) != 0)
      FAIL("state changed %zu", i);
    free(text);
    yyjson_doc *calls = calls_doc(s);
    CHECK(calls);
    yyjson_val *arr = yyjson_doc_get_root(calls);
    if (yyjson_arr_size(arr) != 1 || !str_eq(yyjson_obj_get(yyjson_arr_get(arr, 0), "kind"), "gh"))
      FAIL("alerts %zu", i);
    yyjson_doc_free(calls);
    sandbox_free(s);
  }
}
static void test_cli_object(void) {
  begin("CLI supports object items and malformed saved JSON reseeds instead of replaying a burst");
  const char *db =
      "{\"raw\":\"{\\\"items\\\":[{\\\"id\\\":\\\"5\\\",\\\"reason\\\":\\\"review_requested\\\","
      "\\\"repository\\\":{\\\"full_name\\\":\\\"acme/widgets\\\"},\\\"subject\\\":{\\\"title\\\":"
      "\\\"Fix the parser\\\",\\\"url\\\":\\\"https://api.github.com/repos/acme/widgets/pulls/4\\\"}}]}\"}";
  Sandbox *s = sandbox_new(db);
  CHECK(s && s->root);
  sandbox_seed(s, "{broken");
  Proc p = run_watch(s->env);
  if (p.error || p.status != 0)
    FAIL("status");
  proc_clear(&p);
  yyjson_doc *calls = calls_doc(s);
  CHECK(calls);
  bool found = false;
  size_t i, n;
  yyjson_val *row;
  yyjson_arr_foreach(yyjson_doc_get_root(calls), i, n, row) {
    if (str_eq(yyjson_obj_get(row, "kind"), "notify-send")) {
      yyjson_val *args = yyjson_obj_get(row, "args");
      found = str_eq(yyjson_arr_get(args, yyjson_arr_size(args) - 1), "1 unread notifications already waiting");
    }
  }
  if (!found)
    FAIL("reseed summary");
  yyjson_doc_free(calls);
  sandbox_free(s);
}
static void test_cli_closed(void) {
  begin("CLI closed actions and untrusted URLs do not launch browsers");
  const char *dbs[] = {
      "{\"items\":[{\"id\":\"1\",\"reason\":\"review_requested\",\"repository\":{\"full_name\":\"acme/widgets\"},"
      "\"subject\":{\"title\":\"Fix the parser\",\"url\":\"https://api.github.com/repos/acme/widgets/pulls/4\"}}],"
      "\"action\":\"closed\"}",
      "{\"items\":[{\"id\":\"1\",\"reason\":\"mention\",\"repository\":{\"full_name\":\"acme/widgets\"},"
      "\"subject\":{\"title\":\"Fix the parser\",\"url\":\"https://evil.example/private\"}}],\"action\":\"open\"}"};
  for (size_t d = 0; d < 2; d++) {
    Sandbox *s = sandbox_new(dbs[d]);
    CHECK(s && s->root);
    sandbox_seed(s, "{\"seeded\":true,\"seen\":[]}");
    Proc p = run_watch(s->env);
    if (p.error || p.status != 0)
      FAIL("status");
    proc_clear(&p);
    yyjson_doc *calls = calls_doc(s);
    CHECK(calls);
    size_t i, n;
    yyjson_val *row;
    yyjson_arr_foreach(yyjson_doc_get_root(calls), i, n, row) if (str_eq(yyjson_obj_get(row, "kind"), "xdg-open"))
        FAIL("opened");
    yyjson_doc_free(calls);
    sandbox_free(s);
  }
}

int main(void) {
  binary = sibling("GITHUB_WATCH_BIN", "github-notification-watch");
  fixture = sibling("GITHUB_WATCH_FIXTURE", "watch-fixture");
  fake_bin = sibling(NULL, "watch-fake");
  if (!binary || !fixture || !fake_bin || access(binary, X_OK) != 0 || access(fixture, X_OK) != 0 ||
      access(fake_bin, X_OK) != 0) {
    fprintf(stderr, "Set GITHUB_WATCH_BIN and GITHUB_WATCH_FIXTURE\n");
    return 1;
  }
  test_pages();
  test_reject();
  test_labels();
  test_first();
  test_dedup();
  test_overflow();
  test_fallback();
  test_nul_ids();
  test_command();
  test_open();
  test_capture();
  test_cli_first();
  test_cli_join();
  test_cli_repeat();
  test_cli_errors();
  test_cli_object();
  test_cli_closed();
  free(binary);
  free(fixture);
  free(fake_bin);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("ok");
  return 0;
}
