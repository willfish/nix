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

static const char *test_name;
static int failures;
static char *tools, *icons, *fixture, *python;
static const char *plugin, *omarchy, *legacy;
extern char **environ;
static const char *color_rel = "shell/Commons/Color.qml";
static const char *panel_rel = "shell/Ui/KeyboardPanel.qml";
static const char *theme_anchor =
    "readonly property string currentThemePath: stateHome + \"/omarchy/current/theme\"";
static const char *icon_theme =
    "def from_icon_theme(names):\n"
    "    \"\"\"Whatever the machine already has for this name.\"\"\"\n"
    "    for name in names:";
static const char *icon_desktop =
    "if want in haystack.split(\"-\") or (\"-\" + want + \"-\") in (\"-\" + haystack + \"-\"):";

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
static char *sibling_dir(void) {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n < 0)
    return NULL;
  exe[n] = 0;
  return g_path_get_dirname(exe);
}
typedef struct {
  int status;
  unsigned char *out, *err;
  size_t out_len, err_len;
  bool error;
} Proc;
static void proc_free(Proc *p) {
  g_free(p->out);
  g_free(p->err);
  memset(p, 0, sizeof *p);
}
static bool grow(unsigned char **buf, size_t *len, size_t *cap, const void *add, size_t n) {
  if (*len > SIZE_MAX - n - 1)
    return false;
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1) {
      if (next > SIZE_MAX / 2)
        return false;
      next *= 2;
    }
    unsigned char *p = g_try_realloc(*buf, next);
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
  size_t base = 0, extra = 0, n = 0;
  while (environ[base])
    base++;
  if (over)
    while (over[extra])
      extra++;
  char **out = calloc(base + extra + 1, sizeof *out);
  if (!out)
    return NULL;
  for (size_t i = 0; i < base; i++)
    out[n++] = g_strdup(environ[i]);
  if (!over)
    return out;
  for (size_t i = 0; over[i]; i++) {
    const char *eq = strchr(over[i], '=');
    size_t key = eq ? (size_t)(eq - over[i]) : strlen(over[i]);
    size_t at = 0;
    for (; at < n; at++)
      if (!strncmp(out[at], over[i], key) && out[at][key] == '=')
        break;
    if (!eq) {
      if (at < n) {
        g_free(out[at]);
        memmove(out + at, out + at + 1, (n - at) * sizeof *out);
        n--;
      }
      continue;
    }
    char *copy = g_strdup(over[i]);
    if (at < n) {
      g_free(out[at]);
      out[at] = copy;
    } else
      out[n++] = copy;
  }
  return out;
}
static Proc run_proc(const char *cmd, char *const *argv, const void *input, size_t input_len,
                     const char *const *env, int timeout_ms) {
  Proc p = {.status = -1};
  int inp[2], outp[2], errp[2];
  if (pipe(inp) || pipe(outp) || pipe(errp)) {
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
    char **merged = build_env(env);
    execvpe(cmd, argv, merged ? merged : environ);
    _exit(127);
  }
  setpgid(pid, pid);
  close(inp[0]);
  close(outp[1]);
  close(errp[1]);
  size_t sent = 0;
  while (input && sent < input_len) {
    ssize_t n = write(inp[1], (const char *)input + sent, input_len - sent);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    sent += (size_t)n;
  }
  close(inp[1]);
  fcntl(outp[0], F_SETFL, O_NONBLOCK);
  fcntl(errp[0], F_SETFL, O_NONBLOCK);
  size_t ocap = 0, ecap = 0;
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
    if (!reaped && waitpid(pid, &status, WNOHANG) == pid)
      reaped = 1;
  }
  close(outp[0]);
  close(errp[0]);
  if (!reaped)
    waitpid(pid, &status, 0);
  if (!p.out)
    p.out = (unsigned char *)g_malloc0(1);
  if (!p.err)
    p.err = (unsigned char *)g_malloc0(1);
  if (WIFEXITED(status))
    p.status = WEXITSTATUS(status);
  else
    p.error = true;
  return p;
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
    free(j->keys ? j->keys[i] : NULL);
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
  while (p < end && strchr(" \t\r\n", *p))
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
  GString *s = g_string_new(NULL);
  for (p++; p < end && *p != '"';) {
    if (*p != '\\') {
      g_string_append_c(s, *p++);
      continue;
    }
    if (++p >= end)
      break;
    char c = *p++;
    if (c == 'u') {
      unsigned cp = 0;
      for (int i = 0; i < 4 && p < end; i++) {
        int h = hex_digit(*p++);
        if (h < 0) {
          g_string_free(s, TRUE);
          return NULL;
        }
        cp = (cp << 4) | (unsigned)h;
      }
      char utf[4];
      size_t n = 1;
      if (cp < 0x80)
        utf[0] = (char)cp;
      else if (cp < 0x800) {
        utf[0] = (char)(0xc0 | (cp >> 6));
        utf[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
      } else {
        utf[0] = (char)(0xe0 | (cp >> 12));
        utf[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        utf[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
      }
      g_string_append_len(s, utf, (gssize)n);
    } else if (c == 'n')
      g_string_append_c(s, '\n');
    else if (c == 'r')
      g_string_append_c(s, '\r');
    else if (c == 't')
      g_string_append_c(s, '\t');
    else
      g_string_append_c(s, c);
  }
  if (p >= end || *p != '"') {
    g_string_free(s, TRUE);
    return NULL;
  }
  *pp = p + 1;
  *len = s->len;
  return g_string_free(s, FALSE);
}
static Json *parse_value(const char **pp, const char *end) {
  const char *p = skip_ws(*pp, end);
  if (p >= end)
    return NULL;
  if (!strncmp(p, "null", 4)) {
    *pp = p + 4;
    return json_new(J_NULL);
  }
  if (!strncmp(p, "true", 4)) {
    Json *j = json_new(J_BOOL);
    j->b = true;
    *pp = p + 4;
    return j;
  }
  if (!strncmp(p, "false", 5)) {
    *pp = p + 5;
    return json_new(J_BOOL);
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
  if (*p == '[' || *p == '{') {
    char close = *p == '[' ? ']' : '}';
    Json *j = json_new(*p == '[' ? J_ARR : J_OBJ);
    p = skip_ws(p + 1, end);
    while (p < end && *p != close) {
      char *key = NULL;
      if (j->kind == J_OBJ) {
        size_t klen = 0;
        key = parse_string(&p, end, &klen);
        p = skip_ws(p, end);
        if (!key || p >= end || *p != ':') {
          free(key);
          json_free(j);
          return NULL;
        }
        p++;
      }
      Json *item = parse_value(&p, end);
      if (!item) {
        free(key);
        json_free(j);
        return NULL;
      }
      j->items = realloc(j->items, (j->n + 1) * sizeof *j->items);
      j->items[j->n] = item;
      if (j->kind == J_OBJ) {
        j->keys = realloc(j->keys, (j->n + 1) * sizeof *j->keys);
        j->keys[j->n] = key;
      }
      j->n++;
      p = skip_ws(p, end);
      if (p < end && *p == ',')
        p = skip_ws(p + 1, end);
    }
    if (p >= end || *p != close) {
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
  for (size_t i = 0; i < a->n; i++)
    if (!json_equal(a->items[i], json_get(b, a->keys[i])))
      return false;
  return true;
}
static bool is_error(const Json *j, const char *name) {
  const Json *err = json_get(j, "error");
  return j && j->kind == J_OBJ && j->n == 1 && err && err->kind == J_STR &&
         !strcmp(err->text, name);
}
static bool arr_has(const Json *j, const char *text) {
  if (!j || j->kind != J_ARR)
    return false;
  for (size_t i = 0; i < j->n; i++)
    if (j->items[i]->kind == J_STR && !strcmp(j->items[i]->text, text))
      return true;
  return false;
}
static char *temp_dir(void) {
  char tmpl[] = "/tmp/omapager-c-XXXXXX";
  return g_strdup(mkdtemp(tmpl));
}
static void rm_rf(const char *path) {
  struct stat st;
  if (!path || lstat(path, &st))
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
static void save_bytes(const char *path, const void *data, size_t n) {
  char *copy = g_strdup(path);
  for (char *p = copy + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    g_mkdir_with_parents(copy, 0755);
    *p = '/';
  }
  g_free(copy);
  unlink(path);
  if (!g_file_set_contents(path, data, (gssize)n, NULL))
    FAIL("could not write %s", path);
}
static char *read_bytes(const char *path, size_t *len) {
  gchar *data = NULL;
  gsize n = 0;
  if (!g_file_get_contents(path, &data, &n, NULL))
    return NULL;
  if (len)
    *len = n;
  return data;
}
static int copy_tree(const char *from, const char *to) {
  struct stat st;
  if (lstat(from, &st))
    return -1;
  if (S_ISDIR(st.st_mode)) {
    if (g_mkdir_with_parents(to, 0755))
      return -1;
    DIR *dir = opendir(from);
    if (!dir)
      return -1;
    struct dirent *ent;
    int rc = 0;
    while ((ent = readdir(dir))) {
      if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
        continue;
      char *a = g_build_filename(from, ent->d_name, NULL);
      char *b = g_build_filename(to, ent->d_name, NULL);
      if (copy_tree(a, b))
        rc = -1;
      g_free(a);
      g_free(b);
    }
    closedir(dir);
    return rc;
  }
  size_t n = 0;
  char *data = read_bytes(from, &n);
  if (!data)
    return -1;
  save_bytes(to, data, n);
  free(data);
  return 0;
}
static Proc fixture_call(const char *request, const char *upstream, const char *const *env,
                         int timeout) {
  char *argv[] = {fixture, icons, (char *)upstream, NULL};
  if (!upstream)
    argv[2] = NULL;
  return run_proc(fixture, argv, request, strlen(request), env, timeout);
}
static Json *pure(const char *fn, const char *args, const char *kwargs, const char *extra,
                  const char *upstream, const char *const *env) {
  char *req = g_strdup_printf("{\"fn\":%s,\"args\":%s,\"kwargs\":%s%s%s}", fn, args ? args : "[]",
                              kwargs ? kwargs : "{}", extra ? "," : "", extra ? extra : "");
  /* fn is passed as a JSON string including quotes. */
  Proc r = fixture_call(req, upstream, env, 15000);
  if (r.error || r.status != 0)
    FAIL("fixture %s status %d %s", fn, r.status, r.err ? (char *)r.err : "");
  Json *value = json_parse(r.out, r.out_len);
  if (!value)
    FAIL("fixture %s returned invalid JSON", fn);
  if (legacy && !upstream && strcmp(fn, "\"$origin\"")) {
    char *argv[] = {fixture, (char *)legacy, NULL};
    Proc old = run_proc(fixture, argv, req, strlen(req), env, 15000);
    if (old.error || old.status != 0)
      FAIL("legacy fixture %s status %d", fn, old.status);
    Json *other = json_parse(old.out, old.out_len);
    if (!json_equal(value, other))
      FAIL("legacy parity mismatch for %s", req);
    json_free(other);
    proc_free(&old);
  }
  proc_free(&r);
  g_free(req);
  return value;
}
static Json *pure_fn(const char *fn, const char *args, const char *kwargs) {
  char *quoted = g_strdup_printf("\"%s\"", fn);
  Json *v = pure(quoted, args, kwargs, NULL, NULL, NULL);
  g_free(quoted);
  return v;
}
static Proc command(const char *name, const char *path, const char *extra, bool old) {
  char *exe = NULL;
  char *script = NULL;
  char *argv[6];
  int n = 0;
  if (old) {
    exe = g_strdup(python);
    script = g_build_filename(legacy, !strcmp(name, "prepare-shell") ? "prepare-shell.py" : name, NULL);
    if (strcmp(name, "prepare-shell")) {
      char *py = g_strdup_printf("%s.py", name);
      g_free(script);
      script = g_build_filename(legacy, py, NULL);
      g_free(py);
    }
    argv[n++] = exe;
    argv[n++] = script;
  } else {
    exe = g_strdup_printf("%s/omapager-%s", tools, name);
    argv[n++] = exe;
  }
  argv[n++] = (char *)path;
  if (extra)
    argv[n++] = (char *)extra;
  argv[n] = NULL;
  Proc r = run_proc(argv[0], argv, NULL, 0, NULL, 15000);
  g_free(exe);
  g_free(script);
  return r;
}
static void expect_status(Proc *r, int status) {
  if (r->error || r->status != status)
    FAIL("status %d wanted %d stderr %s", r->status, status, r->err ? (char *)r->err : "");
}
static char *join_source(const char *root, const char *rel) {
  return g_build_filename(root, rel, NULL);
}
static void seed(const char *path) {
  char *color = join_source(omarchy, color_rel);
  char *panel = join_source(omarchy, panel_rel);
  char *dc = join_source(path, color_rel);
  char *dp = join_source(path, panel_rel);
  size_t n = 0;
  char *a = read_bytes(color, &n);
  save_bytes(dc, a, n);
  free(a);
  a = read_bytes(panel, &n);
  save_bytes(dp, a, n);
  free(a);
  g_free(color);
  g_free(panel);
  g_free(dc);
  g_free(dp);
}
static bool contains_file(const char *path, const char *text) {
  char *data = read_bytes(path, NULL);
  bool ok = data && strstr(data, text);
  free(data);
  return ok;
}
static int count_text(const char *path, const char *needle) {
  size_t n = 0, m = strlen(needle);
  char *data = read_bytes(path, &n);
  int count = 0;
  if (data && m)
    for (size_t i = 0; i + m <= n; i++)
      if (!memcmp(data + i, needle, m)) {
        count++;
        i += m - 1;
      }
  free(data);
  return count;
}
static void test_aliases(void) {
  begin("installed chat aliases retain order and deduplication");
  Json *v = pure_fn("expand_names",
                    "[[\"Telegram Desktop\",\"GitHub notifications\",\"Whats-App\",\"Discord\",\"Telegram\"]]",
                    NULL);
  Json *want = json_parse(
      "[\"Telegram Desktop\",\"telegram-desktop\",\"org.telegram.desktop\",\"telegram\","
      "\"GitHub notifications\",\"github-notifications\",\"github\",\"Whats-App\",\"whats-app\","
      "\"whatsapp\",\"Discord\",\"discord\",\"Telegram\"]",
      0);
  /* length filled below */
  json_free(want);
  const char *text =
      "[\"Telegram Desktop\",\"telegram-desktop\",\"org.telegram.desktop\",\"telegram\","
      "\"GitHub notifications\",\"github-notifications\",\"github\",\"Whats-App\",\"whats-app\","
      "\"whatsapp\",\"Discord\",\"discord\",\"Telegram\"]";
  want = json_parse(text, strlen(text));
  EXPECT(json_equal(v, want));
  json_free(v);
  json_free(want);
  v = pure_fn("expand_names", "[[\" GitHub \",\"github\",\"GITHUB\",null,false,0,\"\"]]", NULL);
  want = json_parse("[\"GitHub\",\"github\",\"GITHUB\"]", 29);
  EXPECT(json_equal(v, want));
  json_free(v);
  json_free(want);
}
static void test_iterable(void) {
  begin("names iterable, falsy values and string conversions");
  const char *names[] = {"null", "false", "0", "[]", "{}", "\"Ab\"",
                         "[1,true,{},[\"Telegram\"],12.5]",
                         "{\"Discord\":true,\"GitHub\":false}"};
  for (size_t i = 0; i < 8; i++) {
    char *args = g_strdup_printf("[%s]", names[i]);
    Json *v = pure_fn("expand_names", args, NULL);
    json_free(v);
    g_free(args);
  }
  Json *bad = pure_fn("expand_names", "[12]", NULL);
  EXPECT(is_error(bad, "TypeError"));
  json_free(bad);
}
static void test_slug(void) {
  begin("Unicode slug, punctuation and embedded NUL");
  const char *values[] = {"\"İ WhatsApp\"", "\"ẞ_KELVINK\"", "\"foo--💥bar\"", "\"...a..b--\"",
                          NULL, NULL, NULL, "\"a\\u0000b\"", "\"\\u001cTelegram\\u001f\"", "null",
                          "false", "123", "0", "{\"x\":\"İ\"}", "[\"Telegram\"]"};
  char *long_a = g_strnfill(100, 'a');
  char *a_json = g_strdup_printf("\"%s..z\"", long_a);
  char *smile = g_strnfill(256, 'x');
  /* emoji repeat is built as JSON unicode escapes below */
  g_free(smile);
  GString *emoji = g_string_new("\"");
  for (int i = 0; i < 256; i++)
    g_string_append(emoji, "🙂");
  g_string_append(emoji, "Telegram\"");
  GString *dotted = g_string_new("\"");
  for (int i = 0; i < 256; i++)
    g_string_append(dotted, "İ");
  g_string_append(dotted, "X\"");
  values[4] = a_json;
  values[5] = emoji->str;
  values[6] = dotted->str;
  for (size_t i = 0; i < 15; i++) {
    char *args = g_strdup_printf("[%s]", values[i]);
    Json *v = pure_fn("_slug", args, NULL);
    json_free(v);
    g_free(args);
  }
  Json *slug = pure_fn("_slug", "[\"foo--💥bar\"]", NULL);
  EXPECT(slug && slug->kind == J_STR && !strcmp(slug->text, "foo---bar"));
  json_free(slug);
  slug = pure_fn("_slug", "[\"a\\u0000b\"]", NULL);
  EXPECT(slug && slug->kind == J_STR && !strcmp(slug->text, "a-b"));
  json_free(slug);
  g_string_free(emoji, TRUE);
  g_string_free(dotted, TRUE);
  g_free(a_json);
  g_free(long_a);
}
static void test_corpus(void) {
  begin("deterministic Unicode alias corpus");
  const char *glyphs[] = {"a", "Z", "-", ".", "_", " ", "İ", "ẞ", "K", "💥", "🙂", "\\u0000",
                          "\\u001c", "\\u00a0", "\\u2003", "é", "中", "\\ud800"};
  uint32_t seed = 71;
  GString *req = g_string_new("[[");
  for (int i = 0; i < 180; i++) {
    if (i)
      g_string_append_c(req, ',');
    g_string_append_c(req, '"');
    for (int j = 0; j < 40; j++) {
      seed = seed * 1664525u + 1013904223u;
      const char *g = glyphs[seed % 18];
      if (g[0] == '\\')
        g_string_append(req, g);
      else {
        for (const char *p = g; *p; p++) {
          if (*p == '"' || *p == '\\')
            g_string_append_c(req, '\\');
          g_string_append_c(req, *p);
        }
      }
    }
    g_string_append_c(req, '"');
  }
  g_string_append(req, "]]");
  Json *v = pure_fn("expand_names", req->str, NULL);
  json_free(v);
  g_string_free(req, TRUE);
}
static void test_matches(void) {
  begin("desktop matching tokens");
  const char *cases[][3] = {{"Telegram", "org.telegram.desktop telegram", "true"},
                            {"Discord", "discord", "true"},
                            {"app", "whatsapp", "false"},
                            {"hub", "github-notifications", "false"},
                            {"foo-bar", "prefix foo._bar suffix", "true"},
                            {"foo-bar", "foo x bar", "false"},
                            {"foo.bar", "foo.bar", "false"},
                            {"foo.bar", "foobar", "true"},
                            {"foo_bar", "foo_bar", "false"},
                            {"tele-gram", "tele\\u001cgram", "true"},
                            {"abc", "xabc abcxyz", "false"},
                            {"a-b", "a b", "false"},
                            {"foo--bar", "foo bar", "true"}};
  for (size_t i = 0; i < 13; i++) {
    char *args = g_strdup_printf("[\"%s\",\"%s\"]", cases[i][0], cases[i][1]);
    Json *v = pure_fn("name_matches", args, NULL);
    bool expected = !strcmp(cases[i][2], "true");
    if (!v || v->kind != J_BOOL || v->b != expected)
      FAIL("name_matches %s %s", cases[i][0], cases[i][1]);
    json_free(v);
    g_free(args);
  }
}
static void test_keywords(void) {
  begin("keyword calls and argument failures");
  Json *v = pure_fn("expand_names", "[]", "{\"names\":[\"GitHub\"]}");
  EXPECT(arr_has(v, "github"));
  json_free(v);
  v = pure_fn("name_matches", "[]", "{\"want\":\"Telegram\",\"haystack\":\"org.telegram.desktop\"}");
  EXPECT(v && v->kind == J_BOOL && v->b);
  json_free(v);
  v = pure_fn("_slug", "[]", "{\"text\":\" WhatsApp \"}");
  EXPECT(v && v->kind == J_STR && !strcmp(v->text, "whatsapp"));
  json_free(v);
  const char *fns[] = {"expand_names", "expand_names", "name_matches", "allowed_icon", "_slug"};
  const char *args[] = {"[]", "[[]]", "[\"foo\"]", "[]", "[]"};
  const char *kwargs[] = {"{}", "{\"names\":[]}", "{}", "{}", "{\"bad\":1}"};
  for (int i = 0; i < 5; i++) {
    v = pure_fn(fns[i], args[i], kwargs[i]);
    EXPECT(is_error(v, "TypeError"));
    json_free(v);
  }
}
static void test_inside(void) {
  begin("lexical common paths");
  const char *cases[][3] = {{"/tmp/icons/a", "/tmp/icons", "true"},
                            {"/tmp/icons", "/tmp/icons", "true"},
                            {"/tmp/icons2/a", "/tmp/icons", "false"},
                            {"relative/a", "/absolute", "false"},
                            {"relative/a", "relative", "true"},
                            {"//tmp/a", "/tmp", "true"}};
  for (size_t i = 0; i < 6; i++) {
    char *args = g_strdup_printf("[\"%s\",\"%s\"]", cases[i][0], cases[i][1]);
    Json *v = pure_fn("_inside", args, NULL);
    bool expected = !strcmp(cases[i][2], "true");
    EXPECT(v && v->kind == J_BOOL && v->b == expected);
    json_free(v);
    g_free(args);
  }
  Json *v = pure_fn("_inside", "[{\"$bytes\":[47,97,47,98]},{\"$bytes\":[47,97]}]", NULL);
  EXPECT(v && v->kind == J_BOOL && v->b);
  json_free(v);
  v = pure_fn("_inside", "[{\"$bytes\":[47,97,47,98]},\"/a\"]", NULL);
  EXPECT(is_error(v, "TypeError"));
  json_free(v);
}
static char *store_file(void) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps)
    return NULL;
  char line[1024];
  char *found = NULL;
  while (fgets(line, sizeof line, maps)) {
    char *start = strstr(line, "/nix/store/");
    if (!start)
      continue;
    size_t n = strcspn(start, " \n");
    char *path = g_strndup(start, n);
    struct stat st;
    if (!stat(path, &st) && S_ISREG(st.st_mode)) {
      found = path;
      break;
    }
    g_free(path);
  }
  fclose(maps);
  return found;
}
static void test_allowed(void) {
  begin("allowed icons, links, store paths and failures");
  char *root = temp_dir();
  char *base = g_build_filename(root, "icons", NULL);
  char *file = g_build_filename(base, "telegram.svg", NULL);
  save_bytes(file, "svg", 3);
  char *args = g_strdup_printf("[\"%s\",[\"%s\"]]", file, base);
  Json *v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->kind == J_BOOL && v->b);
  json_free(v);
  g_free(args);
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", file, file);
  v = pure_fn("allowed_icon", args, NULL);
  if (!(v && v->b))
    fprintf(stderr, "allowed base-eq args=%s kind=%d\n", args, v ? (int)v->kind : -1);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  args = g_strdup_printf("[{\"$path\":\"%s\"},[{\"$path\":\"%s\"}]]", file, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  char *wrong = g_build_filename(root, "wrong", NULL);
  args = g_strdup_printf("[\"%s\",[\"%s\",\"%s\"]]", file, wrong, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  args = g_strdup_printf("[]");
  char *kwargs = g_strdup_printf("{\"path\":\"%s\",\"bases\":[\"%s\"]}", file, base);
  v = pure_fn("allowed_icon", args, kwargs);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  g_free(kwargs);
  char *other = g_build_filename(root, "outside.svg", NULL);
  char *inside = g_build_filename(base, "inside.svg", NULL);
  char *escape = g_build_filename(base, "escape.svg", NULL);
  char *sibling = g_build_filename(root, "icons-sibling/a.svg", NULL);
  save_bytes(other, "outside", 7);
  EXPECT(!symlink(file, inside));
  EXPECT(!symlink(other, escape));
  save_bytes(sibling, "svg", 3);
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", inside, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", escape, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->kind == J_BOOL && !v->b);
  json_free(v);
  g_free(args);
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", sibling, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->kind == J_BOOL && !v->b);
  json_free(v);
  g_free(args);
  char *outside = g_build_filename(root, "outside", NULL);
  char *linked = g_build_filename(base, "escape", NULL);
  char *selected = g_build_filename(root, "selected-root", NULL);
  g_mkdir_with_parents(outside, 0755);
  save_bytes(g_build_filename(outside, "a.svg", NULL), "svg", 3);
  EXPECT(!symlink(outside, linked));
  EXPECT(!symlink(outside, selected));
  char *escaped = g_build_filename(base, "escape/a.svg", NULL);
  char *selected_file = g_build_filename(selected, "a.svg", NULL);
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", escaped, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && !v->b);
  json_free(v);
  g_free(args);
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", selected_file, selected);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  char *logical = g_strdup_printf("%s/missing/../selected-root", root);
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", selected_file, logical);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  g_free(logical);
  char *store = store_file();
  EXPECT(store && g_str_has_prefix(store, "/nix/store/"));
  char *store_link = g_build_filename(base, "store.svg", NULL);
  EXPECT(!symlink(store, store_link));
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", store_link, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", store, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->kind == J_BOOL && !v->b);
  json_free(v);
  g_free(args);
  char *broken = g_build_filename(base, "broken", NULL);
  char *loop = g_build_filename(base, "loop", NULL);
  char *fifo = g_build_filename(base, "fifo", NULL);
  EXPECT(!symlink("missing", broken));
  EXPECT(!symlink("loop", loop));
  EXPECT(!mkfifo(fifo, 0644));
  const char *names[] = {"missing", "broken", "loop", "fifo"};
  for (int i = 0; i < 4; i++) {
    char *p = g_build_filename(base, names[i], NULL);
    args = g_strdup_printf("[\"%s\",[\"%s\"]]", p, base);
    v = pure_fn("allowed_icon", args, NULL);
    EXPECT(v && v->kind == J_BOOL && !v->b);
    json_free(v);
    g_free(args);
    g_free(p);
  }
  args = g_strdup_printf("[\"%s\",[\"%s\"]]", base, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && !v->b);
  json_free(v);
  g_free(args);
  args = g_strdup_printf("[\"%s\\u0000x\",[\"%s\"]]", base, base);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && !v->b);
  json_free(v);
  g_free(args);
  v = pure_fn("allowed_icon", "[null,null]", NULL);
  EXPECT(v && v->kind == J_BOOL && !v->b);
  json_free(v);
  v = pure_fn("allowed_icon", "[{},[]]", NULL);
  EXPECT(v && v->kind == J_BOOL && !v->b);
  json_free(v);
  GString *bytes = g_string_new("{\"$bytes\":[");
  GString *base_bytes = g_string_new("{\"$bytes\":[");
  char *raw_prefix = g_strdup_printf("%s/", base);
  for (size_t i = 0; raw_prefix[i]; i++)
    g_string_append_printf(bytes, "%s%d", i ? "," : "", (unsigned char)raw_prefix[i]);
  g_string_append(bytes, ",255,46,115,118,103]}");
  g_free(raw_prefix);
  for (size_t i = 0; base[i]; i++)
    g_string_append_printf(base_bytes, "%s%d", i ? "," : "", (unsigned char)base[i]);
  g_string_append(base_bytes, "]}");
  char *raw_name = g_strdup_printf("%s/%c.svg", base, 255);
  save_bytes(raw_name, "svg", 3);
  args = g_strdup_printf("[%s,[%s]]", bytes->str, base_bytes->str);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(v && v->b);
  json_free(v);
  g_free(args);
  GString *file_bytes = g_string_new("{\"$bytes\":[");
  for (size_t i = 0; file[i]; i++)
    g_string_append_printf(file_bytes, "%s%d", i ? "," : "", (unsigned char)file[i]);
  g_string_append(file_bytes, "]}");
  args = g_strdup_printf("[%s,[\"%s\"]]", file_bytes->str, base);
  g_string_free(file_bytes, TRUE);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(is_error(v, "TypeError"));
  json_free(v);
  g_free(args);
  char *outside_file = g_build_filename(root, "other-file", NULL);
  save_bytes(outside_file, "svg", 3);
  char *link = g_build_filename(base, "escape-bytes", NULL);
  EXPECT(!symlink(outside_file, link));
  GString *link_bytes = g_string_new("{\"$bytes\":[");
  for (size_t i = 0; link[i]; i++)
    g_string_append_printf(link_bytes, "%s%d", i ? "," : "", (unsigned char)link[i]);
  g_string_append(link_bytes, "]}");
  args = g_strdup_printf("[%s,[%s]]", link_bytes->str, base_bytes->str);
  v = pure_fn("allowed_icon", args, NULL);
  EXPECT(is_error(v, "TypeError"));
  json_free(v);
  g_free(args);
  g_string_free(link_bytes, TRUE);
  g_string_free(bytes, TRUE);
  g_string_free(base_bytes, TRUE);
  g_free(link);
  g_free(outside_file);
  g_free(raw_name);
  g_free(fifo);
  g_free(loop);
  g_free(broken);
  g_free(store_link);
  g_free(store);
  g_free(selected_file);
  g_free(escaped);
  g_free(selected);
  g_free(linked);
  g_free(outside);
  g_free(sibling);
  g_free(escape);
  g_free(inside);
  g_free(other);
  g_free(wrong);
  g_free(file);
  g_free(base);
  rm_rf(root);
  g_free(root);
}
static void test_repeat_origin(void) {
  begin("repeated operations and compiled import origin");
  Json *v = pure("\"expand_names\"", "[[\"Telegram Desktop\",\"GitHub\"]]", "{}", "\"repeat\":1000",
                 NULL, NULL);
  Json *want = json_parse("[\"Telegram Desktop\",\"telegram-desktop\",\"org.telegram.desktop\","
                          "\"telegram\",\"GitHub\",\"github\"]",
                          0);
  json_free(want);
  const char *text = "[\"Telegram Desktop\",\"telegram-desktop\",\"org.telegram.desktop\","
                     "\"telegram\",\"GitHub\",\"github\"]";
  want = json_parse(text, strlen(text));
  EXPECT(json_equal(v, want));
  json_free(v);
  json_free(want);
  v = pure("\"name_matches\"", "[\"Telegram\",\"org.telegram.desktop\"]", "{}", "\"repeat\":1000",
           NULL, NULL);
  EXPECT(v && v->kind == J_BOOL && v->b);
  json_free(v);
  v = pure_fn("$origin", NULL, NULL);
  EXPECT(v && v->kind == J_STR && g_str_has_suffix(v->text, ".so"));
  json_free(v);
}
static bool files_equal(const char *a, const char *b) {
  size_t na = 0, nb = 0;
  char *da = read_bytes(a, &na), *db = read_bytes(b, &nb);
  bool ok = da && db && na == nb && !memcmp(da, db, na);
  free(da);
  free(db);
  return ok;
}
static void test_prepare(void) {
  begin("pinned preparation and failure ordering");
  char *root = temp_dir();
  char *native = g_build_filename(root, "native", NULL);
  char *old = g_build_filename(root, "legacy", NULL);
  seed(native);
  Proc r = command("prepare-shell", native, NULL, false);
  expect_status(&r, 0);
  proc_free(&r);
  if (legacy) {
    seed(old);
    r = command("prepare-shell", old, NULL, true);
    expect_status(&r, 0);
    proc_free(&r);
    for (int i = 0; i < 2; i++) {
      const char *rel = i ? panel_rel : color_rel;
      char *a = join_source(native, rel), *b = join_source(old, rel);
      EXPECT(files_equal(a, b));
      g_free(a);
      g_free(b);
    }
  }
  EXPECT(contains_file(join_source(native, color_rel), "HYPR_CONTROLS_THEME"));
  EXPECT(contains_file(join_source(native, panel_rel), "Intersection.Subtract"));
  const char *prefixes[] = {"\ufeff", NULL};
  char nul_prefix[] = {'\0', 'p', 'r', 'e', 'f', 'i', 'x', '\n'};
  for (int p = 0; p < 2; p++) {
    for (int t = 0; t < 2; t++) {
      const char *target = t ? old : native;
      if (!legacy && t)
        continue;
      seed(target);
      for (int f = 0; f < 2; f++) {
        const char *rel = f ? panel_rel : color_rel;
        char *path = join_source(target, rel);
        size_t n = 0;
        char *raw = read_bytes(path, &n);
        GString *s = p ? g_string_new_len(nul_prefix, sizeof nul_prefix) : g_string_new(prefixes[p]);
        g_string_append_len(s, raw, (gssize)n);
        if (!f)
          g_string_append_printf(s, "\n%s", theme_anchor);
        for (size_t i = 0; i < s->len; i++)
          if (s->str[i] == '\n')
            g_string_insert_c(s, (gssize)i++, '\r');
        save_bytes(path, s->str, s->len);
        g_string_free(s, TRUE);
        free(raw);
        g_free(path);
      }
    }
    r = command("prepare-shell", native, NULL, false);
    expect_status(&r, 0);
    proc_free(&r);
    if (legacy) {
      r = command("prepare-shell", old, NULL, true);
      expect_status(&r, 0);
      proc_free(&r);
      for (int f = 0; f < 2; f++) {
        char *a = join_source(native, f ? panel_rel : color_rel);
        char *b = join_source(old, f ? panel_rel : color_rel);
        EXPECT(files_equal(a, b));
        g_free(a);
        g_free(b);
      }
    }
    {
      char *color_path = join_source(native, color_rel);
      int found = count_text(color_path, theme_anchor);

      EXPECT(found == 1);
      g_free(color_path);
    }
  }
  const char *mutations[] = {"color", "panel", "absent"};
  for (int i = 0; i < 3; i++) {
    char *target = g_build_filename(root, mutations[i], NULL);
    seed(target);
    char *color = join_source(target, color_rel);
    char *panel = join_source(target, panel_rel);
    if (!strcmp(mutations[i], "color")) {
      size_t n = 0;
      char *raw = read_bytes(color, &n);
      char *hit = raw ? strstr(raw, theme_anchor) : NULL;
      if (hit) {
        GString *s = g_string_new_len(raw, (gssize)(hit - raw));
        g_string_append(s, "missing");
        g_string_append(s, hit + strlen(theme_anchor));
        save_bytes(color, s->str, s->len);
        g_string_free(s, TRUE);
      }
      free(raw);
    } else if (!strcmp(mutations[i], "panel"))
      save_bytes(panel, "missing", 7);
    else
      unlink(panel);
    size_t before_n = 0;
    char *before = read_bytes(color, &before_n);
    r = command("prepare-shell", target, NULL, false);
    expect_status(&r, 1);
    proc_free(&r);
    if (!strcmp(mutations[i], "color")) {
      size_t after_n = 0;
      char *after = read_bytes(color, &after_n);
      EXPECT(before && after && after_n == before_n && !memcmp(after, before, before_n));
      free(after);
    } else
      EXPECT(contains_file(color, "HYPR_CONTROLS_THEME"));
    free(before);
    g_free(panel);
    g_free(color);
    g_free(target);
  }
  for (int f = 0; f < 2; f++) {
    char *target = g_build_filename(root, f ? "panel-utf" : "color-utf", NULL);
    seed(target);
    char *file = join_source(target, f ? panel_rel : color_rel);
    char *color = join_source(target, color_rel);
    unsigned char bad = 255;
    save_bytes(file, &bad, 1);
    size_t n = 0;
    char *before = read_bytes(color, &n);
    r = command("prepare-shell", target, NULL, false);
    expect_status(&r, 1);
    proc_free(&r);
    if (!f) {
      size_t after = 0;
      char *now = read_bytes(color, &after);
      EXPECT(now && after == n && !memcmp(now, before, n));
      free(now);
    } else
      EXPECT(contains_file(color, "HYPR_CONTROLS_THEME"));
    free(before);
    g_free(file);
    g_free(color);
    g_free(target);
  }
  rm_rf(root);
  g_free(old);
  g_free(native);
  g_free(root);
}
static void test_icon_patch(void) {
  begin("icon patch anchors, failures and inode preservation");
  char *root = temp_dir();
  char *icon = join_source(plugin, "bin/omapager-icon");
  size_t n = 0;
  char *raw = read_bytes(icon, &n);
  for (int doubled = 0; doubled < 2; doubled++) {
    GString *text = g_string_new_len(raw, (gssize)n);
    if (doubled) {
      g_string_append_c(text, '\n');
      g_string_append(text, icon_theme);
      g_string_append_c(text, '\n');
      g_string_append(text, icon_desktop);
    }
    char *a = g_build_filename(root, doubled ? "native-d" : "native", NULL);
    char *b = g_build_filename(root, doubled ? "legacy-d" : "legacy", NULL);
    save_bytes(a, text->str, text->len);
    save_bytes(b, text->str, text->len);
    Proc r = command("patch-icon", a, NULL, false);
    expect_status(&r, 0);
    proc_free(&r);
    if (legacy) {
      r = command("patch-icon", b, NULL, true);
      expect_status(&r, 0);
      proc_free(&r);
      EXPECT(files_equal(a, b));
    }
    EXPECT(count_text(a, icon_theme) == (doubled ? 1 : 0));
    g_string_free(text, TRUE);
    g_free(a);
    g_free(b);
  }
  char *target = g_build_filename(root, "target", NULL);
  char *partial = g_strdup_printf("%s\nmissing second anchor", icon_theme);
  save_bytes(target, partial, strlen(partial));
  size_t before_n = 0;
  char *before = read_bytes(target, &before_n);
  Proc r = command("patch-icon", target, NULL, false);
  expect_status(&r, 1);
  proc_free(&r);
  size_t after = 0;
  char *now = read_bytes(target, &after);
  EXPECT(now && after == before_n && !memcmp(now, before, before_n));
  free(now);
  free(before);
  char *full = g_strdup_printf("%s\n%s", icon_theme, icon_desktop);
  save_bytes(target, full, strlen(full));
  char *link = g_build_filename(root, "link", NULL);
  EXPECT(!symlink(target, link));
  struct stat st;
  EXPECT(!stat(target, &st));
  ino_t ino = st.st_ino;
  mode_t mode = st.st_mode;
  r = command("patch-icon", link, NULL, false);
  expect_status(&r, 0);
  proc_free(&r);
  EXPECT(!stat(target, &st));
  EXPECT(st.st_ino == ino && st.st_mode == mode);
  EXPECT(contains_file(target, "expand_names"));
  free(full);
  free(partial);
  g_free(link);
  g_free(target);
  free(raw);
  g_free(icon);
  rm_rf(root);
  g_free(root);
}
static void test_herdr_patch(void) {
  begin("Herdr patch formatting, anchors and substitution");
  char *service = join_source(plugin, "Service.qml");
  size_t n = 0;
  char *service_text = read_bytes(service, &n);
  char *root = temp_dir();
  char *native = g_build_filename(root, "native", NULL);
  char *old = g_build_filename(root, "legacy", NULL);
  const char *script = "/nix/store/example/bin/herdr-notification-focus";
  save_bytes(native, service_text, n);
  save_bytes(old, service_text, n);
  Proc r = command("patch-herdr-focus", native, script, false);
  expect_status(&r, 0);
  proc_free(&r);
  if (legacy) {
    r = command("patch-herdr-focus", old, script, true);
    expect_status(&r, 0);
    proc_free(&r);
    EXPECT(files_equal(native, old));
  }
  EXPECT(contains_file(native, "rememberRecent(row)\n    rememberHerdrTarget(row)"));
  EXPECT(contains_file(native, "if (!handled && row && openHerdrTarget(row))"));
  char *quoted = g_strdup_printf("\"%s\"", script);
  EXPECT(contains_file(native, quoted));
  g_free(quoted);
  const char *anchors[] = {"  function runExecArgv(argv) {", "    rememberRecent(row)\n",
                           "        // Source first, link last. A Slack message quoting a link to\n"};
  for (int i = 0; i < 3; i++) {
    char *file = g_build_filename(root, "service", NULL);
    char *hit = strstr(service_text, anchors[i]);
    EXPECT(hit);
    if (hit) {
      GString *s = g_string_new_len(service_text, (gssize)(hit - service_text));
      g_string_append(s, "gone");
      g_string_append(s, hit + strlen(anchors[i]));
      save_bytes(file, s->str, s->len);
      r = command("patch-herdr-focus", file, "/bin/focus", false);
      expect_status(&r, 1);
      proc_free(&r);
      size_t got = 0;
      char *now = read_bytes(file, &got);
      EXPECT(now && got == s->len && !memcmp(now, s->str, s->len));
      free(now);
      g_string_free(s, TRUE);
    }
    g_free(file);
  }
  char *herdr = g_strdup_printf("%s/omapager-patch-herdr-focus", tools);
  char *icon = g_strdup_printf("%s/omapager-patch-icon", tools);
  char *argv1[] = {herdr, NULL};
  char *argv2[] = {icon, NULL};
  r = run_proc(herdr, argv1, NULL, 0, NULL, 15000);
  expect_status(&r, 1);
  proc_free(&r);
  r = run_proc(icon, argv2, NULL, 0, NULL, 15000);
  expect_status(&r, 1);
  proc_free(&r);
  const char *edge_script = "/a unicode/π\"\\x";
  GString *doubled = g_string_new_len(service_text, (gssize)n);
  g_string_append_len(doubled, service_text, (gssize)n);
  GString *cr = g_string_new("\ufeff");
  for (size_t i = 0; i < n; i++)
    g_string_append_c(cr, service_text[i] == '\n' ? '\r' : service_text[i]);
  GString *nul = g_string_new_len(service_text, (gssize)n);
  g_string_append_c(nul, 0);
  g_string_append(nul, "tail");
  GString *variants[] = {doubled, cr, nul};
  for (int i = 0; i < 3; i++) {
    char *a = g_build_filename(root, "a", NULL);
    char *b = g_build_filename(root, "b", NULL);
    save_bytes(a, variants[i]->str, variants[i]->len);
    save_bytes(b, variants[i]->str, variants[i]->len);
    r = command("patch-herdr-focus", a, edge_script, false);
    expect_status(&r, 0);
    proc_free(&r);
    if (legacy) {
      r = command("patch-herdr-focus", b, edge_script, true);
      expect_status(&r, 0);
      proc_free(&r);
      EXPECT(files_equal(a, b));
    }
    g_free(a);
    g_free(b);
  }
  g_string_free(doubled, TRUE);
  g_string_free(cr, TRUE);
  g_string_free(nul, TRUE);
  g_free(herdr);
  g_free(icon);
  g_free(old);
  g_free(native);
  free(service_text);
  g_free(service);
  rm_rf(root);
  g_free(root);
}
static void test_runtime(void) {
  const char *bin = getenv("OMAPAGER_RUNTIME_BIN");
  if (!bin || !*bin) {
    printf("SKIP packaged upstream worker (OMAPAGER_RUNTIME_BIN unset)\n");
    return;
  }
  begin("packaged upstream worker");
  char *root = temp_dir();
  char *svg = g_build_filename(
      root, ".local/share/icons/hicolor/scalable/apps/org.telegram.desktop.svg", NULL);
  save_bytes(svg, "<svg/>", 6);
  char *worker = g_build_filename(bin, "omapager-run-helper", NULL);
  char *argv[] = {python, worker, "icon", "--app-icon", "Telegram Desktop", "--why", NULL};
  char *home = g_strdup_printf("HOME=%s", root);
  const char *env[] = {home, "OMAPAGER_REQUIRE_SANDBOX=0", "PYTHONPATH=/nonexistent", NULL};
  Proc r = run_proc(python, argv, NULL, 0, env, 60000);
  expect_status(&r, 0);
  char *want = g_strdup_printf("%s\tfrom_icon_theme:hint", svg);
  EXPECT(r.out_len == strlen(want) && !memcmp(r.out, want, r.out_len));
  proc_free(&r);
  g_free(want);
  g_free(home);
  g_free(worker);
  g_free(svg);
  rm_rf(root);
  g_free(root);
}
static void test_upstream_functions(void) {
  begin("patched upstream icon functions import the compiled module");
  char *root = temp_dir();
  char *bin = g_build_filename(root, "bin", NULL);
  char *src = join_source(plugin, "bin");
  EXPECT(!copy_tree(src, bin));
  char *icon = g_build_filename(bin, "omapager-icon", NULL);
  EXPECT(!chmod(bin, 0700));
  EXPECT(!chmod(icon, 0700));
  size_t n = 0;
  char *text = read_bytes(icon, &n);
  const char *old =
      "from omapager_files import private_dir, write_bytes, write_json, read_json";
  char *hit = text ? strstr(text, old) : NULL;
  EXPECT(hit);
  if (hit) {
    GString *s = g_string_new_len(text, (gssize)(hit - text));
    g_string_append(s, old);
    g_string_append(s, "\nimport icon_paths");
    g_string_append(s, hit + strlen(old));
    save_bytes(icon, s->str, s->len);
    g_string_free(s, TRUE);
  }
  free(text);
  Proc r = command("patch-icon", icon, NULL, false);
  expect_status(&r, 0);
  proc_free(&r);
  char *file = g_build_filename(
      root, ".local/share/icons/hicolor/scalable/apps/org.telegram.desktop.svg", NULL);
  save_bytes(file, "<svg/>", 6);
  char *home = g_strdup_printf("HOME=%s", root);
  const char *env[] = {home, NULL};
  Json *v = pure("\"from_icon_theme\"", "[[\"Telegram Desktop\"]]", "{}", NULL, icon, env);
  EXPECT(v && v->kind == J_STR && !strcmp(v->text, file));
  json_free(v);
  char *desktop = g_build_filename(root, ".local/share/applications/org.telegram.desktop.desktop", NULL);
  save_bytes(desktop, "[Desktop Entry]\nName=Telegram\nIcon=org.telegram.desktop\n", 62);
  v = pure("\"from_desktop_entries\"", "[[\"Telegram\"]]", "{}", NULL, icon, env);
  EXPECT(v && v->kind == J_STR && !strcmp(v->text, file));
  json_free(v);
  g_free(desktop);
  g_free(home);
  g_free(file);
  g_free(icon);
  g_free(src);
  g_free(bin);
  rm_rf(root);
  g_free(root);
}
int main(void) {
  char *dir = sibling_dir();
  const char *tools_env = getenv("OMAPAGER_TOOLS_DIR");
  const char *icons_env = getenv("OMAPAGER_ICONS_DIR");
  const char *fixture_env = getenv("OMAPAGER_ICON_FIXTURE");
  const char *python_env = getenv("OMAPAGER_PYTHON");
  plugin = getenv("OMAPAGER_PLUGIN_SOURCE");
  omarchy = getenv("OMAPAGER_OMARCHY_SOURCE");
  legacy = getenv("OMAPAGER_LEGACY_DIR");
  if (legacy && !*legacy)
    legacy = NULL;
  tools = tools_env && *tools_env ? g_strdup(tools_env) : g_strdup(dir);
  icons = icons_env && *icons_env ? g_strdup(icons_env) : g_strdup(tools);
  fixture = fixture_env && *fixture_env ? g_strdup(fixture_env)
                                       : g_build_filename(dir, "icon-fixture", NULL);
  python = g_strdup(python_env && *python_env ? python_env : "python3");
  if (!tools || !fixture || !plugin || !omarchy || access(fixture, X_OK)) {
    fprintf(stderr, "Set command/fixture and pinned source paths\n");
    return 2;
  }
  test_aliases();
  test_iterable();
  test_slug();
  test_corpus();
  test_matches();
  test_keywords();
  test_inside();
  test_allowed();
  test_repeat_origin();
  test_prepare();
  test_icon_patch();
  test_herdr_patch();
  test_runtime();
  test_upstream_functions();
  g_free(dir);
  g_free(tools);
  g_free(icons);
  g_free(fixture);
  g_free(python);
  if (!failures)
    fprintf(stderr, "ok omapager-checks\n");
  return failures ? 1 : 0;
}
