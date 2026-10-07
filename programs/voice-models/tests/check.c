#define _GNU_SOURCE
#include <dirent.h>
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

#ifndef ASSETS_JSON
#define ASSETS_JSON "tests/assets.json"
#endif

static const char *test_name;
static int failures;
static char *binary, *fixture, *http_bin;
static const char payload[] = "speech model fixture\n";
extern char **environ;

static void failf(const char *file, int line, const char *fmt, ...) {
  fprintf(stderr, "FAIL %s (%s:%d): ", test_name, file, line);
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
  test_name = name;
  printf("RUN %s\n", name);
  fflush(stdout);
}
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static char *sibling(const char *env, const char *name) {
  const char *value = env ? getenv(env) : NULL;
  if (value && *value)
    return g_strdup(value);
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
typedef struct {
  int status;
  unsigned char *out, *err;
  size_t out_len, err_len;
  bool error;
} Proc;
static void proc_free(Proc *p) {
  free(p->out);
  free(p->err);
  memset(p, 0, sizeof *p);
}
static bool grow(unsigned char **buf, size_t *len, size_t *cap, const void *add,
                 size_t n) {
  if (*len > SIZE_MAX - n - 1)
    return false;
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1) {
      if (next > SIZE_MAX / 2)
        return false;
      next *= 2;
    }
    unsigned char *p = realloc(*buf, next);
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
static char **build_env(const char *const *over) {
  size_t base = 0, extra = 0;
  while (environ[base])
    base++;
  if (over)
    while (over[extra])
      extra++;
  char **out = calloc(base + extra + 1, sizeof *out);
  if (!out)
    return NULL;
  size_t n = 0;
  for (size_t i = 0; i < base; i++) {
    out[n] = strdup(environ[i]);
    if (!out[n])
      return out;
    n++;
  }
  if (over) {
    for (size_t i = 0; over[i]; i++) {
      const char *eq = strchr(over[i], '=');
      size_t key = eq ? (size_t)(eq - over[i]) : strlen(over[i]);
      size_t at = 0;
      for (; at < n; at++)
        if (!strncmp(out[at], over[i], key) && out[at][key] == '=')
          break;
      if (!eq) {
        if (at < n) {
          free(out[at]);
          memmove(out + at, out + at + 1, (n - at) * sizeof *out);
          n--;
        }
        continue;
      }
      char *copy = strdup(over[i]);
      if (!copy)
        return out;
      if (at < n) {
        free(out[at]);
        out[at] = copy;
      } else
        out[n++] = copy;
    }
  }
  return out;
}
static Proc run_proc(const char *cmd, char *const *argv, const char *cwd,
                     const char *const *env, int timeout_ms) {
  Proc p = {.status = -1};
  int outp[2], errp[2];
  if (pipe(outp) || pipe(errp)) {
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
    if (cwd && chdir(cwd))
      _exit(127);
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0)
      dup2(null, 0);
    dup2(outp[1], 1);
    dup2(errp[1], 2);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    char **merged = build_env(env);
    if (!merged)
      _exit(127);
    execvpe(cmd, argv, merged);
    _exit(127);
  }
  setpgid(pid, pid);
  close(outp[1]);
  close(errp[1]);
  fcntl(outp[0], F_SETFL, O_NONBLOCK);
  fcntl(errp[0], F_SETFL, O_NONBLOCK);
  size_t oc = 0, ec = 0, ocap = 0, ecap = 0;
  int out_open = 1, err_open = 1, reaped = 0, status = 0;
  int64_t deadline = now_ms() + timeout_ms;
  while (out_open || err_open || !reaped) {
    if (now_ms() > deadline) {
      kill(-pid, SIGKILL);
      p.error = true;
      break;
    }
    struct pollfd fds[2];
    nfds_t nf = 0;
    if (out_open)
      fds[nf++] = (struct pollfd){outp[0], POLLIN, 0};
    if (err_open)
      fds[nf++] = (struct pollfd){errp[0], POLLIN, 0};
    poll(fds, nf, 30);
    for (nfds_t i = 0; i < nf; i++) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      unsigned char tmp[4096];
      ssize_t n = read(fds[i].fd, tmp, sizeof tmp);
      unsigned char **dest = fds[i].fd == outp[0] ? &p.out : &p.err;
      size_t *len = fds[i].fd == outp[0] ? &p.out_len : &p.err_len;
      size_t *cap = fds[i].fd == outp[0] ? &ocap : &ecap;
      int *open = fds[i].fd == outp[0] ? &out_open : &err_open;
      if (n > 0) {
        if (!grow(dest, len, cap, tmp, (size_t)n))
          *open = 0;
      } else if (n == 0 || errno != EAGAIN)
        *open = 0;
    }
    (void)oc;
    (void)ec;
    if (!reaped && waitpid(pid, &status, WNOHANG) == pid)
      reaped = 1;
  }
  close(outp[0]);
  close(errp[0]);
  if (!reaped)
    waitpid(pid, &status, 0);
  if (!p.out)
    p.out = (unsigned char *)calloc(1, 1);
  if (!p.err)
    p.err = (unsigned char *)calloc(1, 1);
  if (WIFEXITED(status))
    p.status = WEXITSTATUS(status);
  else
    p.error = true;
  return p;
}
static bool contains(const Proc *p, const char *text) {
  return p->err && memmem(p->err, p->err_len, text, strlen(text));
}
static bool out_eq(const Proc *p, const char *text) {
  size_t n = strlen(text);
  return p->out_len == n && !memcmp(p->out, text, n);
}
typedef struct Json Json;
struct Json {
  enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } kind;
  bool b;
  char *text;
  size_t text_len;
  Json **items;
  char **keys;
  size_t n;
};
static void json_free(Json *j) {
  if (!j)
    return;
  free(j->text);
  for (size_t i = 0; i < j->n; i++) {
    json_free(j->items[i]);
    if (j->keys)
      free(j->keys[i]);
  }
  free(j->items);
  free(j->keys);
  free(j);
}
static Json *json_new(int kind) {
  Json *j = calloc(1, sizeof *j);
  if (j)
    j->kind = kind;
  return j;
}
static const char *skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    p++;
  return p;
}
static int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static char *parse_string(const char **pp, const char *end, size_t *len) {
  const char *p = *pp;
  if (p >= end || *p != '"')
    return NULL;
  p++;
  GString *s = g_string_new(NULL);
  while (p < end && *p != '"') {
    if (*p == '\\') {
      p++;
      if (p >= end) {
        g_string_free(s, TRUE);
        return NULL;
      }
      char c = *p++;
      if (c == 'u') {
        if (p + 4 > end) {
          g_string_free(s, TRUE);
          return NULL;
        }
        int cp = 0;
        for (int i = 0; i < 4; i++) {
          int h = hex_digit(*p++);
          if (h < 0) {
            g_string_free(s, TRUE);
            return NULL;
          }
          cp = (cp << 4) | h;
        }
        char utf[4];
        if (cp < 0x80)
          g_string_append_c(s, (char)cp);
        else if (cp < 0x800) {
          utf[0] = (char)(0xc0 | (cp >> 6));
          utf[1] = (char)(0x80 | (cp & 0x3f));
          g_string_append_len(s, utf, 2);
        } else {
          utf[0] = (char)(0xe0 | (cp >> 12));
          utf[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
          utf[2] = (char)(0x80 | (cp & 0x3f));
          g_string_append_len(s, utf, 3);
        }
      } else if (c == 'n')
        g_string_append_c(s, '\n');
      else if (c == 'r')
        g_string_append_c(s, '\r');
      else if (c == 't')
        g_string_append_c(s, '\t');
      else
        g_string_append_c(s, c);
    } else
      g_string_append_c(s, *p++);
  }
  if (p >= end || *p != '"') {
    g_string_free(s, TRUE);
    return NULL;
  }
  *pp = p + 1;
  *len = s->len;
  return g_string_free(s, FALSE);
}
static Json *parse_value(const char **pp, const char *end);
static Json *parse_value(const char **pp, const char *end) {
  const char *p = skip_ws(*pp, end);
  if (p >= end)
    return NULL;
  if (*p == 'n' && end - p >= 4 && !memcmp(p, "null", 4)) {
    *pp = p + 4;
    return json_new(J_NULL);
  }
  if (*p == 't' && end - p >= 4 && !memcmp(p, "true", 4)) {
    Json *j = json_new(J_BOOL);
    j->b = true;
    *pp = p + 4;
    return j;
  }
  if (*p == 'f' && end - p >= 5 && !memcmp(p, "false", 5)) {
    Json *j = json_new(J_BOOL);
    *pp = p + 5;
    return j;
  }
  if (*p == '"') {
    size_t len = 0;
    char *text = parse_string(&p, end, &len);
    if (!text)
      return NULL;
    Json *j = json_new(J_STR);
    j->text = text;
    j->text_len = len;
    *pp = p;
    return j;
  }
  if (*p == '-' || (*p >= '0' && *p <= '9')) {
    const char *start = p++;
    while (p < end && strchr("0123456789.eE+-", *p))
      p++;
    Json *j = json_new(J_NUM);
    j->text_len = (size_t)(p - start);
    j->text = g_strndup(start, j->text_len);
    *pp = p;
    return j;
  }
  if (*p == '[') {
    Json *j = json_new(J_ARR);
    p++;
    p = skip_ws(p, end);
    while (p < end && *p != ']') {
      Json *item = parse_value(&p, end);
      if (!item) {
        json_free(j);
        return NULL;
      }
      j->items = realloc(j->items, (j->n + 1) * sizeof *j->items);
      j->items[j->n++] = item;
      p = skip_ws(p, end);
      if (p < end && *p == ',')
        p = skip_ws(p + 1, end);
    }
    if (p >= end || *p != ']') {
      json_free(j);
      return NULL;
    }
    *pp = p + 1;
    return j;
  }
  if (*p == '{') {
    Json *j = json_new(J_OBJ);
    p++;
    p = skip_ws(p, end);
    while (p < end && *p != '}') {
      size_t klen = 0;
      char *key = parse_string(&p, end, &klen);
      if (!key) {
        json_free(j);
        return NULL;
      }
      p = skip_ws(p, end);
      if (p >= end || *p != ':') {
        free(key);
        json_free(j);
        return NULL;
      }
      p++;
      Json *item = parse_value(&p, end);
      if (!item) {
        free(key);
        json_free(j);
        return NULL;
      }
      j->keys = realloc(j->keys, (j->n + 1) * sizeof *j->keys);
      j->items = realloc(j->items, (j->n + 1) * sizeof *j->items);
      j->keys[j->n] = key;
      j->items[j->n++] = item;
      p = skip_ws(p, end);
      if (p < end && *p == ',')
        p = skip_ws(p + 1, end);
    }
    if (p >= end || *p != '}') {
      json_free(j);
      return NULL;
    }
    *pp = p + 1;
    return j;
  }
  return NULL;
}
static Json *json_parse(const void *data, size_t len) {
  const char *p = data, *end = p + len;
  Json *j = parse_value(&p, end);
  p = skip_ws(p, end);
  if (!j || p != end) {
    json_free(j);
    return NULL;
  }
  return j;
}
static const Json *json_get(const Json *j, const char *key) {
  if (!j || j->kind != J_OBJ)
    return NULL;
  for (size_t i = 0; i < j->n; i++)
    if (!strcmp(j->keys[i], key))
      return j->items[i];
  return NULL;
}
static bool json_equal(const Json *a, const Json *b) {
  if (!a || !b || a->kind != b->kind)
    return false;
  if (a->kind == J_NULL)
    return true;
  if (a->kind == J_BOOL)
    return a->b == b->b;
  if (a->kind == J_NUM || a->kind == J_STR)
    return a->text_len == b->text_len && !memcmp(a->text, b->text, a->text_len);
  if (a->n != b->n)
    return false;
  if (a->kind == J_ARR) {
    for (size_t i = 0; i < a->n; i++)
      if (!json_equal(a->items[i], b->items[i]))
        return false;
    return true;
  }
  for (size_t i = 0; i < a->n; i++) {
    const Json *other = json_get(b, a->keys[i]);
    if (!json_equal(a->items[i], other))
      return false;
  }
  return true;
}
static char *read_file(const char *path, size_t *len) {
  gchar *data = NULL;
  gsize n = 0;
  if (!g_file_get_contents(path, &data, &n, NULL))
    return NULL;
  if (len)
    *len = n;
  return data;
}
static char *digest_hex(const void *data, size_t n) {
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, n);
}
static char *temp_dir(void) {
  char tmpl[] = "/tmp/voice-models-XXXXXX";
  char *path = mkdtemp(tmpl);
  return path ? strdup(path) : NULL;
}
static void rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st))
    return;
  if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
    DIR *dir = opendir(path);
    if (dir) {
      struct dirent *ent;
      while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
          continue;
        char *child = g_build_filename(path, ent->d_name, NULL);
        rm_rf(child);
        g_free(child);
      }
      closedir(dir);
    }
    rmdir(path);
  } else
    unlink(path);
}
static void no_temporary(const char *path) {
  DIR *dir = opendir(path);
  if (!dir)
    return;
  struct dirent *ent;
  while ((ent = readdir(dir))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    if (g_str_has_suffix(ent->d_name, ".partial"))
      FAIL("temporary remains: %s", ent->d_name);
    char *child = g_build_filename(path, ent->d_name, NULL);
    struct stat st;
    if (!lstat(child, &st) && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
      no_temporary(child);
    g_free(child);
  }
  closedir(dir);
}
typedef struct {
  pid_t pid;
  int port;
  char *log;
  char *url;
} Server;
static void server_stop(Server *s) {
  if (!s)
    return;
  if (s->pid > 0) {
    kill(s->pid, SIGTERM);
    waitpid(s->pid, NULL, 0);
  }
  free(s->log);
  free(s->url);
  free(s);
}
static Server *server_start(const char *mode, int status, const char *cert,
                            const char *key) {
  char dir[] = "/tmp/voice-http-XXXXXX";
  if (!mkdtemp(dir))
    return NULL;
  char *port_file = g_build_filename(dir, "port", NULL);
  char *log = g_build_filename(dir, "log", NULL);
  char status_text[16];
  snprintf(status_text, sizeof status_text, "%d", status);
  char *argv[] = {http_bin, port_file, log, (char *)mode, status_text,
                  (char *)cert, (char *)key, NULL};
  if (!cert)
    argv[5] = NULL;
  pid_t pid = fork();
  if (pid < 0)
    return NULL;
  if (!pid) {
    execv(http_bin, argv);
    _exit(127);
  }
  int port = 0;
  for (int i = 0; i < 200 && !port; i++) {
    char *text = NULL;
    gsize n = 0;
    if (g_file_get_contents(port_file, &text, &n, NULL) && n)
      port = atoi(text);
    g_free(text);
    if (!port) {
      struct timespec pause = {.tv_nsec = 20 * 1000 * 1000};
      nanosleep(&pause, NULL);
    }
  }
  g_free(port_file);
  if (!port) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    g_free(log);
    rm_rf(dir);
    return NULL;
  }
  Server *s = calloc(1, sizeof *s);
  s->pid = pid;
  s->port = port;
  s->log = log;
  s->url = g_strdup_printf("%s://127.0.0.1:%d/model", cert ? "https" : "http",
                           port);
  return s;
}
typedef struct {
  char *url;
  char *ua;
  char *connection;
  char *encoding;
  bool accept_present;
  char *accept;
} Req;
static Req *read_reqs(const char *path, size_t *count) {
  *count = 0;
  char *text = read_file(path, NULL);
  if (!text)
    return NULL;
  Req *reqs = NULL;
  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    Json *j = json_parse(line, strlen(line));
    if (!j)
      continue;
    reqs = realloc(reqs, (*count + 1) * sizeof *reqs);
    Req *r = &reqs[*count];
    memset(r, 0, sizeof *r);
    const Json *url = json_get(j, "url");
    const Json *ua = json_get(j, "user-agent");
    const Json *conn = json_get(j, "connection");
    const Json *enc = json_get(j, "accept-encoding");
    const Json *accept = json_get(j, "accept");
    r->url = url && url->text ? strdup(url->text) : strdup("");
    r->ua = ua && ua->text ? strdup(ua->text) : strdup("");
    r->connection = conn && conn->text ? strdup(conn->text) : strdup("");
    r->encoding = enc && enc->text ? strdup(enc->text) : strdup("");
    r->accept_present = accept && accept->kind == J_STR;
    r->accept = r->accept_present ? strdup(accept->text) : NULL;
    (*count)++;
    json_free(j);
  }
  free(text);
  return reqs;
}
static void free_reqs(Req *reqs, size_t n) {
  for (size_t i = 0; i < n; i++) {
    free(reqs[i].url);
    free(reqs[i].ua);
    free(reqs[i].connection);
    free(reqs[i].encoding);
    free(reqs[i].accept);
  }
  free(reqs);
}
static Proc install(const char *root, const char *url, const char *hash,
                    const char *bytes, const char *idle, bool check,
                    const char *const *env) {
  char *argv[] = {fixture, "install", (char *)root, "models/model.bin",
                  (char *)url, (char *)hash, (char *)bytes, (char *)idle,
                  check ? "1" : "0", NULL};
  return run_proc(fixture, argv, NULL, env, 15000);
}
static bool dir_empty(const char *path) {
  DIR *dir = opendir(path);
  if (!dir)
    return false;
  struct dirent *ent;
  bool empty = true;
  while ((ent = readdir(dir)))
    if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
      empty = false;
  closedir(dir);
  return empty;
}
static int file_mode(const char *path) {
  struct stat st;
  if (stat(path, &st))
    return -1;
  return (int)(st.st_mode & 0777);
}
static bool bytes_eq(const char *path, const void *data, size_t n) {
  size_t got = 0;
  char *text = read_file(path, &got);
  bool ok = text && got == n && !memcmp(text, data, n);
  free(text);
  return ok;
}
static void expect_status(const Proc *p, int status) {
  if (p->error || p->status != status)
    FAIL("status %d (wanted %d) stderr %s", p->status, status,
         p->err ? (char *)p->err : "");
}
static Json *filter_assets(const Json *pinned, const char *host, bool stt,
                           bool experimental) {
  Json *out = json_new(J_ARR);
  for (size_t i = 0; i < pinned->n; i++) {
    const Json *asset = pinned->items[i];
    const Json *role = json_get(asset, "role");
    const Json *hosts = json_get(asset, "hosts");
    const Json *exp = json_get(asset, "experimental");
    if (stt && !(role && role->kind == J_STR && !strcmp(role->text, "stt")))
      continue;
    if (hosts) {
      bool found = false;
      if (hosts->kind == J_ARR)
        for (size_t h = 0; h < hosts->n; h++)
          if (hosts->items[h]->kind == J_STR &&
              !strcmp(hosts->items[h]->text, host))
            found = true;
      if (!found)
        continue;
    }
    if (exp && exp->kind == J_BOOL && exp->b && !experimental)
      continue;
    out->items = realloc(out->items, (out->n + 1) * sizeof *out->items);
    /* Share no ownership: reparse via equality against fixture output. */
    out->items[out->n++] = (Json *)asset;
  }
  return out;
}
static void test_pinned(void) {
  begin("pinned assets and defaults");
  size_t n = 0;
  char *raw = read_file(ASSETS_JSON, &n);
  Json *pinned = raw ? json_parse(raw, n) : NULL;
  free(raw);
  EXPECT(pinned);
  char *argv[] = {fixture, "assets", NULL};
  Proc result = run_proc(fixture, argv, NULL, NULL, 15000);
  expect_status(&result, 0);
  Json *got = json_parse(result.out, result.out_len);
  EXPECT(json_equal(got, pinned));
  json_free(got);
  proc_free(&result);
  char *defaults_argv[] = {fixture, "defaults", NULL};
  Proc defaults = run_proc(fixture, defaults_argv, NULL, NULL, 15000);
  Json *def = json_parse(defaults.out, defaults.out_len);
  Json *want = json_parse("{\"idle_ms\":30000,\"backoff_ms\":1000}", 35);
  EXPECT(json_equal(def, want));
  json_free(def);
  json_free(want);
  json_free(pinned);
  proc_free(&defaults);
}
static void test_select(void) {
  begin("host, STT and experimental filters");
  size_t n = 0;
  char *raw = read_file(ASSETS_JSON, &n);
  Json *pinned = json_parse(raw, n);
  free(raw);
  const char *hosts[] = {"foundation", "andromeda", "Andromeda", "other"};
  for (size_t h = 0; h < 4; h++)
    for (int stt = 0; stt < 2; stt++)
      for (int experimental = 0; experimental < 2; experimental++) {
        char *argv[] = {fixture, "select", (char *)hosts[h], stt ? "1" : "0",
                        experimental ? "1" : "0", NULL};
        Proc result = run_proc(fixture, argv, NULL, NULL, 15000);
        expect_status(&result, 0);
        Json *got = json_parse(result.out, result.out_len);
        Json *selected =
            filter_assets(pinned, hosts[h], stt, experimental);
        if (!json_equal(got, selected))
          FAIL("selection mismatch host=%s stt=%d experimental=%d", hosts[h],
               stt, experimental);
        selected->items = NULL;
        selected->n = 0;
        json_free(selected);
        json_free(got);
        proc_free(&result);
      }
  json_free(pinned);
}
static void test_roots(void) {
  begin("check-only roots do not create directories");
  char *home = temp_dir();
  EXPECT(home);
  /* xdg_mode: 0 inherit, 1 unset, 2 path under home, 3 empty. */
  struct {
    const char *arg;
    int xdg_mode;
    const char *suffix;
  } cases[4] = {
      {"explicit", 0, "explicit"},
      {NULL, 1, ".local/share/pi-voice"},
      {NULL, 2, "xdg/pi-voice"},
      {NULL, 3, "pi-voice"},
  };
  for (int i = 0; i < 4; i++) {
    char *data = cases[i].arg ? g_build_filename(home, cases[i].arg, NULL) : NULL;
    char *expected =
        i == 3 ? g_strdup("pi-voice/models/ggml-silero-v6.2.0.bin")
               : g_build_filename(home, cases[i].suffix,
                                  "models/ggml-silero-v6.2.0.bin", NULL);
    char *home_env = g_strdup_printf("HOME=%s", home);
    char *xdg_value =
        cases[i].xdg_mode == 2 ? g_build_filename(home, "xdg", NULL) : NULL;
    char *xdg_env = cases[i].xdg_mode == 1   ? g_strdup("XDG_DATA_HOME")
                    : cases[i].xdg_mode == 3 ? g_strdup("XDG_DATA_HOME=")
                    : cases[i].xdg_mode == 2
                        ? g_strdup_printf("XDG_DATA_HOME=%s", xdg_value)
                        : NULL;
    const char *env[] = {home_env, xdg_env, NULL};
    char *argv_store[8];
    int argc = 0;
    argv_store[argc++] = binary;
    if (data) {
      argv_store[argc++] = "--data-dir";
      argv_store[argc++] = data;
    }
    argv_store[argc++] = "--check-only";
    argv_store[argc++] = "--stt-only";
    argv_store[argc++] = "--host";
    argv_store[argc++] = "foundation";
    argv_store[argc] = NULL;
    Proc result = run_proc(binary, argv_store, home, env, 15000);
    expect_status(&result, 1);
    EXPECT(contains(&result, "Missing or invalid model:"));
    EXPECT(memmem(result.err, result.err_len, expected, strlen(expected)));
    proc_free(&result);
    g_free(data);
    g_free(xdg_value);
    g_free(expected);
    g_free(home_env);
    g_free(xdg_env);
  }
  EXPECT(dir_empty(home));
  rm_rf(home);
  free(home);
}
static void test_usage(void) {
  begin("usage, help and unknown arguments do not download");
  char *help[] = {binary, "--help", NULL};
  Proc ok = run_proc(binary, help, NULL, NULL, 15000);
  expect_status(&ok, 0);
  proc_free(&ok);
  const char *bad[][3] = {{"--bad", NULL, NULL},
                          {"--host", NULL, NULL},
                          {"trailing", NULL, NULL},
                          {"--check-only=1", NULL, NULL}};
  for (size_t i = 0; i < 4; i++) {
    char *argv[] = {binary, (char *)bad[i][0], NULL};
    Proc result = run_proc(binary, argv, NULL, NULL, 15000);
    expect_status(&result, 2);
    proc_free(&result);
  }
}
static Proc install_default(const char *root, const char *url, const char *hash,
                            unsigned long long bytes, long idle, bool check,
                            const char *const *env) {
  char bytes_text[32], idle_text[32];
  snprintf(bytes_text, sizeof bytes_text, "%llu", bytes);
  snprintf(idle_text, sizeof idle_text, "%ld", idle);
  char *use_hash = hash ? (char *)hash : digest_hex(payload, strlen(payload));
  Proc result =
      install(root, url, use_hash, bytes_text, idle_text, check, env);
  if (!hash)
    g_free(use_hash);
  return result;
}
static void test_local_hit(void) {
  begin("matching local size and hash avoids requests");
  char *root = temp_dir();
  char *models = g_build_filename(root, "models", NULL);
  char *dest = g_build_filename(models, "model.bin", NULL);
  g_mkdir_with_parents(models, 0755);
  EXPECT(g_file_set_contents(dest, payload, (gssize)strlen(payload), NULL));
  EXPECT(!chmod(dest, 0644));
  Server *remote = server_start("body", 0, NULL, NULL);
  EXPECT(remote);
  Proc result = install_default(root, remote->url, NULL, strlen(payload), 2000,
                                false, NULL);
  expect_status(&result, 0);
  char *want = g_strdup_printf("Verified %s\n", dest);
  EXPECT(out_eq(&result, want));
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs == 0);
  EXPECT(file_mode(dest) == 0644);
  free_reqs(reqs, nreqs);
  proc_free(&result);
  server_stop(remote);
  g_free(want);
  g_free(dest);
  g_free(models);
  rm_rf(root);
  free(root);
}
static void test_check_only(void) {
  begin("check-only never creates directories or contacts the network");
  char *root = temp_dir();
  Server *remote = server_start("body", 0, NULL, NULL);
  EXPECT(remote);
  char *missing = g_build_filename(root, "missing", NULL);
  Proc miss = install_default(missing, remote->url, NULL, strlen(payload), 2000,
                              true, NULL);
  expect_status(&miss, 1);
  EXPECT(access(missing, F_OK) != 0);
  proc_free(&miss);
  char *models = g_build_filename(root, "models", NULL);
  char *dest = g_build_filename(models, "model.bin", NULL);
  g_mkdir_with_parents(models, 0755);
  g_file_set_contents(dest, payload, strlen(payload), NULL);
  Proc size = install_default(root, remote->url, NULL, strlen(payload) + 1, 2000,
                              true, NULL);
  expect_status(&size, 1);
  proc_free(&size);
  char *zeros = g_strnfill(64, '0');
  Proc hash = install_default(root, remote->url, zeros, strlen(payload), 2000,
                              true, NULL);
  expect_status(&hash, 1);
  proc_free(&hash);
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs == 0);
  free_reqs(reqs, nreqs);
  g_free(zeros);
  server_stop(remote);
  g_free(missing);
  g_free(dest);
  g_free(models);
  rm_rf(root);
  free(root);
}
static void expect_headers(const Req *r) {
  EXPECT(!strcmp(r->ua, "pi-voice-model-setup"));
  EXPECT(!strcmp(r->connection, "close"));
  EXPECT(!strcmp(r->encoding, "identity"));
  EXPECT(!r->accept_present);
}
static void test_install_ok(void) {
  begin("valid streams install atomically");
  char *root = temp_dir();
  Server *remote = server_start("body", 0, NULL, NULL);
  EXPECT(remote);
  Proc result = install_default(root, remote->url, NULL, strlen(payload), 2000,
                                false, NULL);
  expect_status(&result, 0);
  char *dest = g_build_filename(root, "models/model.bin", NULL);
  EXPECT(bytes_eq(dest, payload, strlen(payload)));
  EXPECT(file_mode(dest) == 0600);
  no_temporary(root);
  char *want = g_strdup_printf(
      "Downloading models/model.bin (%zu bytes)\nInstalled %s\n",
      strlen(payload), dest);
  EXPECT(out_eq(&result, want));
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs >= 1);
  if (nreqs)
    expect_headers(&reqs[0]);
  free_reqs(reqs, nreqs);
  proc_free(&result);
  server_stop(remote);
  g_free(want);
  g_free(dest);
  rm_rf(root);
  free(root);
}
static void test_symlink(void) {
  begin("downloaded models replace invalid symlinks");
  char *root = temp_dir();
  char *outside = g_build_filename(root, "outside", NULL);
  char *models = g_build_filename(root, "models", NULL);
  char *dest = g_build_filename(models, "model.bin", NULL);
  g_file_set_contents(outside, "not a model", -1, NULL);
  g_mkdir_with_parents(models, 0755);
  EXPECT(!symlink(outside, dest));
  Server *remote = server_start("body", 0, NULL, NULL);
  Proc result = install_default(root, remote->url, NULL, strlen(payload), 2000,
                                false, NULL);
  expect_status(&result, 0);
  char *kept = NULL;
  g_file_get_contents(outside, &kept, NULL, NULL);
  EXPECT(kept && !strcmp(kept, "not a model"));
  EXPECT(bytes_eq(dest, payload, strlen(payload)));
  no_temporary(root);
  g_free(kept);
  proc_free(&result);
  server_stop(remote);
  g_free(dest);
  g_free(models);
  g_free(outside);
  rm_rf(root);
  free(root);
}
static void test_bad_body(void) {
  begin("same-size bad hashes and oversize bodies do not retry");
  char *zeros = g_strnfill(64, '0');
  struct {
    const char *hash;
    unsigned long long bytes;
    const char *stderr_bit;
  } cases[2] = {{zeros, strlen(payload), NULL},
                {NULL, 2, NULL}};
  for (int i = 0; i < 2; i++) {
    char *root = temp_dir();
    char *models = g_build_filename(root, "models", NULL);
    char *dest = g_build_filename(models, "model.bin", NULL);
    g_mkdir_with_parents(models, 0755);
    g_file_set_contents(dest, "old", -1, NULL);
    Server *remote = server_start("body", 0, NULL, NULL);
    Proc result = install_default(root, remote->url, cases[i].hash,
                                  cases[i].bytes, 2000, false, NULL);
    expect_status(&result, 1);
    EXPECT(contains(&result, "integrity check failed") ||
           contains(&result, "exceeds expected size"));
    size_t nreqs = 0;
    Req *reqs = read_reqs(remote->log, &nreqs);
    EXPECT(nreqs == 1);
    free_reqs(reqs, nreqs);
    char *kept = NULL;
    g_file_get_contents(dest, &kept, NULL, NULL);
    EXPECT(kept && !strcmp(kept, "old"));
    no_temporary(root);
    g_free(kept);
    proc_free(&result);
    server_stop(remote);
    g_free(dest);
    g_free(models);
    rm_rf(root);
    free(root);
  }
  g_free(zeros);
}
static void test_short(void) {
  begin("short complete or truncated content-length bodies");
  const char *modes[] = {"short", "shortcl"};
  for (int i = 0; i < 2; i++) {
    char *root = temp_dir();
    Server *remote = server_start(modes[i], 0, NULL, NULL);
    Proc result = install_default(root, remote->url, NULL, strlen(payload),
                                  2000, false, NULL);
    expect_status(&result, 1);
    EXPECT(contains(&result, "integrity check failed"));
    size_t nreqs = 0;
    Req *reqs = read_reqs(remote->log, &nreqs);
    EXPECT(nreqs == 1);
    free_reqs(reqs, nreqs);
    no_temporary(root);
    proc_free(&result);
    server_stop(remote);
    rm_rf(root);
    free(root);
  }
}
static void test_overstate(void) {
  begin("pinned bytes remain authoritative when Content-Length overstates");
  char *root = temp_dir();
  Server *remote = server_start("over", 0, NULL, NULL);
  Proc result =
      install_default(root, remote->url, NULL, strlen(payload), 2000, false, NULL);
  expect_status(&result, 0);
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs == 1);
  free_reqs(reqs, nreqs);
  char *dest = g_build_filename(root, "models/model.bin", NULL);
  EXPECT(bytes_eq(dest, payload, strlen(payload)));
  no_temporary(root);
  proc_free(&result);
  server_stop(remote);
  g_free(dest);
  rm_rf(root);
  free(root);
}
static void test_chunk(void) {
  begin("truncated chunk framing is rejected without retrying");
  char *root = temp_dir();
  Server *remote = server_start("chunk", 0, NULL, NULL);
  Proc result =
      install_default(root, remote->url, NULL, strlen(payload), 2000, false, NULL);
  expect_status(&result, 1);
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs == 1);
  free_reqs(reqs, nreqs);
  EXPECT(contains(&result, "incomplete chunked"));
  char *dest = g_build_filename(root, "models/model.bin", NULL);
  EXPECT(access(dest, F_OK) != 0);
  no_temporary(root);
  proc_free(&result);
  server_stop(remote);
  g_free(dest);
  rm_rf(root);
  free(root);
}
static void test_retries(void) {
  begin("HTTP and transport failures retry three attempts");
  const char *modes[] = {"503", "reset"};
  for (int i = 0; i < 2; i++) {
    char *root = temp_dir();
    Server *remote = server_start(modes[i], 0, NULL, NULL);
    Proc result = install_default(root, remote->url, NULL, strlen(payload),
                                  2000, false, NULL);
    expect_status(&result, 1);
    size_t nreqs = 0;
    Req *reqs = read_reqs(remote->log, &nreqs);
    EXPECT(nreqs == 3);
    free_reqs(reqs, nreqs);
    size_t downloads = 0;
    if (result.out) {
      const char *p = (char *)result.out;
      while ((p = strstr(p, "Downloading "))) {
        downloads++;
        p += 12;
      }
    }
    EXPECT(downloads == 3);
    no_temporary(root);
    if (!strcmp(modes[i], "503"))
      EXPECT(contains(&result, "HTTP 503"));
    proc_free(&result);
    server_stop(remote);
    rm_rf(root);
    free(root);
  }
}
static void test_recover(void) {
  begin("a retry can recover and commit only the successful download");
  char *root = temp_dir();
  Server *remote = server_start("retry", 0, NULL, NULL);
  Proc result =
      install_default(root, remote->url, NULL, strlen(payload), 2000, false, NULL);
  expect_status(&result, 0);
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs == 3);
  free_reqs(reqs, nreqs);
  char *dest = g_build_filename(root, "models/model.bin", NULL);
  EXPECT(bytes_eq(dest, payload, strlen(payload)));
  no_temporary(root);
  proc_free(&result);
  server_stop(remote);
  g_free(dest);
  rm_rf(root);
  free(root);
}
static void test_fast_errors(void) {
  begin("HTTP errors are recognized without waiting on stalled bodies");
  char *root = temp_dir();
  Server *remote = server_start("stall503", 0, NULL, NULL);
  int64_t start = now_ms();
  Proc result = install_default(root, remote->url, NULL, strlen(payload), 10000,
                                false, NULL);
  EXPECT(now_ms() - start < 3000);
  expect_status(&result, 1);
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs == 3);
  free_reqs(reqs, nreqs);
  no_temporary(root);
  proc_free(&result);
  server_stop(remote);
  rm_rf(root);
  free(root);
}
static void test_redirect_status(void) {
  begin("unhandled HTTP status and missing redirect locations");
  int codes[] = {300, 301, 304, 307};
  for (size_t i = 0; i < 4; i++) {
    char *root = temp_dir();
    Server *remote = server_start("status", codes[i], NULL, NULL);
    int64_t start = now_ms();
    Proc result = install_default(root, remote->url, NULL, strlen(payload),
                                  10000, false, NULL);
    expect_status(&result, 1);
    char *needle = g_strdup_printf("HTTP %d", codes[i]);
    EXPECT(contains(&result, needle));
    size_t nreqs = 0;
    Req *reqs = read_reqs(remote->log, &nreqs);
    EXPECT(nreqs == 3);
    free_reqs(reqs, nreqs);
    EXPECT(now_ms() - start < 3000);
    no_temporary(root);
    g_free(needle);
    proc_free(&result);
    server_stop(remote);
    rm_rf(root);
    free(root);
  }
}
static void test_redirect(void) {
  begin("redirects do not hash their bodies");
  char *root = temp_dir();
  Server *remote = server_start("redirect", 0, NULL, NULL);
  Proc result =
      install_default(root, remote->url, NULL, strlen(payload), 2000, false, NULL);
  expect_status(&result, 0);
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs == 2);
  if (nreqs == 2) {
    EXPECT(!strcmp(reqs[0].url, "/model"));
    EXPECT(!strcmp(reqs[1].url, "/final"));
  }
  free_reqs(reqs, nreqs);
  char *dest = g_build_filename(root, "models/model.bin", NULL);
  EXPECT(bytes_eq(dest, payload, strlen(payload)));
  no_temporary(root);
  proc_free(&result);
  server_stop(remote);
  g_free(dest);
  rm_rf(root);
  free(root);
}
static void test_progress(void) {
  begin("progressing transfers outlive idle; stalled transfers retry");
  char *root = temp_dir();
  Server *remote = server_start("drip", 0, NULL, NULL);
  int64_t start = now_ms();
  Proc result =
      install_default(root, remote->url, NULL, strlen(payload), 300, false, NULL);
  EXPECT(now_ms() - start > 1000);
  expect_status(&result, 0);
  no_temporary(root);
  proc_free(&result);
  server_stop(remote);
  rm_rf(root);
  free(root);
  char *stalled_root = temp_dir();
  Server *stalled = server_start("stall200", 0, NULL, NULL);
  Proc failure = install_default(stalled_root, stalled->url, NULL,
                                 strlen(payload), 250, false, NULL);
  expect_status(&failure, 1);
  size_t nreqs = 0;
  Req *reqs = read_reqs(stalled->log, &nreqs);
  EXPECT(nreqs == 3);
  free_reqs(reqs, nreqs);
  no_temporary(stalled_root);
  proc_free(&failure);
  server_stop(stalled);
  rm_rf(stalled_root);
  free(stalled_root);
}
static void test_directory(void) {
  begin("directory destinations fail publication and retain contents");
  char *root = temp_dir();
  char *dir = g_build_filename(root, "models/model.bin", NULL);
  char *keep = g_build_filename(dir, "keep", NULL);
  g_mkdir_with_parents(dir, 0755);
  g_file_set_contents(keep, "keep", -1, NULL);
  Server *remote = server_start("body", 0, NULL, NULL);
  Proc result =
      install_default(root, remote->url, NULL, strlen(payload), 2000, false, NULL);
  expect_status(&result, 1);
  size_t nreqs = 0;
  Req *reqs = read_reqs(remote->log, &nreqs);
  EXPECT(nreqs == 3);
  free_reqs(reqs, nreqs);
  char *text = NULL;
  g_file_get_contents(keep, &text, NULL, NULL);
  EXPECT(text && !strcmp(text, "keep"));
  no_temporary(root);
  g_free(text);
  proc_free(&result);
  server_stop(remote);
  g_free(keep);
  g_free(dir);
  rm_rf(root);
  free(root);
}
static bool openssl_req(const char *key, const char *cert, const char *subj,
                        const char *san) {
  char *argv[] = {"openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                  "-keyout", (char *)key, "-out", (char *)cert, "-days", "1",
                  "-subj", (char *)subj, "-addext", (char *)san, NULL};
  Proc generated = run_proc("openssl", argv, NULL, NULL, 20000);
  bool ok = !generated.error && generated.status == 0;
  if (!ok)
    FAIL("openssl cert: %s", generated.err ? (char *)generated.err : "");
  proc_free(&generated);
  return ok;
}
static void test_tls(void) {
  begin("TLS requires declared CA trust and checks the hostname");
  char *root = temp_dir();
  char *key = g_build_filename(root, "key.pem", NULL);
  char *cert = g_build_filename(root, "cert.pem", NULL);
  if (!openssl_req(key, cert, "/CN=localhost",
                   "subjectAltName=DNS:localhost,IP:127.0.0.1")) {
    rm_rf(root);
    free(root);
    g_free(key);
    g_free(cert);
    return;
  }
  Server *remote = server_start("body", 0, cert, key);
  if (!remote) {
    FAIL("TLS server did not start");
    rm_rf(root);
    free(root);
    g_free(key);
    g_free(cert);
    return;
  }
  char *untrusted = g_build_filename(root, "untrusted", NULL);
  const char *unset_ca[] = {"SSL_CERT_FILE", NULL};
  Proc bad = install_default(untrusted, remote->url, NULL, strlen(payload),
                             2000, false, unset_ca);
  expect_status(&bad, 1);
  proc_free(&bad);
  char *trusted = g_build_filename(root, "trusted", NULL);
  char *ca_env = g_strdup_printf("SSL_CERT_FILE=%s", cert);
  const char *trust_env[] = {ca_env, NULL};
  Proc good = install_default(trusted, remote->url, NULL, strlen(payload), 2000,
                              false, trust_env);
  expect_status(&good, 0);
  proc_free(&good);
  char *missing = g_build_filename(root, "missing-ca", NULL);
  char *absent = g_build_filename(root, "absent.pem", NULL);
  char *missing_env = g_strdup_printf("SSL_CERT_FILE=%s", absent);
  const char *miss_env[] = {missing_env, NULL};
  Proc gone = install_default(missing, remote->url, NULL, strlen(payload), 2000,
                              false, miss_env);
  expect_status(&gone, 1);
  proc_free(&gone);
  char *wrong = g_build_filename(root, "wrong-cert.pem", NULL);
  EXPECT(openssl_req(key, wrong, "/CN=wrong.example",
                     "subjectAltName=DNS:wrong.example"));
  Server *wrong_host = server_start("body", 0, wrong, key);
  EXPECT(wrong_host);
  char *wrong_dir = g_build_filename(root, "wrong-host", NULL);
  char *wrong_env = g_strdup_printf("SSL_CERT_FILE=%s", wrong);
  const char *reject_env[] = {wrong_env, NULL};
  Proc rejected = install_default(wrong_dir, wrong_host->url, NULL,
                                  strlen(payload), 2000, false, reject_env);
  expect_status(&rejected, 1);
  no_temporary(root);
  proc_free(&rejected);
  server_stop(wrong_host);
  server_stop(remote);
  g_free(wrong_env);
  g_free(wrong_dir);
  g_free(wrong);
  g_free(missing_env);
  g_free(absent);
  g_free(missing);
  g_free(ca_env);
  g_free(trusted);
  g_free(untrusted);
  g_free(cert);
  g_free(key);
  rm_rf(root);
  free(root);
}
int main(void) {
  binary = sibling("VOICE_MODELS_BIN", "voice-model-setup");
  fixture = sibling("VOICE_MODELS_FIXTURE", "model-fixture");
  http_bin = sibling(NULL, "model-http");
  if (!binary || !fixture || !http_bin || access(binary, X_OK) ||
      access(fixture, X_OK) || access(http_bin, X_OK)) {
    fprintf(stderr, "missing voice-model-setup, model-fixture or model-http\n");
    return 2;
  }
  test_pinned();
  test_select();
  test_roots();
  test_usage();
  test_local_hit();
  test_check_only();
  test_install_ok();
  test_symlink();
  test_bad_body();
  test_short();
  test_overstate();
  test_chunk();
  test_retries();
  test_recover();
  test_fast_errors();
  test_redirect_status();
  test_redirect();
  test_progress();
  test_directory();
  test_tls();
  g_free(binary);
  g_free(fixture);
  g_free(http_bin);
  if (!failures)
    fprintf(stderr, "ok voice-models-checks\n");
  return failures ? 1 : 0;
}
