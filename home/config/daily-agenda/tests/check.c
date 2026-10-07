#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

extern char **environ;
static const char *current_test;
static int failures;
static char *binary, *fixture, *limits_bin, *pty_bin, *network, *legacy;
static const char *feed =
    "https://calendar.google.com/calendar/ical/user%40example.com/private-fixture_token_0123456789/basic.ics";
static const uint32_t spaces[] = {9,  10, 11, 12, 13, 28, 29, 30, 31, 32, 0x85, 0xa0, 0x1680, 0x2000,
                                  0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009,
                                  0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000};

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
static void begin(const char *name) {
  current_test = name;
  printf("RUN %s\n", name);
  fflush(stdout);
}
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
static bool same_key(const char *a, const char *b) {
  const char *ae = strchr(a, '='), *be = strchr(b, '=');
  size_t al = ae ? (size_t)(ae - a) : strlen(a);
  size_t bl = be ? (size_t)(be - b) : strlen(b);
  return al == bl && !memcmp(a, b, al);
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
    bool skip = false;
    for (size_t j = 0; overrides && overrides[j]; j++)
      if (same_key(environ[i], overrides[j]))
        skip = true;
    if (!skip)
      out[k++] = g_strdup(environ[i]);
  }
  for (size_t j = 0; j < m; j++) {
    bool later = false;
    for (size_t t = j + 1; t < m; t++)
      if (same_key(overrides[j], overrides[t]))
        later = true;
    if (!later)
      out[k++] = g_strdup(overrides[j]);
  }
  return out;
}
static void env_free(char **env) {
  if (!env)
    return;
  for (size_t i = 0; env[i]; i++)
    g_free(env[i]);
  free(env);
}
static Proc proc_run(const char *file, char *const *argv, const void *input, size_t input_len, char **envp,
                     int timeout_ms) {
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
        size_t chunk = input_len - in_off > 65536 ? 65536 : input_len - in_off;
        ssize_t w = write(inp[1], (const char *)input + in_off, chunk);
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
  char *dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    g_free(dir);
    return false;
  }
  g_free(dir);
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
  gchar *contents = NULL;
  gsize n = 0;
  if (!g_file_get_contents(path, &contents, &n, NULL))
    return NULL;
  if (len)
    *len = n;
  return contents;
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
static bool contains(const char *buf, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (!m)
    return true;
  if (!buf || n < m)
    return false;
  for (size_t i = 0; i + m <= n; i++)
    if (!memcmp(buf + i, needle, m))
      return true;
  return false;
}
static char *env_pair(const char *key, const char *value) {
  return g_strdup_printf("%s=%s", key, value ? value : "");
}
static char **agenda_env(const char *const *extra) {
  size_t m = 0;
  while (extra && extra[m])
    m++;
  const char **all = calloc(m + 2, sizeof *all);
  all[0] = "TZ=UTC";
  for (size_t i = 0; i < m; i++)
    all[i + 1] = extra[i];
  char **env = env_with(all);
  free(all);
  return env;
}
static Proc run_at(const char *file, char *const *args, const void *input, size_t len, const char *const *extra,
                   int timeout_ms) {
  size_t n = 0;
  while (args && args[n])
    n++;
  char **argv = calloc(n + 2, sizeof *argv);
  argv[0] = (char *)file;
  for (size_t i = 0; i < n; i++)
    argv[i + 1] = args[i];
  char **env = agenda_env(extra);
  Proc p = proc_run(file, argv, input, len, env, timeout_ms);
  env_free(env);
  free(argv);
  return p;
}
static Proc run_bin(char *const *args, const void *input, size_t len, const char *const *extra) {
  const char *parent = getenv("DAILY_AGENDA_PARENT_FIXTURE");
  bool parse = args && args[0] && g_str_has_prefix(args[0], "--parse-");
  if (parent && *parent && !parse) {
    size_t n = 0;
    while (args && args[n])
      n++;
    char **rewritten = calloc(n + 2, sizeof *rewritten);
    rewritten[0] = "cli";
    for (size_t i = 0; i < n; i++)
      rewritten[i + 1] = args[i];
    Proc p = run_at(parent, rewritten, input, len, extra, 70000);
    free(rewritten);
    return p;
  }
  return run_at(binary, args, input, len, extra, 70000);
}
static char *replace_one(const char *s, const char *from, const char *to) {
  const char *hit = strstr(s, from);
  if (!hit)
    return g_strdup(s);
  GString *out = g_string_new(NULL);
  g_string_append_len(out, s, hit - s);
  g_string_append(out, to);
  g_string_append(out, hit + strlen(from));
  return g_string_free(out, FALSE);
}
static char *replace_all(const char *s, const char *from, const char *to) {
  GString *out = g_string_new(NULL);
  size_t fl = strlen(from);
  for (const char *p = s; *p;) {
    if (fl && !strncmp(p, from, fl)) {
      g_string_append(out, to);
      p += fl;
    } else
      g_string_append_c(out, *p++);
  }
  return g_string_free(out, FALSE);
}
static char *timed(const char *extra, const char *start) {
  if (!start)
    start = "20261006T090000Z";
  if (extra && *extra)
    return g_strdup_printf("UID:event\nDTSTART:%s\nDTEND:20261006T100000Z\nSUMMARY:Example\n%s", start, extra);
  return g_strdup_printf("UID:event\nDTSTART:%s\nDTEND:20261006T100000Z\nSUMMARY:Example", start);
}
static char *ics_raw(const char *const *events, const size_t *lens, size_t n, const char *properties) {
  GString *s = g_string_new("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Fixtures//EN\r\n");
  if (properties)
    g_string_append(s, properties);
  for (size_t i = 0; i < n; i++) {
    g_string_append(s, "BEGIN:VEVENT\r\n");
    size_t len = lens ? lens[i] : strlen(events[i]);
    for (size_t j = 0; j < len; j++) {
      if (events[i][j] == '\n')
        g_string_append(s, "\r\n");
      else
        g_string_append_c(s, events[i][j]);
    }
    g_string_append(s, "\r\nEND:VEVENT\r\n");
  }
  g_string_append(s, "END:VCALENDAR\r\n");
  return g_string_free(s, FALSE);
}
static char *ics1(const char *event) { return ics_raw(&event, NULL, 1, NULL); }
static char *ics2(const char *a, const char *b) {
  const char *events[] = {a, b};
  return ics_raw(events, NULL, 2, NULL);
}
static yyjson_doc *parity(const void *input, size_t len, char *const *args, int expect) {
  char *def[] = {"--parse-feed", "2026-10-06", "UTC", NULL};
  Proc native = run_bin(args ? args : def, input, len, NULL);
  if (native.error || native.status != expect) {
    FAIL("status %d want %d stderr %s", native.status, expect, native.err ? native.err : "");
    proc_clear(&native);
    return NULL;
  }
  if (legacy) {
    Proc old = run_at(legacy, args ? args : def, input, len, NULL, 70000);
    if (old.error || old.status != native.status)
      FAIL("legacy status %d vs %d", native.status, old.status);
    else if (native.status == 0) {
      yyjson_doc *a = yyjson_read(native.out ? native.out : "", native.out_len, 0);
      yyjson_doc *b = yyjson_read(old.out ? old.out : "", old.out_len, 0);
      if (!a || !b || !json_eq(yyjson_doc_get_root(a), yyjson_doc_get_root(b)))
        FAIL("legacy calendar differs");
      yyjson_doc_free(a);
      yyjson_doc_free(b);
    }
    proc_clear(&old);
  }
  yyjson_doc *doc = NULL;
  if (expect == 0) {
    doc = yyjson_read(native.out ? native.out : "", native.out_len, 0);
    if (!doc)
      FAIL("invalid calendar json");
  }
  proc_clear(&native);
  return doc;
}
static void parity_only(const char *event, char *const *args, int expect) {
  char *text = ics1(event);
  yyjson_doc *doc = parity(text, strlen(text), args, expect);
  yyjson_doc_free(doc);
  g_free(text);
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
static void utc_day(char *buf, size_t n, int add) {
  time_t t = time(NULL) + (time_t)add * 86400;
  struct tm tm;
  char tmp[32];
  gmtime_r(&t, &tm);
  int wrote = snprintf(tmp, sizeof tmp, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  if (wrote < 0 || (size_t)wrote >= sizeof tmp || n == 0)
    return;
  if ((size_t)wrote >= n)
    wrote = (int)n - 1;
  memcpy(buf, tmp, (size_t)wrote);
  buf[wrote] = 0;
}
static void utc_stamp(char *buf, size_t n) {
  time_t t = time(NULL);
  struct tm tm;
  char tmp[64];
  gmtime_r(&t, &tm);
  int wrote = snprintf(tmp, sizeof tmp, "%04d-%02d-%02dT%02d:%02d:%02d.000Z", tm.tm_year + 1900,
                       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  if (wrote < 0 || (size_t)wrote >= sizeof tmp || n == 0)
    return;
  if ((size_t)wrote >= n)
    wrote = (int)n - 1;
  memcpy(buf, tmp, (size_t)wrote);
  buf[wrote] = 0;
}
static char *scratch(void) {
  char tmpl[] = "/tmp/agenda-fixture-XXXXXX";
  return g_strdup(mkdtemp(tmpl));
}
static char **home_env(const char *root, const char *creds) {
  char *home = env_pair("HOME", root);
  char *cache = g_build_filename(root, "cache", NULL);
  char *state = g_build_filename(root, "state", NULL);
  char *cache_e = env_pair("XDG_CACHE_HOME", cache);
  char *state_e = env_pair("XDG_STATE_HOME", state);
  char *cred = env_pair("DAILY_CALENDAR_CREDENTIALS", creds);
  const char *extra[] = {home, cache_e, state_e, cred, NULL};
  char **env = agenda_env(extra);
  g_free(home);
  g_free(cache);
  g_free(state);
  g_free(cache_e);
  g_free(state_e);
  g_free(cred);
  return env;
}

static void test_windows(void) {
  begin("one-off, zero duration, all-day, duration and overlap windows");
  char *events[] = {timed(NULL, NULL), g_strdup("UID:zero\nDTSTART:20261006T090000Z"),
                    g_strdup("UID:all\nDTSTART;VALUE=DATE:20261006"),
                    g_strdup("UID:duration\nDTSTART:20261006T090000Z\nDURATION:PT2H"),
                    g_strdup("UID:overnight\nDTSTART:20261005T230000Z\nDTEND:20261006T010000Z"),
                    g_strdup("UID:exclusive\nDTSTART:20261005T230000Z\nDTEND:20261006T000000Z"),
                    g_strdup("UID:tomorrow\nDTSTART:20261007T000000Z")};
  for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) {
    parity_only(events[i], NULL, 0);
    g_free(events[i]);
  }
}
static void test_updates(void) {
  begin("multiple, cancelled, duplicate UID/start and sequence updates");
  char *one = timed("STATUS:CANCELLED", NULL);
  char *base = timed(NULL, NULL);
  char *seq = timed("SEQUENCE:2\nSUMMARY:Updated", NULL);
  char *stamp = timed("DTSTAMP:20261005T090000Z\nSUMMARY:Updated", NULL);
  const char *pairs[][2] = {{one, NULL}, {base, base}, {base, seq}, {base, stamp}};
  for (size_t i = 0; i < 4; i++) {
    char *text = pairs[i][1] ? ics2(pairs[i][0], pairs[i][1]) : ics1(pairs[i][0]);
    yyjson_doc *doc = parity(text, strlen(text), NULL, 0);
    yyjson_doc_free(doc);
    g_free(text);
  }
  const char *dups[] = {"DTSTART:20261006T090000Z\nSUMMARY:No UID", "DTSTART:20261006T090000Z\nSUMMARY:Duplicate"};
  char *text = ics2(dups[0], dups[1]);
  yyjson_doc *doc = parity(text, strlen(text), NULL, 0);
  yyjson_doc_free(doc);
  g_free(text);
  g_free(one);
  g_free(base);
  g_free(seq);
  g_free(stamp);
}
static void test_rules(void) {
  begin("recurrence rules cover daily, weekly, monthly, yearly and positions");
  const char *rules[] = {"FREQ=DAILY;COUNT=9",
                         "FREQ=WEEKLY;BYDAY=TU,TH;COUNT=30",
                         "FREQ=MONTHLY;BYMONTHDAY=6;COUNT=40",
                         "FREQ=MONTHLY;BYDAY=TU;BYSETPOS=1;COUNT=30",
                         "FREQ=YEARLY;BYMONTH=10;BYMONTHDAY=6;COUNT=6",
                         "FREQ=DAILY;INTERVAL=2;UNTIL=20261006T090000Z",
                         "FREQ=HOURLY;COUNT=12",
                         "FREQ=MINUTELY;INTERVAL=20;COUNT=4"};
  for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++) {
    char *extra = g_strdup_printf("RRULE:%s", rules[i]);
    char *event = timed(extra, "20251006T090000Z");
    char *replaced = replace_one(event, "DTEND:20261006T100000Z", "DURATION:PT1H");
    parity_only(replaced, NULL, 0);
    g_free(replaced);
    g_free(event);
    g_free(extra);
  }
}
static void test_exdate(void) {
  begin("RDATE unions, EXDATE/EXRULE exclusions and periods");
  const char *extras[] = {"RDATE:20261006T090000Z,20261006T150000Z", "RDATE;TZID=America/New_York:20261006T150000",
                          "RRULE:FREQ=DAILY;COUNT=3\nEXDATE:20261006T090000Z",
                          "RRULE:FREQ=HOURLY;COUNT=6\nEXRULE:FREQ=HOURLY;INTERVAL=2;COUNT=3",
                          "RDATE;VALUE=PERIOD:20261006T150000Z/20261006T180000Z",
                          "RDATE;VALUE=PERIOD:20261006T150000Z/PT2H"};
  for (size_t i = 0; i < sizeof extras / sizeof extras[0]; i++) {
    char *event = timed(extras[i], NULL);
    parity_only(event, NULL, 0);
    g_free(event);
  }
}
static void test_mixed(void) {
  begin("mixed RDATE timezones, floating attachment and absolute duplicate starts");
  const char *master = "UID:mixed\nDTSTART;TZID=America/New_York:20261006T090000\nDURATION:PT1H";
  const char *extras[] = {"RDATE:20261006T150000", "RDATE:20261006T130000Z",
                          "RDATE;TZID=America/New_York;VALUE=PERIOD:20261006T150000/20261006T160000"};
  for (size_t i = 0; i < 3; i++) {
    char *event = g_strdup_printf("%s\n%s", master, extras[i]);
    parity_only(event, NULL, 0);
    g_free(event);
  }
  char *dup = g_strdup_printf("%s\nRDATE:20261006T130000Z", master);
  char *text = ics1(dup);
  yyjson_doc *doc = parity(text, strlen(text), NULL, 0);
  CHECK(doc && yyjson_arr_size(yyjson_doc_get_root(doc)) == 1);
  yyjson_doc_free(doc);
  g_free(text);
  g_free(dup);
  char *floated = replace_one(master, "DTSTART;TZID=America/New_York:", "DTSTART:");
  char *attached = g_strdup_printf("%s\nRDATE;TZID=America/New_York:20261006T150000", floated);
  parity_only(attached, NULL, 0);
  g_free(attached);
  g_free(floated);
  char *range = g_strdup_printf("%s\nRRULE:FREQ=DAILY;COUNT=4\nRDATE:20261008T150000Z", master);
  char *args[] = {"--parse-range", "2026-10-06", "2026-10-10", "UTC", NULL};
  text = ics1(range);
  doc = parity(text, strlen(text), args, 0);
  yyjson_doc_free(doc);
  g_free(text);
  g_free(range);
}
static void test_detached(void) {
  begin("detached recurrence moves, cancellation and THISANDFUTURE");
  char *master = timed("RRULE:FREQ=DAILY;COUNT=8", "20261004T090000Z");
  char *duration = replace_one(master, "DTEND:20261006T100000Z", "DURATION:PT1H");
  const char *overrides[] = {
      "UID:event\nRECURRENCE-ID:20261006T090000Z\nDTSTART:20261006T150000Z\nDTEND:20261006T170000Z\nSUMMARY:Moved",
      "UID:event\nRECURRENCE-ID:20261006T090000Z\nDTSTART:20261006T090000Z\nSTATUS:CANCELLED",
      "UID:event\nRECURRENCE-ID;RANGE=THISANDFUTURE:20261005T090000Z\nDTSTART:20261005T130000Z\nDURATION:PT2H\nSUMMARY:Shifted"};
  for (size_t i = 0; i < 3; i++) {
    char *text = ics2(duration, overrides[i]);
    yyjson_doc *doc = parity(text, strlen(text), NULL, 0);
    yyjson_doc_free(doc);
    g_free(text);
  }
  char *text = ics2(duration, "UID:event\nRECURRENCE-ID:20261010T090000Z\nDTSTART:20261006T160000Z\nDURATION:PT1H\nSUMMARY:Moved from future");
  yyjson_doc *doc = parity(text, strlen(text), NULL, 0);
  yyjson_doc_free(doc);
  g_free(text);
  g_free(duration);
  g_free(master);
}
static void test_aliases(void) {
  begin("recurrence aliases, excluded moves, collision precedence and range ordering");
  const char *master = "UID:a\nDTSTART:20261004T090000Z\nRRULE:FREQ=DAILY;COUNT=8\nSUMMARY:Original";
  const char *moved = "UID:a\nRECURRENCE-ID:20261006T090000Z\nDTSTART:20261007T090000Z\nSUMMARY:Moved";
  char *args[] = {"--parse-feed", "2026-10-07", "UTC", NULL};
  char *text = ics2(master, moved);
  yyjson_doc *doc = parity(text, strlen(text), args, 0);
  CHECK(doc && str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "summary"), "Original"));
  yyjson_doc_free(doc);
  g_free(text);
  char *shifted = replace_one(moved, "DTSTART:20261007T090000Z", "DTSTART:20261006T150000Z");
  char *range[] = {"--parse-range", "2026-10-04", "2026-10-09", "UTC", NULL};
  text = ics2(master, shifted);
  doc = parity(text, strlen(text), range, 0);
  yyjson_doc_free(doc);
  g_free(text);
  g_free(shifted);
  char *excluded = g_strdup_printf("%s\nEXDATE:20261006T090000Z", master);
  text = ics2(excluded, moved);
  doc = parity(text, strlen(text), args, 0);
  CHECK(doc && yyjson_arr_size(yyjson_doc_get_root(doc)) == 1 &&
        str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "summary"), "Original"));
  yyjson_doc_free(doc);
  g_free(text);
  g_free(excluded);
  char *named = replace_one(master, "DTSTART:20261004T090000Z", "DTSTART;TZID=America/New_York:20261004T090000");
  const char *alternate = "UID:a\nRECURRENCE-ID:20261006T130000Z\nDTSTART:20261006T150000Z\nSUMMARY:Moved";
  text = ics2(named, alternate);
  doc = parity(text, strlen(text), NULL, 0);
  CHECK(doc && yyjson_arr_size(yyjson_doc_get_root(doc)) == 1);
  yyjson_doc_free(doc);
  g_free(text);
  char *future = replace_one(alternate, "RECURRENCE-ID:", "RECURRENCE-ID;RANGE=THISANDFUTURE:");
  const char *days[] = {"2026-10-06", "2026-10-07"};
  for (size_t i = 0; i < 2; i++) {
    char *day_args[] = {"--parse-feed", (char *)days[i], "UTC", NULL};
    text = ics2(named, future);
    doc = parity(text, strlen(text), day_args, 0);
    yyjson_doc_free(doc);
    g_free(text);
  }
  g_free(future);
  g_free(named);
}
static void test_zones(void) {
  begin("floating, IANA, attachment zones, UTC and daylight transitions");
  char *floating = replace_all(timed(NULL, NULL), "Z", "");
  char *named = replace_all(replace_all(timed(NULL, NULL), "DTSTART:", "DTSTART;TZID=America/New_York:"), "DTEND:",
                            "DTEND;TZID=America/New_York:");
  char *named_nz = replace_all(named, "Z", "");
  g_free(named);
  const char *rows_event[] = {floating, floating, named_nz, "UID:fold\nDTSTART:20261101T013000\nDURATION:PT1H",
                              "UID:gap\nDTSTART:20260308T023000\nDURATION:PT1H",
                              "UID:dst\nDTSTART;TZID=Europe/London:20261024T093000\nDURATION:PT1H\nRRULE:FREQ=DAILY;COUNT=3"};
  const char *dates[] = {"2026-10-06", "2026-10-06", "2026-10-06", "2026-11-01", "2026-03-08", "2026-10-25"};
  const char *zones[] = {"Europe/London", "UTC", "UTC", "America/New_York", "America/New_York", "Europe/London"};
  const char *props[] = {"", "X-WR-TIMEZONE:America/New_York\r\n", "", "", "", ""};
  for (size_t i = 0; i < 6; i++) {
    char *args[] = {"--parse-feed", (char *)dates[i], (char *)zones[i], NULL};
    char *text = ics_raw(&rows_event[i], NULL, 1, props[i][0] ? props[i] : NULL);
    yyjson_doc *doc = parity(text, strlen(text), args, 0);
    yyjson_doc_free(doc);
    g_free(text);
  }
  g_free(floating);
  g_free(named_nz);
}
static void test_vtimezone(void) {
  begin("VTIMEZONE custom rules remain attached to the event");
  const char *tz = "BEGIN:VTIMEZONE\r\nTZID:Fixture/Custom\r\nBEGIN:STANDARD\r\nDTSTART:19700101T000000\r\nTZOFFSETFROM:+0230\r\nTZOFFSETTO:+0230\r\nTZNAME:FIX\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n";
  const char *event = "UID:custom\nDTSTART;TZID=Fixture/Custom:20261006T090000\nDURATION:PT1H";
  char *text = ics_raw(&event, NULL, 1, tz);
  yyjson_doc *doc = parity(text, strlen(text), NULL, 0);
  yyjson_doc_free(doc);
  g_free(text);
}
static void test_text(void) {
  begin("Unicode, folded/escaped text, safe links and range metadata");
  char *event = timed("LOCATION:Room\\, 2\nDESCRIPTION:Line 1\\nLine 2\nURL:https://calendar.google.com/event?eid=a", NULL);
  char *folded = replace_one(event, "SUMMARY:Example", "SUMMARY:🌞 Résumé\\, planner\\; \\nnext\n line");
  parity_only(folded, NULL, 0);
  char *args[] = {"--parse-range", "2026-10-01", "2026-10-10", "UTC", NULL};
  char *text = ics1(folded);
  yyjson_doc *doc = parity(text, strlen(text), args, 0);
  yyjson_doc_free(doc);
  g_free(text);
  const char *links[] = {"https://calendar.google.com/event?eid=x",
                         "https://calendar.google.com/calendar/ical/a/private-secret/basic.ics",
                         "http://calendar.google.com/event", "https://example.com/"};
  for (size_t i = 0; i < 4; i++) {
    char *extra = g_strdup_printf("URL:%s", links[i]);
    char *item = timed(extra, NULL);
    parity_only(item, NULL, 0);
    g_free(item);
    g_free(extra);
  }
  g_free(folded);
  g_free(event);
}
static void test_malformed(void) {
  begin("malformed components, windows and untrusted input are rejected");
  const char *events[] = {"SUMMARY:Missing start", "DTSTART:garbage", "DTSTART:20260230T090000Z",
                          "DTSTART:20261006T090000Z\nRRULE:COUNT=3", "DTSTART:20261006T090000Z\nRRULE:FREQ=BOGUS",
                          "DTSTART:20261006T090000Z\nEXDATE:garbage",
                          "DTSTART;VALUE=DATE:20261006\nDTEND;VALUE=DATE:20261006"};
  for (size_t i = 0; i < sizeof events / sizeof events[0]; i++)
    parity_only(events[i], NULL, 1);
  const char *raws[] = {"garbage", "BEGIN:VCALENDAR\nVERSION:2.0\nBEGIN:VEVENT\nDTSTART:20261006T090000Z",
                        "BEGIN:VEVENT\nDTSTART:20261006T090000Z\nEND:VEVENT"};
  for (size_t i = 0; i < 3; i++) {
    yyjson_doc *doc = parity(raws[i], strlen(raws[i]), NULL, 1);
    yyjson_doc_free(doc);
  }
  const char *bad[][5] = {{"--parse-feed", "2026-10-06", "Bogus/Zone", NULL},
                          {"--parse-feed", "2026-10-06", "local", NULL},
                          {"--parse-feed", "2026-02-30", "UTC", NULL},
                          {"--parse-range", "2026-10-06", "2026-10-06", "UTC", NULL},
                          {"--parse-range", "2026-01-01", "2026-04-02", "UTC", NULL}};
  char *good = timed(NULL, NULL);
  char *text = ics1(good);
  for (size_t i = 0; i < 5; i++) {
    char *args[5];
    for (int k = 0; k < 5; k++)
      args[k] = (char *)bad[i][k];
    yyjson_doc *doc = parity(text, strlen(text), args, 1);
    yyjson_doc_free(doc);
  }
  char *huge = g_malloc(2000001);
  memset(huge, 65, 2000001);
  yyjson_doc *doc = parity(huge, 2000001, NULL, 1);
  yyjson_doc_free(doc);
  g_free(huge);
  g_free(text);
  g_free(good);
}
static void test_bounds(void) {
  begin("occurrence bounds and bar range bounds are independent");
  const char *event = "UID:many\nDTSTART:20261006T000000Z\nRRULE:FREQ=MINUTELY;COUNT=501";
  parity_only(event, NULL, 1);
  char *args[] = {"--parse-range", "2026-10-06", "2026-10-07", "UTC", NULL};
  char *text = ics1(event);
  yyjson_doc *doc = parity(text, strlen(text), args, 0);
  yyjson_doc_free(doc);
  g_free(text);
}
static int fixture_status(char *const *args) {
  Proc p = run_at(fixture, args, "", 0, NULL, 70000);
  int status = p.error ? -1 : p.status;
  proc_clear(&p);
  return status;
}
static void test_credentials(void) {
  begin("credential grammar, naming, permissions, symlink targets and BOMs");
  char *root = scratch();
  CHECK(root);
  char *path = g_build_filename(root, "secret", NULL);
  char *alias = g_build_filename(root, "alias", NULL);
  GString *bom = g_string_new(NULL);
  g_string_append_len(bom, "\xef\xbb\xbf ", 4);
  g_string_append(bom, feed);
  g_string_append_c(bom, '\n');
  CHECK(write_file(path, bom->str, bom->len, 0600));
  g_string_free(bom, TRUE);
  char *feeds[] = {"feeds", path, NULL};
  if (fixture_status(feeds) != 0)
    FAIL("bom feeds");
  if (symlink(path, alias) != 0)
    FAIL("symlink");
  char *alias_args[] = {"feeds", alias, NULL};
  if (fixture_status(alias_args) != 0)
    FAIL("alias feeds");
  if (chmod(path, 0644) != 0 || fixture_status(feeds) != 1)
    FAIL("loose permissions");
  chmod(path, 0600);
  yyjson_mut_doc *docs[6];
  int expect[6] = {0, 1, 1, 1, 1, 1};
  docs[0] = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *o = yyjson_mut_obj(docs[0]);
  yyjson_mut_doc_set_root(docs[0], o);
  yyjson_mut_obj_add_strcpy(docs[0], o, "Work", feed);
  yyjson_mut_obj_add_strcpy(docs[0], o, "Family", feed);
  docs[1] = yyjson_mut_doc_new(NULL);
  o = yyjson_mut_obj(docs[1]);
  yyjson_mut_doc_set_root(docs[1], o);
  yyjson_mut_obj_add_strcpy(docs[1], o, "", feed);
  docs[2] = yyjson_mut_doc_new(NULL);
  o = yyjson_mut_obj(docs[2]);
  yyjson_mut_doc_set_root(docs[2], o);
  char *long_key = g_strnfill(41, 'a');
  yyjson_mut_obj_add_strcpy(docs[2], o, long_key, feed);
  g_free(long_key);
  docs[3] = yyjson_mut_doc_new(NULL);
  o = yyjson_mut_obj(docs[3]);
  yyjson_mut_doc_set_root(docs[3], o);
  yyjson_mut_obj_add_strcpy(docs[3], o, "bad\x1b", feed);
  docs[4] = yyjson_mut_doc_new(NULL);
  o = yyjson_mut_obj(docs[4]);
  yyjson_mut_doc_set_root(docs[4], o);
  char *http = replace_one(feed, "https:", "http:");
  yyjson_mut_obj_add_strcpy(docs[4], o, "Work", http);
  g_free(http);
  docs[5] = yyjson_mut_doc_new(NULL);
  o = yyjson_mut_obj(docs[5]);
  yyjson_mut_doc_set_root(docs[5], o);
  yyjson_mut_obj_add_strcpy(docs[5], o, "Work", feed);
  for (int i = 0; i < 8; i++) {
    char key[8];
    snprintf(key, sizeof key, "C%d", i);
    yyjson_mut_obj_add_strcpy(docs[5], o, key, feed);
  }
  for (int i = 0; i < 6; i++) {
    size_t n = 0;
    char *json = yyjson_mut_write(docs[i], 0, &n);
    CHECK(write_file(path, json, n, 0600));
    free(json);
    yyjson_mut_doc_free(docs[i]);
    if (fixture_status(feeds) != expect[i])
      FAIL("json credentials %d", i);
  }
  char *hash = replace_one(feed, "user%40example.com", "a%23b");
  char *ok_args[] = {"url", (char *)feed, NULL};
  char *hash_args[] = {"url", hash, NULL};
  if (fixture_status(ok_args) != 0 || fixture_status(hash_args) != 0)
    FAIL("accepted url");
  g_free(hash);
  char *bad_urls[7];
  bad_urls[0] = g_strdup_printf("%s?x", feed);
  bad_urls[1] = g_strdup_printf("%s#x", feed);
  bad_urls[2] = replace_one(feed, "calendar.google.com", "calendar.google.com.evil");
  bad_urls[3] = replace_one(feed, "user%40example.com", "a%2fb");
  bad_urls[4] = replace_one(feed, "user%40example.com", "a%2540b");
  bad_urls[5] = replace_one(feed, "user%40example.com", "..");
  bad_urls[6] = replace_one(feed, "calendar.google.com", "calendar.google.com:443");
  for (int i = 0; i < 7; i++) {
    char *args[] = {"url", bad_urls[i], NULL};
    if (fixture_status(args) != 1)
      FAIL("rejected url %d", i);
    g_free(bad_urls[i]);
  }
  g_free(path);
  g_free(alias);
  rm_rf(root);
  g_free(root);
}
static void test_whitespace(void) {
  begin("credential files strip every Python whitespace character before feed validation");
  char *root = scratch();
  CHECK(root);
  char *path = g_build_filename(root, "secret", NULL);
  char **env = home_env(root, path);
  for (size_t i = 0; i < sizeof spaces / sizeof spaces[0]; i++) {
    char enc[4];
    size_t n = utf8(spaces[i], enc);
    GString *body = g_string_new(NULL);
    g_string_append_len(body, enc, n);
    g_string_append(body, feed);
    g_string_append_len(body, enc, n);
    CHECK(write_file(path, body->str, body->len, 0600));
    g_string_free(body, TRUE);
    char *args[] = {"feeds", path, NULL};
    Proc feeds = run_at(fixture, args, "", 0, NULL, 70000);
    if (feeds.error || feeds.status != 0)
      FAIL("feeds %u %s", spaces[i], feeds.err ? feeds.err : "");
    proc_clear(&feeds);
    char *status_args[] = {"--status", NULL};
    Proc native = run_bin(status_args, "", 0, (const char *const *)env);
    if (native.error || native.status != 0 || !contains(native.out, native.out_len, "Calendar feed: present"))
      FAIL("status %u %s", spaces[i], native.err ? native.err : "");
    if (legacy) {
      Proc old = run_at(legacy, status_args, "", 0, (const char *const *)env, 70000);
      if (old.out_len != native.out_len || memcmp(old.out ? old.out : "", native.out ? native.out : "", native.out_len))
        FAIL("legacy status %u", spaces[i]);
      proc_clear(&old);
    }
    proc_clear(&native);
  }
  env_free(env);
  g_free(path);
  rm_rf(root);
  g_free(root);
}
static void test_notes(void) {
  begin("notes sections, unchecked boxes, fences, deduplication and Unicode lines");
  char *root = scratch();
  CHECK(root);
  char *path = g_build_filename(root, "notes", NULL);
  GString *body = g_string_new("# Reminders\r\n- First\n- [ ] Work\n- [x] Done\n```\n- hidden\n```\n# Other\n- Not a reminder\n- [ ] Everywhere\n# Todos\n+ First");
  g_string_append_unichar(body, 0x2028);
  g_string_append(body, "* Café\n- ");
  g_string_append_c(body, 0x1b);
  g_string_append(body, "[31mDanger");
  g_string_append_len(body, "", 1);
  g_string_append(body, "here\n");
  CHECK(write_file(path, body->str, body->len, 0600));
  g_string_free(body, TRUE);
  char *args[] = {"notes", path, NULL};
  Proc p = run_at(fixture, args, "", 0, NULL, 70000);
  CHECK(!p.error && p.status == 0);
  yyjson_doc *doc = yyjson_read(p.out, p.out_len, 0);
  CHECK(doc);
  const char *rows[] = {"First", "Work", "Everywhere", "Café", "[31mDangerhere"};
  yyjson_val *arr = yyjson_obj_get(yyjson_doc_get_root(doc), "rows");
  if (!arr || yyjson_arr_size(arr) != 5)
    FAIL("rows");
  for (size_t i = 0; i < 5 && arr && i < yyjson_arr_size(arr); i++)
    if (!str_eq(yyjson_arr_get(arr, i), rows[i]))
      FAIL("row %zu", i);
  yyjson_doc_free(doc);
  proc_clear(&p);
  CHECK(write_file(path, "\xff", 1, 0600));
  p = run_at(fixture, args, "", 0, NULL, 70000);
  if (p.error || p.status != 1)
    FAIL("invalid notes");
  proc_clear(&p);
  GString *huge = g_string_new("# Reminders\n- ");
  const char emoji[] = "\xf0\x9f\x98\x83";
  for (int i = 0; i < 550000; i++)
    g_string_append_len(huge, emoji, 4);
  CHECK(write_file(path, huge->str, huge->len, 0600));
  g_string_free(huge, TRUE);
  p = run_at(fixture, args, "", 0, NULL, 70000);
  if (p.error || p.status != 0)
    FAIL("large notes %s", p.err ? p.err : "");
  proc_clear(&p);
  g_free(path);
  rm_rf(root);
  g_free(root);
}
static void test_cache_display(void) {
  begin("cache-only display and status match legacy without external reads");
  char *root = scratch();
  CHECK(root);
  char today[16], tomorrow[16], stamp[40];
  utc_day(today, sizeof today, 0);
  utc_day(tomorrow, sizeof tomorrow, 1);
  utc_stamp(stamp, sizeof stamp);
  char *notes = g_build_filename(root, "Notes", today, NULL);
  g_mkdir_with_parents(notes, 0700);
  char *note = g_build_filename(notes, "today.md", NULL);
  GString *note_body = g_string_new("# Reminders\n- Brief task\n");
  for (int i = 0; i < 4; i++)
    g_string_append(note_body, "- [ ] A longer half-hyphenated reminder ");
  CHECK(write_file(note, note_body->str, note_body->len, 0600));
  g_string_free(note_body, TRUE);
  char *missing = g_build_filename(root, "missing", NULL);
  char **env = home_env(root, missing);
  char *cache = g_build_filename(root, "cache", "daily-agenda", "calendar.json", NULL);
  g_mkdir_with_parents(g_path_get_dirname(cache), 0700);
  const char *kinds[] = {"missing", "old", "empty", "event", "bad"};
  for (size_t k = 0; k < 5; k++) {
    if (!strcmp(kinds[k], "missing"))
      unlink(cache);
    else {
      char *json = NULL;
      if (!strcmp(kinds[k], "old"))
        json = g_strdup("{\"day\":\"2000-01-01\"}");
      else if (!strcmp(kinds[k], "empty"))
        json = g_strdup_printf("{\"day\":\"%s\",\"fetched_at\":\"%s\",\"events\":[]}", today, stamp);
      else if (!strcmp(kinds[k], "event"))
        json = g_strdup_printf(
            "{\"day\":\"%s\",\"fetched_at\":\"%sT10:00:00+00:00\",\"events\":[{\"summary\":\"Demo\\u001b[31m\",\"start\":{\"date\":\"%s\"},\"end\":{\"date\":\"%s\"},\"calendar\":\"Work\"}]}",
            today, today, today, tomorrow);
      else
        json = g_strdup_printf("{\"day\":\"%s\",\"fetched_at\":\"%s\",\"events\":\"bad\"}", today, stamp);
      CHECK(write_file(cache, json, strlen(json), 0600));
      g_free(json);
    }
    char *sets[3][3] = {{NULL}, {"--status", NULL}, {"--refresh", "--status", NULL}};
    for (int a = 0; a < 3; a++) {
      Proc n = run_bin(sets[a][0] ? sets[a] : NULL, "", 0, (const char *const *)env);
      if (n.error || n.status != 0)
        FAIL("display %s %d", kinds[k], n.status);
      if (legacy) {
        Proc old = run_at(legacy, sets[a][0] ? sets[a] : NULL, "", 0, (const char *const *)env, 70000);
        if (old.status != n.status || old.out_len != n.out_len ||
            memcmp(old.out ? old.out : "", n.out ? n.out : "", n.out_len))
          FAIL("legacy display %s", kinds[k]);
        proc_clear(&old);
      }
      proc_clear(&n);
    }
  }
  g_free(cache);
  g_free(missing);
  g_free(note);
  g_free(notes);
  env_free(env);
  rm_rf(root);
  g_free(root);
}
static void test_save(void) {
  begin("safe private state publication rejects leaf/parent links");
  char *root = scratch();
  CHECK(root);
  char *file = g_build_filename(root, "state", "calendar.json", NULL);
  char *args[] = {"save", file, NULL};
  Proc p = run_at(fixture, args, "{\"version\":1}", 13, NULL, 70000);
  if (p.error || p.status != 0)
    FAIL("save %s", p.err ? p.err : "");
  proc_clear(&p);
  struct stat st;
  char *dir = g_build_filename(root, "state", NULL);
  if (stat(dir, &st) != 0 || (st.st_mode & 0777) != 0700)
    FAIL("dir mode");
  if (stat(file, &st) != 0 || (st.st_mode & 0777) != 0600)
    FAIL("file mode");
  size_t n = 0;
  char *text = read_file(file, &n);
  if (!text || n != strlen("{\"version\": 1}") || memcmp(text, "{\"version\": 1}", n))
    FAIL("saved text");
  g_free(text);
  unlink(file);
  char *target = g_build_filename(root, "target", NULL);
  if (symlink(target, file) != 0)
    FAIL("leaf symlink");
  p = run_at(fixture, args, "{}", 2, NULL, 70000);
  if (p.error || p.status != 1)
    FAIL("leaf accepted");
  proc_clear(&p);
  unlink(file);
  rm_rf(dir);
  if (mkdir(target, 0700) != 0 || symlink(target, dir) != 0)
    FAIL("parent symlink");
  p = run_at(fixture, args, "{}", 2, NULL, 70000);
  if (p.error || p.status != 1)
    FAIL("parent accepted");
  proc_clear(&p);
  g_free(target);
  g_free(dir);
  g_free(file);
  rm_rf(root);
  g_free(root);
}
static void test_bar(void) {
  begin("bar export identifiers, colors, spanning days, meeting hosts and redaction");
  const char *json =
      "[{\"uid\":\"event\",\"summary\":\"Title fixture_token_0123456789\",\"start\":{\"dateTime\":\"2026-10-06T22:00:00+00:00\"},\"end\":{\"dateTime\":\"2026-10-08T00:00:00+00:00\"},\"location\":\"Room https://meet.google.com/abc-defg-hij).\",\"description\":\"";
  GString *body = g_string_new(json);
  g_string_append(body, feed);
  g_string_append(body, "\",\"url\":\"https://calendar.google.com/event?eid=x\"}]");
  char *args[] = {"bar", "Work", "UTC", (char *)feed, NULL};
  Proc p = run_at(fixture, args, body->str, body->len, NULL, 70000);
  CHECK(!p.error && p.status == 0);
  if (contains(p.out, p.out_len, "fixture_token_"))
    FAIL("token leaked");
  yyjson_doc *doc = yyjson_read(p.out, p.out_len, 0);
  CHECK(doc && yyjson_arr_size(yyjson_doc_get_root(doc)) == 2);
  yyjson_val *row = yyjson_arr_get(yyjson_doc_get_root(doc), 0);
  if (!str_eq(yyjson_obj_get(row, "dateKey"), "2026-10-06") ||
      !str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 1), "dateKey"), "2026-10-07") ||
      !str_eq(yyjson_obj_get(row, "title"), "Title [redacted]") ||
      !str_eq(yyjson_obj_get(row, "meetingUrl"), "https://meet.google.com/abc-defg-hij"))
    FAIL("bar fields");
  yyjson_val *id = yyjson_obj_get(row, "id");
  if (!id || yyjson_get_len(id) != 20)
    FAIL("id length");
  for (size_t i = 0; id && i < yyjson_get_len(id); i++) {
    char c = yyjson_get_str(id)[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      FAIL("id hex");
  }
  yyjson_doc_free(doc);
  proc_clear(&p);
  g_string_free(body, TRUE);
}
static void test_expand(void) {
  begin("sandboxed child parsing preserves limits and narrowed environment");
  const char *child = getenv("DAILY_AGENDA_CHILD");
  if (!child || !*child)
    child = binary;
  char *event = timed(NULL, NULL);
  char *text = ics1(event);
  char *args[] = {"expand", (char *)child, "2026-10-06", "2026-10-07", "UTC", NULL};
  Proc p = run_at(fixture, args, text, strlen(text), NULL, 70000);
  CHECK(!p.error && p.status == 0);
  yyjson_doc *doc = yyjson_read(p.out, p.out_len, 0);
  CHECK(doc && str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(doc), 0), "summary"), "Example"));
  yyjson_doc_free(doc);
  proc_clear(&p);
  g_free(text);
  g_free(event);
  CHECK(limits_bin);
  char *limit_args[] = {"expand", limits_bin, "2026-10-06", "2026-10-07", "UTC", NULL};
  const char *extra[] = {"AGENDA_PARENT_SECRET=not inherited", "LD_PRELOAD=", NULL};
  p = run_at(fixture, limit_args, "", 0, extra, 70000);
  CHECK(!p.error && p.status == 0);
  doc = yyjson_read(p.out, p.out_len, 0);
  CHECK(doc);
  yyjson_val *row = yyjson_arr_get(yyjson_doc_get_root(doc), 0);
  if (yyjson_get_sint(yyjson_obj_get(row, "cpu")) != 30 || yyjson_get_sint(yyjson_obj_get(row, "as")) != 1073741824 ||
      yyjson_get_sint(yyjson_obj_get(row, "file")) != 2000000 || yyjson_get_sint(yyjson_obj_get(row, "core")) != 0 ||
      yyjson_get_bool(yyjson_obj_get(row, "secret")) || yyjson_get_bool(yyjson_obj_get(row, "preload")) ||
      !str_eq(yyjson_obj_get(row, "lang"), "C.UTF-8") || !str_eq(yyjson_obj_get(row, "tz"), "UTC"))
    FAIL("limits");
  yyjson_doc_free(doc);
  proc_clear(&p);
}
static void test_iso(void) {
  begin("ISO week/basic dates and rejected noncanonical date syntax");
  char *event = timed(NULL, NULL);
  char *text = ics1(event);
  const char *good[] = {"20261006", "2026-W41-2", "2026W412"};
  for (size_t i = 0; i < 3; i++) {
    char *args[] = {"--parse-feed", (char *)good[i], "UTC", NULL};
    yyjson_doc *doc = parity(text, strlen(text), args, 0);
    yyjson_doc_free(doc);
  }
  const char *bad[] = {"2026-1-6", "2026-10-6", "2026--W41-2", "2026-W54-2"};
  for (size_t i = 0; i < 4; i++) {
    char *args[] = {"--parse-feed", (char *)bad[i], "UTC", NULL};
    yyjson_doc *doc = parity(text, strlen(text), args, 1);
    yyjson_doc_free(doc);
  }
  g_free(text);
  g_free(event);
}
static void test_untrusted(void) {
  begin("NUL/invalid UTF-8 text and category-C characters remain untrusted data");
  GString *event = g_string_new("UID:event\nDTSTART:20261006T090000Z\nDTEND:20261006T100000Z\nSUMMARY:A");
  g_string_append_c(event, 0);
  g_string_append(event, "B");
  g_string_append_c(event, 0x1b);
  g_string_append(event, "[31m");
  g_string_append_unichar(event, 0x200d);
  g_string_append_unichar(event, 0xe000);
  size_t len = event->len;
  char *text = ics_raw((const char *const *)&event->str, &len, 1, NULL);
  yyjson_doc *doc = parity(text, strlen(text), NULL, 0);
  yyjson_doc_free(doc);
  g_free(text);
  g_string_free(event, TRUE);
  char *plain = timed(NULL, NULL);
  char *replaced = replace_one(plain, "Example", "PLACEHOLDER");
  text = ics1(replaced);
  char *at = strstr(text, "PLACEHOLDER");
  CHECK(at);
  *at = (char)0xff;
  doc = parity(text, strlen(text), NULL, 0);
  yyjson_doc_free(doc);
  g_free(text);
  g_free(replaced);
  g_free(plain);
}
static void test_long(void) {
  begin("long durations and mixed end timezones preserve construction");
  parity_only("UID:long\nDTSTART:19000101T090000Z\nDTEND:22000101T100000Z", NULL, 0);
  parity_only("UID:mixed\nDTSTART;TZID=America/New_York:20261006T090000\nDTEND:20261006T140000Z", NULL, 0);
}
static void test_sequences(void) {
  begin("override sequences, active overrides of cancellation and future shifts");
  char *master = timed("RRULE:FREQ=DAILY;COUNT=8\nSEQUENCE:3", "20261004T090000Z");
  char *duration = replace_one(master, "DTEND:20261006T100000Z", "DURATION:PT1H");
  char *text = ics2(duration, "UID:event\nRECURRENCE-ID:20261006T090000Z\nDTSTART:20261006T150000Z\nSEQUENCE:2\nSUMMARY:Obsolete");
  yyjson_doc *doc = parity(text, strlen(text), NULL, 0);
  yyjson_doc_free(doc);
  g_free(text);
  char *cancelled = g_strdup_printf("%s\nSTATUS:CANCELLED", duration);
  text = ics2(cancelled, "UID:event\nRECURRENCE-ID;RANGE=THISANDFUTURE:20261005T090000Z\nDTSTART:20261005T130000Z\nSEQUENCE:4\nSUMMARY:Active");
  doc = parity(text, strlen(text), NULL, 0);
  yyjson_doc_free(doc);
  g_free(text);
  g_free(cancelled);
  text = ics2("UID:future\nDTSTART:21261005T090000Z\nRRULE:FREQ=DAILY;COUNT=3",
              "UID:future\nRECURRENCE-ID;RANGE=THISANDFUTURE:21261005T090000Z\nDTSTART:20261005T090000Z\nSUMMARY:Earlier");
  doc = parity(text, strlen(text), NULL, 0);
  yyjson_doc_free(doc);
  g_free(text);
  g_free(duration);
  g_free(master);
}
static void test_schema(void) {
  begin("cache schema, embedded NULs and duplicate key semantics match legacy");
  char *root = scratch();
  CHECK(root);
  char today[16], stamp[40];
  utc_day(today, sizeof today, 0);
  utc_stamp(stamp, sizeof stamp);
  char *missing = g_build_filename(root, "missing", NULL);
  char **env = home_env(root, missing);
  char *path = g_build_filename(root, "cache", "daily-agenda", "calendar.json", NULL);
  const char *events[] = {
      "{\"summary\":\"A\\u0000B\",\"calendar\":\"W\\u0000ork\",\"start\":{\"date\":\"%s\"}}",
      "{\"summary\":\"x\",\"start\":{\"dateTime\":\"%sT09:00:00Z\"},\"end\":{\"dateTime\":\"%sT08:00:00Z\"}}",
      "{\"summary\":\"x\",\"start\":{\"date\":\"%s\"},\"end\":{\"date\":\"%s\"}}",
      "{\"summary\":\"x\",\"start\":{\"dateTime\":\"%sT09:00:00\"}}", "{\"summary\":5,\"start\":{\"date\":\"%s\"}}"};
  for (size_t i = 0; i < 5; i++) {
    char item[512];
    if (i == 1 || i == 2)
      snprintf(item, sizeof item, events[i], today, today);
    else
      snprintf(item, sizeof item, events[i], today);
    char *json = g_strdup_printf("{\"day\":\"%s\",\"fetched_at\":\"%s\",\"events\":[%s]}", today, stamp, item);
    CHECK(write_file(path, json, strlen(json), 0600));
    g_free(json);
    Proc n = run_bin(NULL, "", 0, (const char *const *)env);
    if (n.error)
      FAIL("schema spawn");
    if (legacy) {
      Proc old = run_at(legacy, NULL, "", 0, (const char *const *)env, 70000);
      if (old.out_len != n.out_len || memcmp(old.out ? old.out : "", n.out ? n.out : "", n.out_len))
        FAIL("legacy schema");
      proc_clear(&old);
    }
    proc_clear(&n);
  }
  char *evil = g_strdup_printf(
      "{\"day\":\"%s\",\"fetched_at\":\"%s\",\"events\":[{\"summary\":\"Key\",\"start\\u0000evil\":{\"date\":\"%s\"}}]}",
      today, stamp, today);
  CHECK(write_file(path, evil, strlen(evil), 0600));
  g_free(evil);
  Proc n = run_bin(NULL, "", 0, (const char *const *)env);
  if (!contains(n.out, n.out_len, "Calendar cache unavailable"))
    FAIL("embedded key");
  if (legacy) {
    Proc old = run_at(legacy, NULL, "", 0, (const char *const *)env, 70000);
    if (old.out_len != n.out_len || memcmp(old.out ? old.out : "", n.out ? n.out : "", n.out_len))
      FAIL("legacy embedded key");
    proc_clear(&old);
  }
  proc_clear(&n);
  g_free(path);
  g_free(missing);
  env_free(env);
  rm_rf(root);
  g_free(root);
}
static void test_tz(void) {
  begin("unrecognised TZ falls back to the system timezone");
  char *root = scratch();
  CHECK(root);
  char *missing = g_build_filename(root, "missing", NULL);
  char **base = home_env(root, missing);
  size_t n = 0;
  while (base[n])
    n++;
  char **env = calloc(n + 2, sizeof *env);
  for (size_t i = 0; i < n; i++)
    env[i] = g_strdup(strncmp(base[i], "TZ=", 3) ? base[i] : "TZ=Not/AZone");
  bool saw = false;
  for (size_t i = 0; env[i]; i++)
    if (!strncmp(env[i], "TZ=", 3))
      saw = true;
  if (!saw)
    env[n] = g_strdup("TZ=Not/AZone");
  char *args[] = {"--status", NULL};
  Proc native = run_bin(args, "", 0, (const char *const *)env);
  if (native.error || native.status != 0)
    FAIL("status");
  if (legacy) {
    Proc old = run_at(legacy, args, "", 0, (const char *const *)env, 70000);
    if (old.out_len != native.out_len || memcmp(old.out ? old.out : "", native.out ? native.out : "", native.out_len))
      FAIL("legacy tz");
    proc_clear(&old);
  }
  proc_clear(&native);
  env_free(env);
  env_free(base);
  g_free(missing);
  rm_rf(root);
  g_free(root);
}
static void test_pty(void) {
  begin("pretty terminal output matches legacy without live calendar access");
  CHECK(pty_bin);
  char *root = scratch();
  CHECK(root);
  char today[16];
  utc_day(today, sizeof today, 0);
  char *dir = g_build_filename(root, "Notes", today, NULL);
  g_mkdir_with_parents(dir, 0700);
  char *note = g_build_filename(dir, "today.md", NULL);
  const char *body = "# Reminders\n- Pretty résumé with a hyphenated-word\n";
  CHECK(write_file(note, body, strlen(body), 0600));
  char *missing = g_build_filename(root, "missing", NULL);
  char **env = home_env(root, missing);
  char *args[] = {binary, NULL};
  Proc n = run_at(pty_bin, args, "", 0, (const char *const *)env, 70000);
  if (n.error || n.status != 0 || !contains(n.out, n.out_len, "\033[1;36mTODAY"))
    FAIL("pty %s", n.err ? n.err : "");
  if (legacy) {
    char *old_args[] = {legacy, NULL};
    Proc old = run_at(pty_bin, old_args, "", 0, (const char *const *)env, 70000);
    if (old.out_len != n.out_len || memcmp(old.out ? old.out : "", n.out ? n.out : "", n.out_len))
      FAIL("legacy pty");
    proc_clear(&old);
  }
  proc_clear(&n);
  env_free(env);
  g_free(missing);
  g_free(note);
  g_free(dir);
  rm_rf(root);
  g_free(root);
}

typedef enum { MODE_GOOD, MODE_REDIRECT, MODE_STATUS, MODE_SIZE, MODE_TYPE, MODE_OVERSTATE, MODE_PROGRESS, MODE_BAR, MODE_SLOW } Mode;
typedef struct {
  char *method, *url, *host, *accept, *encoding, *authorization, *cookie;
} Req;
typedef struct {
  Mode mode;
  int fd, port;
  SSL_CTX *ctx;
  GThread *thread;
  volatile int stop, ready;
  int calls;
  GMutex mu;
  Req *reqs;
  size_t nreqs, cap;
  char *calendar;
  size_t calendar_len;
} Server;
static bool ssl_write_all(SSL *ssl, const void *data, size_t n) {
  const char *p = data;
  while (n) {
    int chunk = n > 16384 ? 16384 : (int)n;
    int w = SSL_write(ssl, p, chunk);
    if (w <= 0)
      return false;
    p += w;
    n -= (size_t)w;
  }
  return true;
}
static bool send_headers(SSL *ssl, const char *status, const char *extra, const void *body, size_t blen) {
  GString *hdr = g_string_new("HTTP/1.1 ");
  g_string_append(hdr, status);
  g_string_append(hdr, "\r\n");
  if (extra)
    g_string_append(hdr, extra);
  g_string_append(hdr, "Connection: close\r\n\r\n");
  bool ok = ssl_write_all(ssl, hdr->str, hdr->len);
  g_string_free(hdr, TRUE);
  if (ok && body && blen)
    ok = ssl_write_all(ssl, body, blen);
  return ok;
}
static bool send_chunk(SSL *ssl, const void *data, size_t n) {
  char hdr[32];
  int h = snprintf(hdr, sizeof hdr, "%zx\r\n", n);
  return h > 0 && ssl_write_all(ssl, hdr, (size_t)h) && (n == 0 || ssl_write_all(ssl, data, n)) &&
         ssl_write_all(ssl, "\r\n", 2);
}
static void record_req(Server *s, Req req) {
  g_mutex_lock(&s->mu);
  if (s->nreqs == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 4;
    s->reqs = realloc(s->reqs, s->cap * sizeof *s->reqs);
  }
  s->reqs[s->nreqs++] = req;
  g_mutex_unlock(&s->mu);
}
static void respond(Server *s, SSL *ssl, int call) {
  if (s->mode == MODE_BAR && call % 2 == 0) {
    send_headers(ssl, "500 Internal Server Error", "Content-Length: 0\r\n", NULL, 0);
    return;
  }
  if (s->mode == MODE_REDIRECT) {
    send_headers(ssl, "302 Found", "Location: https://example.com/blocked\r\nContent-Length: 0\r\n", NULL, 0);
    return;
  }
  if (s->mode == MODE_STATUS) {
    const char *body = "No access fixture_token_0123456789";
    char extra[64];
    snprintf(extra, sizeof extra, "Content-Length: %zu\r\n", strlen(body));
    send_headers(ssl, "403 Forbidden", extra, body, strlen(body));
    return;
  }
  if (s->mode == MODE_SIZE) {
    send_headers(ssl, "200 OK", "Content-Length: 2000001\r\n", NULL, 0);
    return;
  }
  if (s->mode == MODE_TYPE) {
    send_headers(ssl, "200 OK", "Content-Length: 16\r\n", "not an iCalendar", 16);
    return;
  }
  if (s->mode == MODE_OVERSTATE) {
    char extra[64];
    snprintf(extra, sizeof extra, "Content-Length: %zu\r\n", s->calendar_len + 100);
    send_headers(ssl, "200 OK", extra, s->calendar, s->calendar_len);
    return;
  }
  if (s->mode == MODE_PROGRESS || s->mode == MODE_SLOW) {
    if (!ssl_write_all(ssl, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n", 70))
      return;
    if (s->mode == MODE_SLOW) {
      size_t first = s->calendar_len < 50 ? s->calendar_len : 50;
      send_chunk(ssl, s->calendar, first);
      g_usleep(21000000);
      if (s->calendar_len > first)
        send_chunk(ssl, s->calendar + first, s->calendar_len - first);
      send_chunk(ssl, "", 0);
      return;
    }
    size_t step = (s->calendar_len + 4) / 5;
    size_t offset = 0;
    size_t first = step < s->calendar_len ? step : s->calendar_len;
    if (!send_chunk(ssl, s->calendar, first))
      return;
    offset = first;
    while (offset < s->calendar_len) {
      g_usleep(5100000);
      if (offset + step >= s->calendar_len) {
        send_chunk(ssl, s->calendar + offset, s->calendar_len - offset);
        break;
      }
      if (!send_chunk(ssl, s->calendar + offset, step))
        return;
      offset += step;
    }
    send_chunk(ssl, "", 0);
    return;
  }
  char extra[96];
  snprintf(extra, sizeof extra, "Content-Type: text/calendar\r\nContent-Length: %zu\r\n", s->calendar_len);
  send_headers(ssl, "200 OK", extra, s->calendar, s->calendar_len);
}
static void handle_client(Server *s, int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  SSL *ssl = SSL_new(s->ctx);
  SSL_set_fd(ssl, fd);
  if (SSL_accept(ssl) != 1) {
    SSL_free(ssl);
    close(fd);
    return;
  }
  GString *req = g_string_new(NULL);
  char buf[2048];
  while (!strstr(req->str, "\r\n\r\n") && req->len < 65536) {
    int n = SSL_read(ssl, buf, sizeof buf);
    if (n <= 0)
      break;
    g_string_append_len(req, buf, (gssize)n);
  }
  Req rec = {0};
  char *line_end = strstr(req->str, "\r\n");
  if (line_end) {
    char *sp = memchr(req->str, ' ', (size_t)(line_end - req->str));
    if (sp) {
      rec.method = g_strndup(req->str, (gsize)(sp - req->str));
      char *sp2 = memchr(sp + 1, ' ', (size_t)(line_end - (sp + 1)));
      if (sp2)
        rec.url = g_strndup(sp + 1, (gsize)(sp2 - (sp + 1)));
    }
    char *p = line_end + 2;
    while (p < req->str + req->len) {
      char *nl = strstr(p, "\r\n");
      if (!nl || nl == p)
        break;
      char *colon = memchr(p, ':', (size_t)(nl - p));
      if (colon) {
        char *name = g_ascii_strdown(p, (gssize)(colon - p));
        const char *val = colon + 1;
        while (*val == ' ' || *val == '\t')
          val++;
        char *copy = g_strndup(val, (gsize)(nl - val));
        if (!strcmp(name, "host") && !rec.host)
          rec.host = copy;
        else if (!strcmp(name, "accept") && !rec.accept)
          rec.accept = copy;
        else if (!strcmp(name, "accept-encoding") && !rec.encoding)
          rec.encoding = copy;
        else if (!strcmp(name, "authorization") && !rec.authorization)
          rec.authorization = copy;
        else if (!strcmp(name, "cookie") && !rec.cookie)
          rec.cookie = copy;
        else
          g_free(copy);
        g_free(name);
      }
      p = nl + 2;
    }
  }
  record_req(s, rec);
  int call = ++s->calls;
  respond(s, ssl, call);
  SSL_shutdown(ssl);
  SSL_free(ssl);
  close(fd);
  g_string_free(req, TRUE);
}
static gpointer serve_loop(gpointer data) {
  Server *s = data;
  g_atomic_int_set(&s->ready, 1);
  while (!g_atomic_int_get(&s->stop)) {
    int fd = accept(s->fd, NULL, NULL);
    if (fd < 0)
      continue;
    if (g_atomic_int_get(&s->stop)) {
      close(fd);
      break;
    }
    handle_client(s, fd);
  }
  return NULL;
}
static Server *server_new(Mode mode, const char *key, const char *cert, const char *calendar) {
  Server *s = g_new0(Server, 1);
  s->mode = mode;
  s->fd = -1;
  s->calendar = g_strdup(calendar);
  s->calendar_len = strlen(calendar);
  g_mutex_init(&s->mu);
  s->ctx = SSL_CTX_new(TLS_server_method());
  if (!s->ctx || SSL_CTX_use_certificate_file(s->ctx, cert, SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_use_PrivateKey_file(s->ctx, key, SSL_FILETYPE_PEM) != 1) {
    FAIL("tls context");
    if (s->ctx)
      SSL_CTX_free(s->ctx);
    g_free(s->calendar);
    g_free(s);
    return NULL;
  }
  s->fd = socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(s->fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s->fd, 16) != 0) {
    FAIL("listen");
    return NULL;
  }
  socklen_t alen = sizeof addr;
  getsockname(s->fd, (struct sockaddr *)&addr, &alen);
  s->port = ntohs(addr.sin_port);
  s->thread = g_thread_new("agenda-tls", serve_loop, s);
  while (!g_atomic_int_get(&s->ready))
    g_usleep(1000);
  return s;
}
static void server_stop(Server *s) {
  if (!s)
    return;
  g_atomic_int_set(&s->stop, 1);
  int poke = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)s->port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (poke >= 0) {
    connect(poke, (struct sockaddr *)&addr, sizeof addr);
    close(poke);
  }
  shutdown(s->fd, SHUT_RDWR);
  g_thread_join(s->thread);
  close(s->fd);
  SSL_CTX_free(s->ctx);
  for (size_t i = 0; i < s->nreqs; i++) {
    g_free(s->reqs[i].method);
    g_free(s->reqs[i].url);
    g_free(s->reqs[i].host);
    g_free(s->reqs[i].accept);
    g_free(s->reqs[i].encoding);
    g_free(s->reqs[i].authorization);
    g_free(s->reqs[i].cookie);
  }
  free(s->reqs);
  g_free(s->calendar);
  g_mutex_clear(&s->mu);
  g_free(s);
}
static bool make_cert(const char *key, const char *cert) {
  char *argv[] = {"openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", (char *)key, "-out",
                  (char *)cert, "-days", "1", "-subj", "/CN=calendar.google.com", "-addext",
                  "subjectAltName=DNS:calendar.google.com", "-addext", "basicConstraints=critical,CA:TRUE", NULL};
  Proc p = proc_run("openssl", argv, "", 0, NULL, 20000);
  bool ok = !p.error && p.status == 0;
  if (!ok)
    FAIL("openssl %s", p.err ? p.err : "");
  proc_clear(&p);
  return ok;
}
static char *calendar_for_today(void) {
  char day[16], stamp[16];
  utc_day(day, sizeof day, 0);
  memcpy(stamp, day, 4);
  memcpy(stamp + 4, day + 5, 2);
  memcpy(stamp + 6, day + 8, 2);
  stamp[8] = 0;
  return g_strdup_printf(
      "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:test\r\nDTSTART:%sT090000Z\r\nDTEND:%sT100000Z\r\nSUMMARY:Demo fixture_token_0123456789 %s\r\nLOCATION:Room fixture_token_0123456789\r\nDESCRIPTION:Join https://meet.google.com/abc-defg-hij\r\nURL:https://calendar.google.com/event?eid=test\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n",
      stamp, stamp, feed);
}
static Proc refresh(char **env) {
  const char *parent = getenv("DAILY_AGENDA_PARENT_FIXTURE");
  if (parent && *parent) {
    char *args[] = {"cli", "--refresh", NULL};
    return run_at(parent, args, "", 0, (const char *const *)env, 75000);
  }
  char *args[] = {"--refresh", NULL};
  return run_at(binary, args, "", 0, (const char *const *)env, 75000);
}
static void scenario(Mode mode, void (*fn)(const char *root, char **env, Server *server)) {
  char *root = scratch();
  CHECK(root);
  char *key = g_build_filename(root, "key.pem", NULL);
  char *cert = g_build_filename(root, "cert.pem", NULL);
  if (!make_cert(key, cert)) {
    rm_rf(root);
    g_free(root);
    g_free(key);
    g_free(cert);
    return;
  }
  char *calendar = calendar_for_today();
  Server *server = server_new(mode, key, cert, calendar);
  CHECK(server);
  char port[16];
  snprintf(port, sizeof port, "%d", server->port);
  char *home = env_pair("HOME", root);
  char *cache = g_build_filename(root, "cache", NULL);
  char *state = g_build_filename(root, "state", NULL);
  char *cache_e = env_pair("XDG_CACHE_HOME", cache);
  char *state_e = env_pair("XDG_STATE_HOME", state);
  char *secret = g_build_filename(root, "secret", NULL);
  char *cred = env_pair("DAILY_CALENDAR_CREDENTIALS", secret);
  char *ssl = env_pair("SSL_CERT_FILE", cert);
  char *nix = env_pair("NIX_SSL_CERT_FILE", cert);
  char *preload = env_pair("LD_PRELOAD", network);
  char *fixture_port = env_pair("AGENDA_FIXTURE_PORT", port);
  const char *extra[] = {home, cache_e, state_e, cred, ssl, nix, preload, fixture_port, "HTTPS_PROXY=http://127.0.0.1:1",
                         "ALL_PROXY=http://127.0.0.1:1", NULL};
  char **env = agenda_env(extra);
  char *json = g_strdup_printf("{\"Work\":\"%s\"}", feed);
  write_file(secret, json, strlen(json), 0600);
  g_free(json);
  char day[16];
  utc_day(day, sizeof day, 0);
  char *notes = g_build_filename(root, "Notes", day, NULL);
  g_mkdir_with_parents(notes, 0700);
  char *note = g_build_filename(notes, "today.md", NULL);
  write_file(note, "# Reminders\n- Fixture reminder\n", 32, 0600);
  fn(root, env, server);
  server_stop(server);
  env_free(env);
  g_free(home);
  g_free(cache);
  g_free(state);
  g_free(cache_e);
  g_free(state_e);
  g_free(secret);
  g_free(cred);
  g_free(ssl);
  g_free(nix);
  g_free(preload);
  g_free(fixture_port);
  g_free(notes);
  g_free(note);
  g_free(key);
  g_free(cert);
  g_free(calendar);
  rm_rf(root);
  g_free(root);
}
static bool no_token(Proc *p) {
  return !contains(p->out, p->out_len, "fixture_token_") && !contains(p->err, p->err_len, "fixture_token_");
}
static void http_good(const char *root, char **env, Server *server) {
  Proc native = refresh(env);
  if (native.error || native.status != 0 || !no_token(&native))
    FAIL("refresh %d %s", native.status, native.err ? native.err : "");
  g_mutex_lock(&server->mu);
  size_t nreqs = server->nreqs;
  Req *reqs = server->reqs;
  if (nreqs != 2)
    FAIL("requests %zu", nreqs);
  for (size_t i = 0; i < nreqs; i++) {
    if (!reqs[i].method || strcmp(reqs[i].method, "GET") || !reqs[i].host || strcmp(reqs[i].host, "calendar.google.com") ||
        !reqs[i].accept || strcmp(reqs[i].accept, "text/calendar, text/plain") || !reqs[i].encoding ||
        strcmp(reqs[i].encoding, "identity") || reqs[i].authorization || reqs[i].cookie)
      FAIL("header %zu", i);
  }
  g_mutex_unlock(&server->mu);
  char *path = g_build_filename(root, "cache", "daily-agenda", "calendar.json", NULL);
  char *bar = g_build_filename(root, "state", "omarchy", "calendar-events.json", NULL);
  size_t n = 0, bn = 0;
  char *saved_text = read_file(path, &n);
  char *bar_text = read_file(bar, &bn);
  yyjson_doc *saved = saved_text ? yyjson_read(saved_text, n, 0) : NULL;
  yyjson_doc *rows = bar_text ? yyjson_read(bar_text, bn, 0) : NULL;
  yyjson_val *event = saved ? yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(saved), "events"), 0) : NULL;
  yyjson_val *row = rows ? yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(rows), "events"), 0) : NULL;
  if (!str_eq(yyjson_obj_get(event, "summary"), "Demo [redacted] [redacted]") ||
      !str_eq(yyjson_obj_get(row, "location"), "Room [redacted]") ||
      !str_eq(yyjson_obj_get(row, "meetingUrl"), "https://meet.google.com/abc-defg-hij"))
    FAIL("redaction");
  struct stat st;
  if (stat(path, &st) != 0 || (st.st_mode & 0777) != 0600 || stat(bar, &st) != 0 || (st.st_mode & 0777) != 0600)
    FAIL("modes");
  if (contains(bar_text, bn, "fixture_token_"))
    FAIL("bar token");
  if (legacy) {
    Proc old = run_at(legacy, (char *[]){"--refresh", NULL}, "", 0, (const char *const *)env, 75000);
    if (old.error || old.status != 0)
      FAIL("legacy refresh");
    size_t en = 0, ebn = 0;
    char *expected_text = read_file(path, &en);
    char *expected_bar = read_file(bar, &ebn);
    yyjson_doc *expected = expected_text ? yyjson_read(expected_text, en, 0) : NULL;
    yyjson_doc *expected_rows = expected_bar ? yyjson_read(expected_bar, ebn, 0) : NULL;
    if (!saved || !expected || !rows || !expected_rows)
      FAIL("legacy files");
    else {
      yyjson_mut_doc *a = yyjson_doc_mut_copy(saved, NULL);
      yyjson_mut_doc *b = yyjson_doc_mut_copy(expected, NULL);
      yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(a), "fetched_at");
      yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(b), "fetched_at");
      yyjson_doc *ai = yyjson_mut_doc_imut_copy(a, NULL);
      yyjson_doc *bi = yyjson_mut_doc_imut_copy(b, NULL);
      if (!json_eq(yyjson_doc_get_root(ai), yyjson_doc_get_root(bi)))
        FAIL("legacy cache");
      yyjson_doc_free(ai);
      yyjson_doc_free(bi);
      yyjson_mut_doc_free(a);
      yyjson_mut_doc_free(b);
      a = yyjson_doc_mut_copy(rows, NULL);
      b = yyjson_doc_mut_copy(expected_rows, NULL);
      yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(a), "syncedAt");
      yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(b), "syncedAt");
      ai = yyjson_mut_doc_imut_copy(a, NULL);
      bi = yyjson_mut_doc_imut_copy(b, NULL);
      if (!json_eq(yyjson_doc_get_root(ai), yyjson_doc_get_root(bi)))
        FAIL("legacy bar");
      yyjson_doc_free(ai);
      yyjson_doc_free(bi);
      yyjson_mut_doc_free(a);
      yyjson_mut_doc_free(b);
    }
    yyjson_doc_free(expected);
    yyjson_doc_free(expected_rows);
    g_free(expected_text);
    g_free(expected_bar);
    proc_clear(&old);
  }
  yyjson_doc_free(saved);
  yyjson_doc_free(rows);
  g_free(saved_text);
  g_free(bar_text);
  g_free(path);
  g_free(bar);
  proc_clear(&native);
}
static const char *retain_label;
static void retain_fn(const char *root, char **env, Server *server) {
  char day[16], stamp[40];
  utc_day(day, sizeof day, 0);
  utc_stamp(stamp, sizeof stamp);
  char *path = g_build_filename(root, "cache", "daily-agenda", "calendar.json", NULL);
  char *saved = g_strdup_printf("{\"day\":\"%s\",\"fetched_at\":\"%s\",\"events\":[]}", day, stamp);
  if (!write_file(path, saved, strlen(saved), 0600))
    FAIL("seed");
  Proc r = refresh(env);
  if (r.error || r.status != 1 || !no_token(&r))
    FAIL("%s status %d", retain_label, r.status);
  g_mutex_lock(&server->mu);
  if (server->nreqs != 1)
    FAIL("%s requests %zu", retain_label, server->nreqs);
  g_mutex_unlock(&server->mu);
  size_t n = 0;
  char *got = read_file(path, &n);
  if (!got || n != strlen(saved) || memcmp(got, saved, n))
    FAIL("%s cache changed", retain_label);
  g_free(got);
  if (legacy) {
    char *args[] = {"--refresh", NULL};
    Proc old = run_at(legacy, args, "", 0, (const char *const *)env, 75000);
    if (old.status != 1 || old.out_len != r.out_len || memcmp(old.out ? old.out : "", r.out ? r.out : "", r.out_len))
      FAIL("%s legacy", retain_label);
    proc_clear(&old);
  }
  proc_clear(&r);
  g_free(saved);
  g_free(path);
}
static void retain(Mode mode, const char *label) {
  retain_label = label;
  scenario(mode, retain_fn);
}
static void trust_fn(const char *root, char **env, Server *server) {
  size_t n = 0;
  while (env[n])
    n++;
  char **broken = calloc(n + 1, sizeof *broken);
  char *missing = g_build_filename(root, "missing.pem", NULL);
  char *ssl = env_pair("SSL_CERT_FILE", missing);
  for (size_t i = 0; i < n; i++)
    broken[i] = g_strdup(strncmp(env[i], "SSL_CERT_FILE=", 14) ? env[i] : ssl);
  Proc r = refresh(broken);
  if (r.error || r.status != 1 || !no_token(&r))
    FAIL("trust %d", r.status);
  g_mutex_lock(&server->mu);
  if (server->nreqs != 0)
    FAIL("unexpected request");
  g_mutex_unlock(&server->mu);
  proc_clear(&r);
  env_free(broken);
  g_free(ssl);
  g_free(missing);
}
static void bar_fail_fn(const char *root, char **env, Server *server) {
  (void)server;
  char *dir = g_build_filename(root, "state", "omarchy", NULL);
  g_mkdir_with_parents(dir, 0700);
  char *path = g_build_filename(dir, "calendar-events.json", NULL);
  write_file(path, "previous bar cache", 19, 0600);
  Proc r = refresh(env);
  const char *warn = "Warning: Calendar bar export failed; the previous rail calendar was kept.\n";
  if (r.error || r.status != 0 || r.err_len != strlen(warn) || memcmp(r.err ? r.err : "", warn, strlen(warn)))
    FAIL("bar fail %d %s", r.status, r.err ? r.err : "");
  size_t n = 0;
  char *got = read_file(path, &n);
  if (!got || n != 19 || memcmp(got, "previous bar cache", 19))
    FAIL("bar rolled back");
  g_free(got);
  char *cache = g_build_filename(root, "cache", "daily-agenda", "calendar.json", NULL);
  char *text = read_file(cache, &n);
  yyjson_doc *doc = text ? yyjson_read(text, n, 0) : NULL;
  if (!doc || yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "events")) != 1)
    FAIL("daily cache");
  yyjson_doc_free(doc);
  g_free(text);
  g_free(cache);
  proc_clear(&r);
  g_free(path);
  g_free(dir);
}
static void unsafe_fn(const char *root, char **env, Server *server) {
  (void)server;
  char *dir = g_build_filename(root, "state", "omarchy", NULL);
  g_mkdir_with_parents(dir, 0700);
  char *target = g_build_filename(root, "target", NULL);
  write_file(target, "keep", 4, 0600);
  char *link = g_build_filename(dir, "calendar-events.json", NULL);
  if (symlink(target, link) != 0)
    FAIL("symlink");
  Proc r = refresh(env);
  const char *warn = "Warning: Calendar bar export failed; the previous rail calendar was kept.\n";
  size_t n = 0;
  char *got = read_file(target, &n);
  if (r.error || r.status != 0 || r.err_len != strlen(warn) || memcmp(r.err ? r.err : "", warn, strlen(warn)) || !got ||
      n != 4 || memcmp(got, "keep", 4))
    FAIL("unsafe bar");
  g_free(got);
  proc_clear(&r);
  g_free(link);
  g_free(target);
  g_free(dir);
}
static void overstate_fn(const char *root, char **env, Server *server) {
  (void)root;
  (void)server;
  Proc native = refresh(env);
  if (legacy) {
    char *args[] = {"--refresh", NULL};
    Proc old = run_at(legacy, args, "", 0, (const char *const *)env, 75000);
    if (old.status != native.status)
      FAIL("legacy framing");
    proc_clear(&old);
  }
  if (native.error || native.status != 0)
    FAIL("overstate %d %s", native.status, native.err ? native.err : "");
  proc_clear(&native);
}
static void progress_fn(const char *root, char **env, Server *server) {
  (void)root;
  (void)server;
  int64_t started = now_ms();
  Proc r = refresh(env);
  if (r.error || r.status != 0 || now_ms() - started < 40000)
    FAIL("progress %d elapsed %lld", r.status, (long long)(now_ms() - started));
  proc_clear(&r);
}
static void slow_fn(const char *root, char **env, Server *server) {
  (void)root;
  (void)server;
  int64_t started = now_ms();
  Proc r = refresh(env);
  int64_t elapsed = now_ms() - started;
  if (r.error || r.status != 1 || elapsed < 19000 || elapsed >= 30000)
    FAIL("slow %d elapsed %lld", r.status, (long long)elapsed);
  proc_clear(&r);
}
static void test_http(void) {
  begin("private HTTPS refresh, request headers, proxy isolation and export parity");
  scenario(MODE_GOOD, http_good);
  const Mode modes[] = {MODE_REDIRECT, MODE_STATUS, MODE_SIZE, MODE_TYPE};
  const char *names[] = {"redirect", "status", "size", "type"};
  begin("redirects, HTTP errors, response bounds and invalid calendars retain cache");
  for (size_t i = 0; i < 4; i++)
    retain(modes[i], names[i]);
  begin("declared certificate trust fails closed without credential diagnostics");
  scenario(MODE_GOOD, trust_fn);
  begin("bar failure does not roll back the freshly written daily cache");
  scenario(MODE_BAR, bar_fail_fn);
  begin("unsafe bar publication retains state and reports only the bounded warning");
  scenario(MODE_GOOD, unsafe_fn);
  begin("complete calendars with overstated Content-Length retain legacy framing semantics");
  scenario(MODE_OVERSTATE, overstate_fn);
  begin("progressing transfers retain socket idle semantics beyond twenty seconds");
  scenario(MODE_PROGRESS, progress_fn);
  begin("idle reads time out rather than accepting a stalled calendar");
  scenario(MODE_SLOW, slow_fn);
}

int main(void) {
  OPENSSL_init_ssl(0, NULL);
  binary = sibling("DAILY_AGENDA_BIN", "daily-agenda");
  fixture = sibling("DAILY_AGENDA_FIXTURE", "agenda-fixture");
  limits_bin = sibling("DAILY_AGENDA_LIMITS", "agenda-limits");
  pty_bin = sibling("DAILY_AGENDA_PTY", "agenda-pty");
  network = sibling("DAILY_AGENDA_NETWORK", "libagenda-network.so");
  const char *leg = getenv("DAILY_AGENDA_LEGACY");
  legacy = leg && *leg ? g_strdup(leg) : NULL;
  if (!binary || !fixture || !limits_bin || !pty_bin || !network || access(binary, X_OK) || access(fixture, X_OK) ||
      access(limits_bin, X_OK) || access(pty_bin, X_OK) || access(network, R_OK)) {
    fprintf(stderr, "Set DAILY_AGENDA_BIN, DAILY_AGENDA_FIXTURE, DAILY_AGENDA_LIMITS, DAILY_AGENDA_PTY and DAILY_AGENDA_NETWORK\n");
    return 1;
  }
  test_windows();
  test_updates();
  test_rules();
  test_exdate();
  test_mixed();
  test_detached();
  test_aliases();
  test_zones();
  test_vtimezone();
  test_text();
  test_malformed();
  test_bounds();
  test_credentials();
  test_whitespace();
  test_notes();
  test_cache_display();
  test_save();
  test_bar();
  test_expand();
  test_iso();
  test_untrusted();
  test_long();
  test_sequences();
  test_schema();
  test_tz();
  test_pty();
  test_http();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("ok");
  return 0;
}
