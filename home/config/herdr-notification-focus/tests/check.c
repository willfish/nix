#define _GNU_SOURCE
#include <dirent.h>
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

#pragma GCC diagnostic ignored "-Wunused-result"

static const char *test_name;
static int failures;
static char *binary, *fixture, *peer_bin, *hypr_bin;
extern char **environ;
static const char *body = "pi finished: admin · 3 · 2 hadleigh review";
static const char *snap =
    "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\",\"number\":1},"
    "{\"workspace_id\":\"w3\",\"label\":\"admin\",\"number\":3}],\"tabs\":["
    "{\"tab_id\":\"t1\",\"workspace_id\":\"w1\",\"label\":\"only\"},"
    "{\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"label\":\"2 hadleigh review\"},"
    "{\"tab_id\":\"t3\",\"workspace_id\":\"w3\",\"label\":\"other\"}],\"agents\":["
    "{\"agent\":\"pi\",\"display_agent\":\"pi · medium\",\"agent_status\":\"working\","
    "\"pane_id\":\"p1\",\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"state_change_seq\":4},"
    "{\"agent\":\"pi\",\"display_agent\":\"pi · medium\",\"agent_status\":\"done\","
    "\"pane_id\":\"p2\",\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"state_change_seq\":9},"
    "{\"agent\":\"pi\",\"agent_status\":\"idle\",\"pane_id\":\"p3\",\"tab_id\":\"t2\","
    "\"workspace_id\":\"w3\",\"state_change_seq\":3}]}";

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
    return strdup(value);
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n < 0)
    return NULL;
  exe[n] = 0;
  char *slash = strrchr(exe, '/');
  if (!slash)
    return NULL;
  *slash = 0;
  char *out = NULL;
  if (asprintf(&out, "%s/%s", exe, name) < 0)
    return NULL;
  return out;
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
  size_t base = 0;
  while (environ[base])
    base++;
  size_t extra = 0;
  if (over)
    while (over[extra])
      extra++;
  char **out = calloc(base + extra + 1, sizeof *out);
  if (!out)
    return NULL;
  size_t n = 0;
  for (size_t i = 0; i < base; i++)
    out[n++] = strdup(environ[i]);
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
        free(out[at]);
        memmove(out + at, out + at + 1, (n - at) * sizeof *out);
        n--;
      }
      continue;
    }
    char *copy = strdup(over[i]);
    if (at < n) {
      free(out[at]);
      out[at] = copy;
    } else
      out[n++] = copy;
  }
  return out;
}
static Proc run_proc(const char *cmd, char *const *argv, const void *input,
                     size_t input_len, const char *cwd, const char *const *env,
                     int timeout_ms) {
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
    if (cwd && chdir(cwd))
      _exit(127);
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
    if (!merged)
      _exit(127);
    execvpe(cmd, argv, merged);
    _exit(127);
  }
  setpgid(pid, pid);
  close(inp[0]);
  close(outp[1]);
  close(errp[1]);
  size_t sent = 0;
  while (sent < input_len) {
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
    p.out = calloc(1, 1);
  if (!p.err)
    p.err = calloc(1, 1);
  if (WIFEXITED(status))
    p.status = WEXITSTATUS(status);
  else
    p.error = true;
  return p;
}
static char *temp_dir(const char *prefix) {
  char tmpl[64];
  snprintf(tmpl, sizeof tmpl, "/tmp/%sXXXXXX", prefix);
  char *path = strdup(tmpl);
  if (!path || !mkdtemp(path)) {
    free(path);
    return NULL;
  }
  return path;
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
        char *child = NULL;
        if (asprintf(&child, "%s/%s", path, ent->d_name) > 0)
          rm_rf(child);
        free(child);
      }
      closedir(dir);
    }
    rmdir(path);
  } else
    unlink(path);
}
static int write_file(const char *path, const void *data, size_t n) {
  char *copy = strdup(path);
  if (!copy)
    return -1;
  for (char *p = copy + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    if (mkdir(copy, 0755) && errno != EEXIST) {
      free(copy);
      return -1;
    }
    *p = '/';
  }
  free(copy);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;
  const unsigned char *p = data;
  size_t at = 0;
  while (at < n) {
    ssize_t w = write(fd, p + at, n - at);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0) {
      close(fd);
      return -1;
    }
    at += (size_t)w;
  }
  return close(fd);
}
static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END)) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return NULL;
  }
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[n] = 0;
  fclose(f);
  if (len)
    *len = (size_t)n;
  return buf;
}
static bool json_equal(yyjson_val *a, yyjson_val *b) {
  if (!a || !b)
    return a == b;
  if (yyjson_is_num(a) || yyjson_is_num(b)) {
    if (!yyjson_is_num(a) || !yyjson_is_num(b))
      return false;
    if (yyjson_is_raw(a) || yyjson_is_raw(b))
      return yyjson_is_raw(a) && yyjson_is_raw(b) &&
             yyjson_get_len(a) == yyjson_get_len(b) &&
             !memcmp(yyjson_get_raw(a), yyjson_get_raw(b), yyjson_get_len(a));
    return yyjson_get_num(a) == yyjson_get_num(b);
  }
  if (yyjson_get_type(a) != yyjson_get_type(b))
    return false;
  if (yyjson_is_str(a))
    return yyjson_get_len(a) == yyjson_get_len(b) &&
           !memcmp(yyjson_get_str(a), yyjson_get_str(b), yyjson_get_len(a));
  if (yyjson_is_arr(a)) {
    if (yyjson_arr_size(a) != yyjson_arr_size(b))
      return false;
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(a, i, n, item) if (!json_equal(item, yyjson_arr_get(b, i)))
      return false;
    return true;
  }
  if (yyjson_is_obj(a)) {
    if (yyjson_obj_size(a) != yyjson_obj_size(b))
      return false;
    size_t i, n;
    yyjson_val *key, *item;
    yyjson_obj_foreach(a, i, n, key, item) {
      yyjson_val *other =
          yyjson_obj_getn(b, yyjson_get_str(key), yyjson_get_len(key));
      if (!json_equal(item, other))
        return false;
    }
    return true;
  }
  return yyjson_is_null(a) || yyjson_is_true(a) == yyjson_is_true(b);
}
static yyjson_doc *parse_out(const Proc *p) {
  if (!p->out_len)
    return NULL;
  return yyjson_read((char *)p->out, p->out_len,
                     YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_BIGNUM_AS_RAW);
}
static Proc fx(const char *mode, const char **args, const char *input,
               const char *const *env, int status) {
  size_t n = 2;
  if (args)
    while (args[n - 2])
      n++;
  char **argv = calloc(n + 1, sizeof *argv);
  argv[0] = fixture;
  argv[1] = (char *)mode;
  for (size_t i = 2; i < n; i++)
    argv[i] = (char *)args[i - 2];
  Proc p = run_proc(fixture, argv, input ? input : "", input ? strlen(input) : 0,
                    NULL, env, 15000);
  free(argv);
  if (p.error || p.status != status)
    FAIL("fixture %s status %d wanted %d stderr %s", mode, p.status, status,
         p.err ? (char *)p.err : "");
  return p;
}
static yyjson_val *fx_val(Proc *p, yyjson_doc **doc) {
  *doc = parse_out(p);
  return *doc ? yyjson_doc_get_root(*doc) : NULL;
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
static char *cp_str(uint32_t cp) {
  char tmp[4];
  size_t n = utf8(cp, tmp);
  char *s = malloc(n + 1);
  memcpy(s, tmp, n);
  s[n] = 0;
  return s;
}
static char *replace_one(const char *text, const char *old, const char *neu) {
  const char *hit = strstr(text, old);
  if (!hit)
    return strdup(text);
  size_t n = (size_t)(hit - text);
  char *out = NULL;
  if (asprintf(&out, "%.*s%s%s", (int)n, text, neu, hit + strlen(old)) < 0)
    return NULL;
  return out;
}
static bool str_eq(yyjson_val *v, const char *s) {
  return yyjson_is_str(v) && yyjson_get_len(v) == strlen(s) &&
         !memcmp(yyjson_get_str(v), s, strlen(s));
}
static void expect_toast(yyjson_val *v) {
  EXPECT(str_eq(yyjson_obj_get(v, "agent"), "pi"));
  EXPECT(str_eq(yyjson_obj_get(v, "event"), "finished"));
  EXPECT(str_eq(yyjson_obj_get(v, "workspace_label"), "admin"));
  EXPECT(yyjson_is_num(yyjson_obj_get(v, "workspace_number")) &&
         yyjson_get_num(yyjson_obj_get(v, "workspace_number")) == 3);
  EXPECT(str_eq(yyjson_obj_get(v, "tab_label"), "2 hadleigh review"));
}
static void test_formats(void) {
  begin("toast formats and numeric rejection");
  const char *args[] = {"Ghostty", body, NULL};
  Proc p = fx("parse", args, NULL, NULL, 0);
  yyjson_doc *doc = NULL;
  expect_toast(fx_val(&p, &doc));
  yyjson_doc_free(doc);
  proc_free(&p);
  const char *short_args[] = {"Ghostty", "pi needs attention: admin · 3", NULL};
  p = fx("parse", short_args, NULL, NULL, 0);
  yyjson_val *v = fx_val(&p, &doc);
  EXPECT(yyjson_is_null(yyjson_obj_get(v, "tab_label")));
  yyjson_doc_free(doc);
  proc_free(&p);
  const char *later[] = {"Ghostty", "pi finished: dot · 1 · 1 race · later", NULL};
  p = fx("parse", later, NULL, NULL, 0);
  v = fx_val(&p, &doc);
  EXPECT(str_eq(yyjson_obj_get(v, "tab_label"), "1 race · later"));
  yyjson_doc_free(doc);
  proc_free(&p);
  char *wide = cp_str(0xff13);
  char *wrapped = NULL;
  asprintf(&wrapped, "\x1cpi finished: admin · %s\xc2\x85", wide);
  const char *uni[] = {"Ghostty", wrapped, NULL};
  p = fx("parse", uni, NULL, NULL, 0);
  v = fx_val(&p, &doc);
  EXPECT(yyjson_get_num(yyjson_obj_get(v, "workspace_number")) == 3);
  yyjson_doc_free(doc);
  proc_free(&p);
  free(wrapped);
  free(wide);
  const char *split[] = {"pi finished", "admin · 3", "com.Ghostty", NULL};
  p = fx("parse", split, NULL, NULL, 0);
  v = fx_val(&p, &doc);
  EXPECT(str_eq(yyjson_obj_get(v, "agent"), "pi"));
  yyjson_doc_free(doc);
  proc_free(&p);
  const char *supx[] = {"Ghostty", "pi finished: admin · ²x", NULL};
  p = fx("parse", supx, NULL, NULL, 0);
  v = fx_val(&p, &doc);
  EXPECT(yyjson_is_null(v));
  yyjson_doc_free(doc);
  proc_free(&p);
  const uint32_t digits[] = {0xb2, 0x2460, 0x1369, 0x1f101};
  for (size_t i = 0; i < 4; i++) {
    char *d = cp_str(digits[i]);
    char *text = NULL;
    asprintf(&text, "pi finished: admin · %s", d);
    const char *bad[] = {"Ghostty", text, NULL};
    Proc badp = fx("parse", bad, NULL, NULL, 1);
    proc_free(&badp);
    free(text);
    free(d);
  }
  const char *summaries[] = {"Slack", "Ghostty", "Ghostty", "Ghostty", "ghostty"};
  const char *texts[] = {body, "finished: admin · 3", "pi finished: dot · -1",
                         "pi finished: dot · x", body};
  for (size_t i = 0; i < 5; i++) {
    const char *bad[] = {summaries[i], texts[i], NULL};
    Proc badp = fx("parse", bad, NULL, NULL, 0);
    v = fx_val(&badp, &doc);
    EXPECT(yyjson_is_null(v));
    yyjson_doc_free(doc);
    proc_free(&badp);
  }
}
static const uint32_t py_spaces[] = {
    9, 10, 11, 12, 13, 28, 29, 30, 31, 32, 0x85, 0xa0, 0x1680, 0x2000, 0x2001,
    0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a,
    0x2028, 0x2029, 0x202f, 0x205f, 0x3000};
static void test_whitespace(void) {
  begin("Python whitespace stripping");
  for (size_t i = 0; i < sizeof py_spaces / sizeof py_spaces[0]; i++) {
    char sp[8];
    size_t n = utf8(py_spaces[i], sp);
    sp[n] = 0;
    char *summary = NULL;
    char *text = NULL;
    asprintf(&summary, "%sGhostty%s", sp, sp);
    asprintf(&text, "%spi%s finished: %sadmin%s · %s3%s · %s2 hadleigh review%s",
             sp, sp, sp, sp, sp, sp, sp, sp);
    const char *args[] = {summary, text, NULL};
    Proc p = fx("parse", args, NULL, NULL, 0);
    yyjson_doc *doc = NULL;
    yyjson_val *parsed = fx_val(&p, &doc);
    expect_toast(parsed);
    if (py_spaces[i] < 28 || py_spaces[i] > 31) {
      yyjson_doc *base = yyjson_read(snap, strlen(snap), 0);
      yyjson_mut_doc *mut = yyjson_doc_mut_copy(base, NULL);
      yyjson_doc_free(base);
      yyjson_mut_val *root = yyjson_mut_doc_get_root(mut);
      yyjson_mut_val *ws = yyjson_mut_obj_get(root, "workspaces");
      yyjson_mut_val *agents = yyjson_mut_obj_get(root, "agents");
      char *num = NULL;
      char *seq = NULL;
      asprintf(&num, "%s3%s", sp, sp);
      asprintf(&seq, "%s9%s", sp, sp);
      yyjson_mut_obj_remove_key(yyjson_mut_arr_get(ws, 1), "number");
      yyjson_mut_obj_add_strcpy(mut, yyjson_mut_arr_get(ws, 1), "number", num);
      yyjson_mut_obj_remove_key(yyjson_mut_arr_get(agents, 1), "state_change_seq");
      yyjson_mut_obj_add_strcpy(mut, yyjson_mut_arr_get(agents, 1),
                                "state_change_seq", seq);
      size_t len = 0;
      char *snap_text = yyjson_mut_write(mut, 0, &len);
      char *toast = yyjson_val_write(parsed, 0, NULL);
      char *input = NULL;
      asprintf(&input, "{\"snapshot\":%s,\"toast\":%s}", snap_text, toast);
      Proc r = fx("resolve", NULL, input, NULL, 0);
      yyjson_doc *rd = NULL;
      EXPECT(str_eq(yyjson_obj_get(fx_val(&r, &rd), "pane_id"), "p2"));
      yyjson_doc_free(rd);
      proc_free(&r);
      free(input);
      free(toast);
      free(snap_text);
      free(num);
      free(seq);
      yyjson_mut_doc_free(mut);
    }
    yyjson_doc_free(doc);
    proc_free(&p);
    free(summary);
    free(text);
  }
}
static char *resolve_input(const char *snapshot, const char *text) {
  const char *args[] = {"Ghostty", text, NULL};
  Proc toast = fx("parse", args, NULL, NULL, 0);
  yyjson_doc *doc = NULL;
  yyjson_val *parsed = fx_val(&toast, &doc);
  char *toast_text = parsed ? yyjson_val_write(parsed, 0, NULL) : strdup("null");
  char *input = NULL;
  asprintf(&input, "{\"snapshot\":%s,\"toast\":%s}", snapshot, toast_text);
  yyjson_doc_free(doc);
  proc_free(&toast);
  free(toast_text);
  return input;
}
static void test_preference(void) {
  begin("pane status preference and tie breaking");
  char *input = resolve_input(snap, body);
  Proc p = fx("resolve", NULL, input, NULL, 0);
  yyjson_doc *doc = NULL;
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p2"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  const char *names[] = {"PI", "pi · medium", "pi custom model"};
  for (size_t i = 0; i < 3; i++) {
    char *text = NULL;
    asprintf(&text, "%s finished: admin · 3 · 2 hadleigh review", names[i]);
    input = resolve_input(snap, text);
    p = fx("resolve", NULL, input, NULL, 0);
    EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p2"));
    yyjson_doc_free(doc);
    proc_free(&p);
    free(input);
    free(text);
  }
  yyjson_doc *base = yyjson_read(snap, strlen(snap), 0);
  yyjson_mut_doc *mut = yyjson_doc_mut_copy(base, NULL);
  yyjson_doc_free(base);
  yyjson_mut_val *agents = yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "agents");
  yyjson_mut_obj_remove_key(yyjson_mut_arr_get(agents, 0), "agent_status");
  yyjson_mut_obj_add_str(mut, yyjson_mut_arr_get(agents, 0), "agent_status", "blocked");
  yyjson_mut_obj_remove_key(yyjson_mut_arr_get(agents, 0), "state_change_seq");
  yyjson_mut_obj_add_int(mut, yyjson_mut_arr_get(agents, 0), "state_change_seq", 11);
  char *text = yyjson_mut_write(mut, 0, NULL);
  char *attention = replace_one(body, "finished", "needs attention");
  input = resolve_input(text, attention);
  p = fx("resolve", NULL, input, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p1"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(attention);
  yyjson_mut_obj_remove_key(yyjson_mut_arr_get(agents, 1), "state_change_seq");
  yyjson_mut_obj_add_int(mut, yyjson_mut_arr_get(agents, 1), "state_change_seq", 3);
  free(text);
  text = yyjson_mut_write(mut, 0, NULL);
  input = resolve_input(text, body);
  p = fx("resolve", NULL, input, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p2"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  for (size_t i = 0; i < 3; i++) {
    yyjson_mut_obj_remove_key(yyjson_mut_arr_get(agents, i), "agent_status");
    yyjson_mut_obj_add_str(mut, yyjson_mut_arr_get(agents, i), "agent_status",
                           "working");
  }
  free(text);
  text = yyjson_mut_write(mut, 0, NULL);
  input = resolve_input(text, body);
  p = fx("resolve", NULL, input, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p1"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(text);
  yyjson_mut_doc_free(mut);
}
static void test_workspace(void) {
  begin("workspace matching and ambiguous tabs");
  char *missing = replace_one(body, "admin", "missing");
  char *input = resolve_input(snap, missing);
  Proc p = fx("resolve", NULL, input, NULL, 0);
  yyjson_doc *doc = NULL;
  EXPECT(yyjson_is_null(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(missing);
  char *renamed = replace_one(body, "2 hadleigh review", "renamed");
  input = resolve_input(snap, renamed);
  p = fx("resolve", NULL, input, NULL, 0);
  yyjson_val *v = fx_val(&p, &doc);
  EXPECT(yyjson_obj_size(v) == 1 && str_eq(yyjson_obj_get(v, "workspace_id"), "w3"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  yyjson_doc *base = yyjson_read(snap, strlen(snap), 0);
  yyjson_mut_doc *mut = yyjson_doc_mut_copy(base, NULL);
  yyjson_doc_free(base);
  yyjson_mut_val *ws = yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "workspaces");
  yyjson_mut_val *extra = yyjson_mut_obj(mut);
  yyjson_mut_obj_add_str(mut, extra, "workspace_id", "other");
  yyjson_mut_obj_add_str(mut, extra, "label", "admin");
  yyjson_mut_obj_add_int(mut, extra, "number", 8);
  yyjson_mut_arr_add_val(ws, extra);
  char *text = yyjson_mut_write(mut, 0, NULL);
  input = resolve_input(text, body);
  p = fx("resolve", NULL, input, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "workspace_id"), "w3"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  char *seven = replace_one(body, " · 3", " · 7");
  input = resolve_input(text, seven);
  p = fx("resolve", NULL, input, NULL, 0);
  EXPECT(yyjson_is_null(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(seven);
  yyjson_mut_val *tabs = yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "tabs");
  yyjson_mut_arr_add_val(tabs, yyjson_mut_arr_get(tabs, 1));
  free(text);
  text = yyjson_mut_write(mut, 0, NULL);
  input = resolve_input(text, body);
  p = fx("resolve", NULL, input, NULL, 0);
  v = fx_val(&p, &doc);
  EXPECT(yyjson_obj_size(v) == 1 && str_eq(yyjson_obj_get(v, "workspace_id"), "w3"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(text);
  free(renamed);
  yyjson_mut_doc_free(mut);
}
static void test_fallback(void) {
  begin("single-tab and no-tab fallback");
  yyjson_doc *base = yyjson_read(snap, strlen(snap), 0);
  yyjson_mut_doc *mut = yyjson_doc_mut_copy(base, NULL);
  yyjson_doc_free(base);
  yyjson_mut_val *root = yyjson_mut_doc_get_root(mut);
  const char *one_tab =
      "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\",\"number\":1},"
      "{\"workspace_id\":\"w3\",\"label\":\"admin\",\"number\":3}],\"tabs\":["
      "{\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"label\":\"2 hadleigh review\"}],"
      "\"agents\":[{\"agent\":\"pi\",\"display_agent\":\"pi · medium\",\"agent_status\":\"working\","
      "\"pane_id\":\"p1\",\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"state_change_seq\":4},"
      "{\"agent\":\"pi\",\"display_agent\":\"pi · medium\",\"agent_status\":\"done\","
      "\"pane_id\":\"p2\",\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"state_change_seq\":9},"
      "{\"agent\":\"pi\",\"agent_status\":\"idle\",\"pane_id\":\"p3\",\"tab_id\":\"t2\","
      "\"workspace_id\":\"w3\",\"state_change_seq\":3}]}";
  char *text = NULL;
  char *input = resolve_input(one_tab, "pi finished: admin · 3");
  Proc p = fx("resolve", NULL, input, NULL, 0);
  yyjson_doc *doc = NULL;
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p2"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  yyjson_mut_obj_remove_key(root, "tabs");
  yyjson_mut_obj_add_arr(mut, root, "tabs");
  free(text);
  text = yyjson_mut_write(mut, 0, NULL);
  input = resolve_input(text, "pi finished: admin · 3");
  p = fx("resolve", NULL, input, NULL, 0);
  yyjson_val *v = fx_val(&p, &doc);
  EXPECT(yyjson_obj_size(v) == 1 && str_eq(yyjson_obj_get(v, "workspace_id"), "w3"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  const char *no_idle =
      "{\"workspaces\":[{\"workspace_id\":\"w3\",\"label\":\"admin\",\"number\":3}],"
      "\"tabs\":[],\"agents\":[{\"agent\":\"pi\",\"agent_status\":\"working\","
      "\"pane_id\":\"p1\",\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"state_change_seq\":4},"
      "{\"agent\":\"pi\",\"agent_status\":\"done\",\"pane_id\":\"p2\",\"tab_id\":\"t2\","
      "\"workspace_id\":\"w3\",\"state_change_seq\":9}]}";
  free(text);
  text = NULL;
  input = resolve_input(no_idle, "pi finished: admin · 3");
  p = fx("resolve", NULL, input, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p2"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  yyjson_mut_obj_remove_key(root, "agents");
  yyjson_mut_obj_add_arr(mut, root, "agents");
  yyjson_mut_val *empty = yyjson_mut_obj(mut);
  yyjson_mut_obj_add_str(mut, empty, "workspace_id", "w3");
  yyjson_mut_obj_add_str(mut, empty, "tab_id", "empty");
  yyjson_mut_obj_add_str(mut, empty, "label", "2 hadleigh review");
  yyjson_mut_val *one = yyjson_mut_arr(mut);
  yyjson_mut_arr_add_val(one, empty);
  yyjson_mut_obj_remove_key(root, "tabs");
  yyjson_mut_obj_add_val(mut, root, "tabs", one);
  free(text);
  text = yyjson_mut_write(mut, 0, NULL);
  input = resolve_input(text, body);
  p = fx("resolve", NULL, input, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "tab_id"), "empty"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(text);
  yyjson_mut_doc_free(mut);
}
static void test_numbers(void) {
  begin("wide integers, ID types and invalid numeric records");
  yyjson_doc *base = yyjson_read(snap, strlen(snap), 0);
  yyjson_mut_doc *mut = yyjson_doc_mut_copy(base, NULL);
  yyjson_doc_free(base);
  yyjson_mut_val *root = yyjson_mut_doc_get_root(mut);
  yyjson_mut_val *ws = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "workspaces"), 1);
  yyjson_mut_val *agent = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "agents"), 0);
  yyjson_mut_obj_remove_key(ws, "number");
  yyjson_mut_obj_add_str(mut, ws, "number", " +0_3 ");
  yyjson_mut_obj_remove_key(agent, "state_change_seq");
  yyjson_mut_obj_add_str(mut, agent, "state_change_seq", "999999999999999999999999");
  yyjson_mut_obj_remove_key(agent, "agent_status");
  yyjson_mut_obj_add_str(mut, agent, "agent_status", "done");
  char *text = yyjson_mut_write(mut, 0, NULL);
  char *input = resolve_input(text, body);
  Proc p = fx("resolve", NULL, input, NULL, 0);
  yyjson_doc *doc = NULL;
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p1"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  yyjson_mut_obj_remove_key(agent, "state_change_seq");
  yyjson_mut_obj_add_str(mut, agent, "state_change_seq", "bad");
  free(text);
  text = yyjson_mut_write(mut, 0, NULL);
  input = resolve_input(text, body);
  p = fx("resolve", NULL, input, NULL, 1);
  proc_free(&p);
  free(input);
  free(text);
  yyjson_mut_doc_free(mut);
  char *wide = replace_one(snap, "\"state_change_seq\":9",
                           "\"state_change_seq\":18446744073709551615");
  input = resolve_input(wide, body);
  p = fx("resolve", NULL, input, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "pane_id"), "p2"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(wide);
  base = yyjson_read(snap, strlen(snap), 0);
  mut = yyjson_doc_mut_copy(base, NULL);
  yyjson_doc_free(base);
  root = yyjson_mut_doc_get_root(mut);
  yyjson_mut_obj_remove_key(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "workspaces"), 1),
                            "workspace_id");
  yyjson_mut_obj_add_int(mut, yyjson_mut_arr_get(yyjson_mut_obj_get(root, "workspaces"), 1),
                         "workspace_id", 3);
  yyjson_mut_obj_remove_key(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "tabs"), 1),
                            "workspace_id");
  yyjson_mut_obj_add_str(mut, yyjson_mut_arr_get(yyjson_mut_obj_get(root, "tabs"), 1),
                         "workspace_id", "3");
  text = yyjson_mut_write(mut, 0, NULL);
  input = resolve_input(text, body);
  p = fx("resolve", NULL, input, NULL, 0);
  yyjson_val *v = fx_val(&p, &doc);
  EXPECT(yyjson_obj_size(v) == 1 && yyjson_is_num(yyjson_obj_get(v, "workspace_id")) &&
         yyjson_get_num(yyjson_obj_get(v, "workspace_id")) == 3);
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(text);
  yyjson_mut_doc_free(mut);
  const char *sup[] = {"Ghostty", "pi finished: admin · ²", NULL};
  p = fx("parse", sup, NULL, NULL, 1);
  proc_free(&p);
  for (uint32_t cp = 28; cp <= 31; cp++) {
    char *sep = cp_str(cp);
    char *num = NULL;
    char *seq = NULL;
    asprintf(&num, "%s3%s", sep, sep);
    asprintf(&seq, "%s9%s", sep, sep);
    base = yyjson_read(snap, strlen(snap), 0);
    mut = yyjson_doc_mut_copy(base, NULL);
    yyjson_doc_free(base);
    root = yyjson_mut_doc_get_root(mut);
    yyjson_mut_obj_remove_key(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "workspaces"), 1),
                              "number");
    yyjson_mut_obj_add_strcpy(mut, yyjson_mut_arr_get(yyjson_mut_obj_get(root, "workspaces"), 1),
                              "number", num);
    text = yyjson_mut_write(mut, 0, NULL);
    input = resolve_input(text, body);
    p = fx("resolve", NULL, input, NULL, 1);
    proc_free(&p);
    free(input);
    free(text);
    yyjson_mut_doc_free(mut);
    base = yyjson_read(snap, strlen(snap), 0);
    mut = yyjson_doc_mut_copy(base, NULL);
    yyjson_doc_free(base);
    root = yyjson_mut_doc_get_root(mut);
    yyjson_mut_obj_remove_key(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "agents"), 1),
                              "state_change_seq");
    yyjson_mut_obj_add_strcpy(mut,
                              yyjson_mut_arr_get(yyjson_mut_obj_get(root, "agents"), 1),
                              "state_change_seq", seq);
    text = yyjson_mut_write(mut, 0, NULL);
    input = resolve_input(text, body);
    p = fx("resolve", NULL, input, NULL, 1);
    proc_free(&p);
    free(input);
    free(text);
    yyjson_mut_doc_free(mut);
    free(num);
    free(seq);
    free(sep);
  }
}
static void test_malformed_groups(void) {
  begin("unused malformed groups and selected malformed status");
  const char *toast_args[] = {"Ghostty", body, NULL};
  Proc toast = fx("parse", toast_args, NULL, NULL, 0);
  yyjson_doc *td = NULL;
  char *toast_text = yyjson_val_write(fx_val(&toast, &td), 0, NULL);
  char *input = NULL;
  asprintf(&input, "{\"snapshot\":{\"workspaces\":[],\"tabs\":\"bad\",\"agents\":\"bad\"},\"toast\":%s}",
           toast_text);
  Proc p = fx("resolve", NULL, input, NULL, 0);
  yyjson_doc *doc = NULL;
  EXPECT(yyjson_is_null(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  yyjson_doc *base = yyjson_read(snap, strlen(snap), 0);
  char *ws = yyjson_val_write(yyjson_obj_get(yyjson_doc_get_root(base), "workspaces"), 0, NULL);
  yyjson_doc_free(base);
  asprintf(&input,
           "{\"snapshot\":{\"workspaces\":%s,\"tabs\":[],\"agents\":\"bad\"},\"toast\":%s}",
           ws, toast_text);
  p = fx("resolve", NULL, input, NULL, 0);
  yyjson_val *v = fx_val(&p, &doc);
  EXPECT(yyjson_obj_size(v) == 1 && str_eq(yyjson_obj_get(v, "workspace_id"), "w3"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  free(ws);
  base = yyjson_read(snap, strlen(snap), 0);
  yyjson_mut_doc *mut = yyjson_doc_mut_copy(base, NULL);
  yyjson_doc_free(base);
  yyjson_mut_val *agent = yyjson_mut_arr_get(
      yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "agents"), 1);
  yyjson_mut_obj_remove_key(agent, "agent_status");
  yyjson_mut_obj_add_arr(mut, agent, "agent_status");
  char *text = yyjson_mut_write(mut, 0, NULL);
  asprintf(&input, "{\"snapshot\":%s,\"toast\":%s}", text, toast_text);
  p = fx("resolve", NULL, input, NULL, 1);
  proc_free(&p);
  free(input);
  free(text);
  yyjson_mut_doc_free(mut);
  p = fx("alive", NULL,
         "{\"snapshot\":{\"agents\":[{\"pane_id\":\"p\"},{\"pane_id\":[]}]},\"target\":{\"pane_id\":\"p\"}}",
         NULL, 1);
  proc_free(&p);
  yyjson_doc_free(td);
  proc_free(&toast);
  free(toast_text);
}
static void test_alive_payload_choose(void) {
  begin("alive, focus payload and window selection");
  char *input = NULL;
  asprintf(&input, "{\"snapshot\":%s,\"target\":{\"pane_id\":\"p2\",\"workspace_id\":\"gone\"}}", snap);
  Proc p = fx("alive", NULL, input, NULL, 0);
  yyjson_doc *doc = NULL;
  EXPECT(yyjson_is_true(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  p = fx("alive", NULL, "{\"snapshot\":{\"panes\":[{\"pane_id\":\"p\"}]},\"target\":{\"pane_id\":\"p\"}}",
         NULL, 0);
  EXPECT(yyjson_is_true(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  asprintf(&input, "{\"snapshot\":%s,\"target\":{\"pane_id\":\"gone\",\"tab_id\":\"t2\"}}", snap);
  p = fx("alive", NULL, input, NULL, 0);
  EXPECT(yyjson_is_false(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  asprintf(&input, "{\"snapshot\":%s,\"target\":{\"tab_id\":\"t2\"}}", snap);
  p = fx("alive", NULL, input, NULL, 0);
  EXPECT(yyjson_is_true(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  asprintf(&input, "{\"snapshot\":%s,\"target\":{\"workspace_id\":\"w3\"}}", snap);
  p = fx("alive", NULL, input, NULL, 0);
  EXPECT(yyjson_is_true(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(input);
  const char *payloads[] = {
      "{\"pane_id\":\"p\",\"tab_id\":\"t\"}",
      "{\"pane_id\":\"\",\"tab_id\":\"t\"}",
      "{\"workspace_id\":3}",
      "{}",
  };
  const char *methods[] = {"pane.focus", "tab.focus", "workspace.focus"};
  const char *params[] = {"{\"pane_id\":\"p\"}", "{\"tab_id\":\"t\"}", "{\"workspace_id\":3}"};
  for (int i = 0; i < 3; i++) {
    p = fx("payload", NULL, payloads[i], NULL, 0);
    yyjson_val *v = fx_val(&p, &doc);
    EXPECT(str_eq(yyjson_obj_get(v, "method"), methods[i]));
    yyjson_doc *want = yyjson_read(params[i], strlen(params[i]), 0);
    EXPECT(json_equal(yyjson_obj_get(v, "params"), yyjson_doc_get_root(want)));
    yyjson_doc_free(want);
    yyjson_doc_free(doc);
    proc_free(&p);
  }
  p = fx("payload", NULL, payloads[3], NULL, 0);
  EXPECT(yyjson_is_null(fx_val(&p, &doc)));
  yyjson_doc_free(doc);
  proc_free(&p);
  const char *clients =
      "[{\"pid\":1,\"class\":\"Ghostty\",\"address\":\"one\",\"title\":\"other\",\"focusHistoryID\":0},"
      "{\"pid\":9,\"class\":\"Ghostty\",\"address\":\"nine\",\"title\":\"admin\",\"focusHistoryID\":4},"
      "{\"pid\":3,\"class\":\"cliamp.ghostty\",\"address\":\"three\",\"focusHistoryID\":-9}]";
  const char *chooses[] = {
      "{\"clients\":%s,\"pid\":3,\"ancestors\":[],\"label\":\"\"}",
      "{\"clients\":%s,\"pid\":0,\"ancestors\":[4,9],\"label\":\"admin\"}",
      "{\"clients\":%s,\"pid\":0,\"ancestors\":[],\"label\":\"admin\"}",
      "{\"clients\":%s,\"pid\":0,\"ancestors\":[],\"label\":\"\"}",
  };
  const char *addresses[] = {"three", "nine", "nine", "one"};
  for (int i = 0; i < 4; i++) {
    char *req = NULL;
    asprintf(&req, chooses[i], clients);
    p = fx("choose", NULL, req, NULL, 0);
    EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "address"), addresses[i]));
    yyjson_doc_free(doc);
    proc_free(&p);
    free(req);
  }
  char *tied = replace_one(clients, "\"title\":\"admin\",\"focusHistoryID\":4",
                           "\"title\":\"other\",\"focusHistoryID\":0");
  char *req = NULL;
  asprintf(&req, "{\"clients\":%s,\"pid\":0,\"ancestors\":[],\"label\":\"\"}", tied);
  p = fx("choose", NULL, req, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "address"), "one"));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(req);
  free(tied);
  p = fx("choose", NULL,
         "{\"clients\":[{\"pid\":8,\"class\":\"other\",\"address\":\"eight\"}],\"ancestors\":[8]}",
         NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &doc), "address"), "eight"));
  yyjson_doc_free(doc);
  proc_free(&p);
}
static int mode_of(const char *path) {
  struct stat st;
  if (stat(path, &st))
    return -1;
  return (int)(st.st_mode & 0777);
}
static void test_cache(void) {
  begin("cache publication, lookup and malformed input");
  char *root = temp_dir("hf-");
  char *path = NULL;
  asprintf(&path, "%s/new/targets.json", root);
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *rootv = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, rootv);
  yyjson_mut_val *extra = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_bool(doc, extra, "kept", true);
  yyjson_mut_obj_add_val(doc, rootv, "extra", extra);
  yyjson_mut_val *entries = yyjson_mut_arr(doc);
  for (int i = 0; i < 110; i++) {
    char key[16], pane[16];
    snprintf(key, sizeof key, "%d", i);
    snprintf(pane, sizeof pane, "%d", i);
    yyjson_mut_val *e = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, e, "key", key);
    yyjson_mut_obj_add_strcpy(doc, e, "pane_id", pane);
    yyjson_mut_arr_add_val(entries, e);
  }
  yyjson_mut_obj_add_val(doc, rootv, "entries", entries);
  char *initial = yyjson_mut_write(doc, 0, NULL);
  yyjson_mut_doc_free(doc);
  EXPECT(!write_file(path, initial, strlen(initial)));
  free(initial);
  char *remember = NULL;
  asprintf(&remember, "{\"pane_id\":\"p\",\"socket\":\"sock\"}");
  const char *args[] = {"Ghostty", body, "12.5", path, NULL};
  Proc p = fx("remember", args, remember, NULL, 0);
  proc_free(&p);
  free(remember);
  size_t n = 0;
  char *saved = read_file(path, &n);
  yyjson_doc *got = yyjson_read(saved, n, 0);
  yyjson_val *ents = yyjson_obj_get(yyjson_doc_get_root(got), "entries");
  EXPECT(yyjson_arr_size(ents) == 100);
  EXPECT(str_eq(yyjson_obj_get(yyjson_arr_get(ents, 0), "key"), "11"));
  EXPECT(yyjson_is_true(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(got), "extra"), "kept")));
  EXPECT(mode_of(path) == 0600);
  char *tmp = NULL;
  asprintf(&tmp, "%s/new/targets.tmp", root);
  EXPECT(access(tmp, F_OK) != 0);
  free(tmp);
  yyjson_doc_free(got);
  free(saved);
  p = fx("remember", args, "{\"pane_id\":\"new\"}", NULL, 0);
  proc_free(&p);
  saved = read_file(path, &n);
  got = yyjson_read(saved, n, 0);
  EXPECT(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(got), "entries")) == 100);
  yyjson_doc_free(got);
  free(saved);
  p = fx("cache", args, NULL, NULL, 0);
  EXPECT(str_eq(yyjson_obj_get(fx_val(&p, &got), "pane_id"), "new"));
  yyjson_doc_free(got);
  proc_free(&p);
  const char *spaced[] = {"Ghostty ", body, "12.5", path, NULL};
  p = fx("cache", spaced, NULL, NULL, 0);
  got = parse_out(&p);
  EXPECT(!got || yyjson_is_null(yyjson_doc_get_root(got)));
  yyjson_doc_free(got);
  proc_free(&p);
  char *fresh = NULL;
  asprintf(&fresh, "%s/private/deep/targets.json", root);
  const char *fresh_args[] = {"s", "b", "t", fresh, NULL};
  p = fx("remember", fresh_args, "{\"workspace_id\":\"w\"}", NULL, 0);
  proc_free(&p);
  char *parent = NULL;
  asprintf(&parent, "%s/private/deep", root);
  EXPECT(mode_of(parent) == 0700);
  free(parent);
  free(fresh);
  free(path);
  rm_rf(root);
  free(root);
  root = temp_dir("hf-");
  asprintf(&path, "%s/targets.json", root);
  const char *bads[] = {"bad", "[]", "{\"entries\":1}"};
  for (int i = 0; i < 3; i++) {
    write_file(path, bads[i], strlen(bads[i]));
    const char *ra[] = {"s", "b", "t", path, NULL};
    p = fx("remember", ra, "{\"pane_id\":\"p\"}", NULL, 0);
    proc_free(&p);
    saved = read_file(path, &n);
    got = yyjson_read(saved, n, 0);
    EXPECT(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(got), "entries")) == 1);
    yyjson_doc_free(got);
    free(saved);
  }
  write_file(path, "{\"entries\":[4]}", strlen("{\"entries\":[4]}"));
  const char *ca[] = {"s", "b", "t", path, NULL};
  p = fx("cache", ca, NULL, NULL, 1);
  proc_free(&p);
  unsigned char badutf = 0xff;
  write_file(path, &badutf, 1);
  p = fx("remember", ca, "{\"pane_id\":\"p\"}", NULL, 1);
  proc_free(&p);
  saved = read_file(path, &n);
  EXPECT(n == 1 && (unsigned char)saved[0] == 0xff);
  free(saved);
  free(path);
  rm_rf(root);
  free(root);
}
static void test_paths_proc(void) {
  begin("socket discovery and proc metadata");
  char *root = temp_dir("hf-");
  char *base = NULL;
  asprintf(&base, "%s/config/herdr", root);
  const char *rels[] = {"herdr.sock", "sessions/z/herdr.sock", "sessions/a/herdr.sock",
                        "sessions/.hidden/herdr.sock"};
  for (size_t i = 0; i < 4; i++) {
    char *path = NULL;
    asprintf(&path, "%s/%s", base, rels[i]);
    write_file(path, "", 0);
    free(path);
  }
  char *home = NULL, *xdg = NULL, *override = NULL;
  asprintf(&home, "HOME=%s", root);
  asprintf(&xdg, "XDG_CONFIG_HOME=%s/config", root);
  asprintf(&override, "HERDR_SOCKET_PATH=%s/./herdr.sock", base);
  const char *env[] = {home, xdg, override, NULL};
  Proc p = fx("paths", NULL, NULL, env, 0);
  yyjson_doc *doc = NULL;
  yyjson_val *arr = fx_val(&p, &doc);
  char *expect[4];
  asprintf(&expect[0], "%s/herdr.sock", base);
  asprintf(&expect[1], "%s/sessions/.hidden/herdr.sock", base);
  asprintf(&expect[2], "%s/sessions/a/herdr.sock", base);
  asprintf(&expect[3], "%s/sessions/z/herdr.sock", base);
  EXPECT(yyjson_arr_size(arr) == 4);
  for (size_t i = 0; i < 4; i++)
    EXPECT(str_eq(yyjson_arr_get(arr, i), expect[i]));
  yyjson_doc_free(doc);
  proc_free(&p);
  for (int i = 0; i < 4; i++)
    free(expect[i]);
  char *ov = NULL;
  asprintf(&ov, "%s/override", root);
  write_file(ov, "", 0);
  char *ovenv = NULL;
  asprintf(&ovenv, "HERDR_SOCKET_PATH=%s", ov);
  const char *env2[] = {home, xdg, ovenv, NULL};
  p = fx("paths", NULL, NULL, env2, 0);
  EXPECT(str_eq(yyjson_arr_get(fx_val(&p, &doc), 0), ov));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(ovenv);
  free(ov);
  free(override);
  free(xdg);
  free(home);
  free(base);
  rm_rf(root);
  free(root);
  root = temp_dir("hf-");
  char *proc = NULL;
  asprintf(&proc, "%s/proc", root);
  char *stat9 = NULL, *stat8 = NULL;
  asprintf(&stat9, "%s/9/stat", proc);
  asprintf(&stat8, "%s/8/stat", proc);
  write_file(stat9, "9 (name with ) parens) S 8 0", strlen("9 (name with ) parens) S 8 0"));
  write_file(stat8, "8 (parent) S 9 0", strlen("8 (parent) S 9 0"));
  const char *anc[] = {"9", proc, NULL};
  p = fx("ancestors", anc, NULL, NULL, 0);
  yyjson_val *ids = fx_val(&p, &doc);
  EXPECT(yyjson_arr_size(ids) == 2 && yyjson_get_num(yyjson_arr_get(ids, 0)) == 9 &&
         yyjson_get_num(yyjson_arr_get(ids, 1)) == 8);
  yyjson_doc_free(doc);
  proc_free(&p);
  p = fx("parent", anc, NULL, NULL, 0);
  EXPECT(yyjson_get_num(fx_val(&p, &doc)) == 8);
  yyjson_doc_free(doc);
  proc_free(&p);
  for (int n = 1; n <= 40; n++) {
    char *path = NULL, *text = NULL;
    asprintf(&path, "%s/%d/stat", proc, n);
    asprintf(&text, "%d (p) S %d", n, n + 1);
    write_file(path, text, strlen(text));
    free(path);
    free(text);
  }
  const char *one[] = {"1", proc, NULL};
  p = fx("ancestors", one, NULL, NULL, 0);
  EXPECT(yyjson_arr_size(fx_val(&p, &doc)) == 32);
  yyjson_doc_free(doc);
  proc_free(&p);
  char *c42 = NULL, *c43 = NULL, *c44 = NULL;
  asprintf(&c42, "%s/42/cmdline", proc);
  asprintf(&c43, "%s/43/cmdline", proc);
  asprintf(&c44, "%s/44/cmdline", proc);
  write_file(c42, "herdr\0session\0attach\0default\0", 29);
  write_file(c43, "herdr\0session\0attach\0work\0", 26);
  write_file(c44, "other\0attach\0work\0", 18);
  const char *def[] = {"/x/herdr.sock", proc, NULL};
  p = fx("attached", def, NULL, NULL, 0);
  ids = fx_val(&p, &doc);
  EXPECT(yyjson_arr_size(ids) == 1 && yyjson_get_num(yyjson_arr_get(ids, 0)) == 42);
  yyjson_doc_free(doc);
  proc_free(&p);
  const char *work[] = {"/x/sessions/work/herdr.sock", proc, NULL};
  p = fx("attached", work, NULL, NULL, 0);
  ids = fx_val(&p, &doc);
  EXPECT(yyjson_arr_size(ids) == 1 && yyjson_get_num(yyjson_arr_get(ids, 0)) == 43);
  yyjson_doc_free(doc);
  proc_free(&p);
  const char *miss[] = {"/x/sessions/missing/herdr.sock", proc, NULL};
  p = fx("attached", miss, NULL, NULL, 0);
  ids = fx_val(&p, &doc);
  EXPECT(yyjson_arr_size(ids) == 2);
  double a = yyjson_get_num(yyjson_arr_get(ids, 0));
  double b = yyjson_get_num(yyjson_arr_get(ids, 1));
  EXPECT((a == 42 && b == 43) || (a == 43 && b == 42));
  yyjson_doc_free(doc);
  proc_free(&p);
  free(c42);
  free(c43);
  free(c44);
  free(stat9);
  free(stat8);
  free(proc);
  rm_rf(root);
  free(root);
}
typedef struct {
  char *root, *calls, *binpath;
  char *home_env, *xdg_env, *state_env, *sock_env, *path_env, *mode_env;
  pid_t peers[8];
  int npeers;
} Scene;
static void scene_init(Scene *s) {
  memset(s, 0, sizeof *s);
  s->root = temp_dir("hf-rpc-");
  asprintf(&s->calls, "%s/calls.jsonl", s->root);
  asprintf(&s->binpath, "%s/bin", s->root);
  mkdir(s->binpath, 0755);
  char *link = NULL;
  asprintf(&link, "%s/hyprctl", s->binpath);
  unlink(link);
  EXPECT(!symlink(hypr_bin, link));
  free(link);
  write_file(s->root[0] ? s->calls : s->calls, "", 0);
  char *clients = NULL;
  asprintf(&clients, "%s/clients.json", s->root);
  write_file(clients, "[]", 2);
  free(clients);
  asprintf(&s->home_env, "HOME=%s", s->root);
  asprintf(&s->xdg_env, "XDG_CONFIG_HOME=%s/config", s->root);
  asprintf(&s->state_env, "XDG_STATE_HOME=%s/state", s->root);
  asprintf(&s->sock_env, "HERDR_SOCKET_PATH=%s/none", s->root);
  char *oldpath = getenv("PATH");
  asprintf(&s->path_env, "PATH=%s%s%s", s->binpath, oldpath ? ":" : "", oldpath ? oldpath : "");
  s->mode_env = strdup("HYPR_FAKE_MODE=normal");
}
static void scene_free(Scene *s) {
  for (int i = 0; i < s->npeers; i++) {
    kill(s->peers[i], SIGTERM);
    waitpid(s->peers[i], NULL, 0);
  }
  rm_rf(s->root);
  free(s->root);
  free(s->calls);
  free(s->binpath);
  free(s->home_env);
  free(s->xdg_env);
  free(s->state_env);
  free(s->sock_env);
  free(s->path_env);
  free(s->mode_env);
}
static void scene_peer(Scene *s, const char *path, const char *snapshot, const char *mode) {
  char *dir = strdup(path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    char *copy = strdup(dir);
    for (char *p = copy + 1; *p; p++) {
      if (*p != '/')
        continue;
      *p = 0;
      mkdir(copy, 0755);
      *p = '/';
    }
    mkdir(copy, 0755);
    free(copy);
  }
  free(dir);
  char *snap_path = NULL;
  asprintf(&snap_path, "%s/snap-%d.json", s->root, s->npeers);
  write_file(snap_path, snapshot, strlen(snapshot));
  pid_t pid = fork();
  if (!pid) {
    char *argv[] = {peer_bin, (char *)path, (char *)mode, snap_path, s->calls, NULL};
    execv(peer_bin, argv);
    _exit(127);
  }
  free(snap_path);
  s->peers[s->npeers++] = pid;
  for (int i = 0; i < 100 && access(path, F_OK); i++) {
    struct timespec ts = {.tv_nsec = 20 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }
}
static const char **scene_env(Scene *s, const char *extra) {
  static const char *env[8];
  env[0] = s->home_env;
  env[1] = s->xdg_env;
  env[2] = s->state_env;
  env[3] = s->sock_env;
  env[4] = s->path_env;
  env[5] = s->mode_env;
  env[6] = extra;
  env[7] = NULL;
  return env;
}
static Proc scene_run(Scene *s, const char *exe, char **argv, int timeout) {
  return run_proc(exe, argv, "", 0, NULL, scene_env(s, NULL), timeout);
}
static char *cache_path(Scene *s) {
  char *path = NULL;
  asprintf(&path, "%s/state/herdr-notification-focus/targets.json", s->root);
  return path;
}
static size_t load_lines(const char *path, yyjson_doc ***out) {
  size_t n = 0;
  char *text = read_file(path, &n);
  *out = NULL;
  size_t count = 0;
  if (!text)
    return 0;
  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    yyjson_doc *doc = yyjson_read(line, strlen(line), YYJSON_READ_ALLOW_INF_AND_NAN |
                                                           YYJSON_READ_BIGNUM_AS_RAW);
    if (!doc)
      continue;
    *out = realloc(*out, (count + 1) * sizeof **out);
    (*out)[count++] = doc;
  }
  free(text);
  return count;
}
static void free_docs(yyjson_doc **docs, size_t n) {
  for (size_t i = 0; i < n; i++)
    yyjson_doc_free(docs[i]);
  free(docs);
}
static void test_cli(const char *exe) {
  Scene s;
  scene_init(&s);
  char *first = NULL, *second = NULL;
  asprintf(&first, "%s/first.sock", s.root);
  asprintf(&second, "%s/config/herdr/herdr.sock", s.root);
  scene_peer(&s, first, "{\"workspaces\":[]}", "normal");
  scene_peer(&s, second, snap, "normal");
  char *sock = NULL;
  asprintf(&sock, "HERDR_SOCKET_PATH=%s", first);
  char *argv[] = {(char *)exe, "remember", "--summary", "Ghostty", "--body", (char *)body,
                  "--ts", "12.5", NULL};
  const char *env[] = {s.home_env, s.xdg_env, s.state_env, sock, s.path_env, s.mode_env, NULL};
  Proc r = run_proc(exe, argv, "", 0, NULL, env, 15000);
  if (r.error || r.status != 0)
    FAIL("remember status %d %s", r.status, r.err ? (char *)r.err : "");
  yyjson_doc **calls = NULL;
  size_t nc = load_lines(s.calls, &calls);
  EXPECT(nc == 2);
  if (nc >= 2) {
    EXPECT(str_eq(yyjson_obj_get(yyjson_doc_get_root(calls[0]), "method"), "session.snapshot"));
    EXPECT(str_eq(yyjson_obj_get(yyjson_doc_get_root(calls[0]), "id"), "omapager-herdr-focus"));
  }
  char *cache = cache_path(&s);
  size_t n = 0;
  char *saved = read_file(cache, &n);
  yyjson_doc *doc = saved ? yyjson_read(saved, n, 0) : NULL;
  yyjson_val *entry = doc ? yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "entries"), 0) : NULL;
  EXPECT(str_eq(yyjson_obj_get(entry, "pane_id"), "p2"));
  if (nc >= 2)
    EXPECT(str_eq(yyjson_obj_get(entry, "socket"),
                  yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(calls[1]), "path"))));
  yyjson_doc **actions = NULL;
  char *actions_path = NULL;
  asprintf(&actions_path, "%s/actions", s.root);
  size_t na = load_lines(actions_path, &actions);
  EXPECT(na == 0);
  free_docs(actions, na);
  free(actions_path);
  free_docs(calls, nc);
  yyjson_doc_free(doc);
  free(saved);
  free(cache);
  proc_free(&r);
  free(sock);
  free(first);
  free(second);
  scene_free(&s);
}
static void test_wrapped(const char *exe) {
  begin("whitespace-wrapped CLI toasts");
  Scene s;
  scene_init(&s);
  char *path = NULL;
  asprintf(&path, "%s/config/herdr/herdr.sock", s.root);
  scene_peer(&s, path, snap, "normal");
  char *wrapped = NULL;
  asprintf(&wrapped, "\v%s\v", body);
  char *summary = strdup("\vGhostty\v");
  char *argv[] = {(char *)exe, "remember", "--summary", summary, "--body", wrapped, "--ts",
                  "space", NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  if (r.error || r.status != 0)
    FAIL("wrapped status %d %s", r.status, r.err ? (char *)r.err : "");
  yyjson_doc **calls = NULL;
  EXPECT(load_lines(s.calls, &calls) == 1);
  free_docs(calls, 1);
  char *cache = cache_path(&s);
  size_t n = 0;
  char *saved = read_file(cache, &n);
  yyjson_doc *doc = yyjson_read(saved, n, 0);
  EXPECT(str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "entries"), 0),
                               "pane_id"),
                "p2"));
  yyjson_doc_free(doc);
  free(saved);
  free(cache);
  char *actions_path = NULL;
  asprintf(&actions_path, "%s/actions", s.root);
  EXPECT(access(actions_path, F_OK) != 0);
  free(actions_path);
  proc_free(&r);
  free(summary);
  free(wrapped);
  free(path);
  scene_free(&s);
}
static void test_unrelated(const char *exe) {
  begin("unrelated toasts do not contact peers");
  Scene s;
  scene_init(&s);
  char *path = NULL;
  asprintf(&path, "%s/config/herdr/herdr.sock", s.root);
  scene_peer(&s, path, snap, "normal");
  char *argv[] = {(char *)exe, "remember", "--summary", "Slack", "--body", (char *)body, NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  EXPECT(!r.error && r.status == 0);
  yyjson_doc **calls = NULL;
  EXPECT(load_lines(s.calls, &calls) == 0);
  free_docs(calls, 0);
  char *cache = cache_path(&s);
  EXPECT(access(cache, F_OK) != 0);
  char *actions_path = NULL;
  asprintf(&actions_path, "%s/actions", s.root);
  EXPECT(access(actions_path, F_OK) != 0);
  free(actions_path);
  free(cache);
  proc_free(&r);
  free(path);
  scene_free(&s);
}
static void write_clients(Scene *s, const char *json) {
  char *path = NULL;
  asprintf(&path, "%s/clients.json", s->root);
  write_file(path, json, strlen(json));
  free(path);
}
static void test_open_live(const char *exe) {
  begin("open reuses a live saved pane");
  Scene s;
  scene_init(&s);
  char *path = NULL;
  asprintf(&path, "%s/saved.sock", s.root);
  char *changed = replace_one(snap, "\"agent_status\":\"working\"", "\"agent_status\":\"blocked\"");
  scene_peer(&s, path, changed, "normal");
  char *cache = cache_path(&s);
  char *entry = NULL;
  asprintf(&entry, "{\"entries\":[{\"key\":\"12.5|Ghostty|%s\",\"pane_id\":\"p1\",\"socket\":\"%s\"}]}",
           body, path);
  write_file(cache, entry, strlen(entry));
  write_clients(&s, "[{\"pid\":7,\"class\":\"Ghostty\",\"address\":\"0xabc\",\"title\":\"admin\"}]");
  char *argv[] = {(char *)exe, "open", "--summary", "Ghostty", "--body", (char *)body, "--ts",
                  "12.5", NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  if (r.error || r.status != 0)
    FAIL("open status %d %s", r.status, r.err ? (char *)r.err : "");
  yyjson_doc **calls = NULL;
  size_t nc = load_lines(s.calls, &calls);
  EXPECT(nc == 2);
  if (nc == 2) {
    EXPECT(str_eq(yyjson_obj_get(yyjson_doc_get_root(calls[0]), "method"), "session.snapshot"));
    EXPECT(str_eq(yyjson_obj_get(yyjson_doc_get_root(calls[1]), "method"), "pane.focus"));
    EXPECT(str_eq(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(calls[1]), "params"), "pane_id"),
                  "p1"));
  }
  free_docs(calls, nc);
  char *actions_path = NULL;
  asprintf(&actions_path, "%s/actions", s.root);
  yyjson_doc **actions = NULL;
  size_t na = load_lines(actions_path, &actions);
  EXPECT(na == 2);
  if (na == 2) {
    EXPECT(str_eq(yyjson_arr_get(yyjson_doc_get_root(actions[0]), 0), "clients"));
    EXPECT(str_eq(yyjson_arr_get(yyjson_doc_get_root(actions[1]), 0), "dispatch"));
    EXPECT(str_eq(yyjson_arr_get(yyjson_doc_get_root(actions[1]), 2), "address:0xabc"));
  }
  free_docs(actions, na);
  free(actions_path);
  proc_free(&r);
  free(entry);
  free(cache);
  free(changed);
  free(path);
  scene_free(&s);
}
static void test_stale(const char *exe) {
  begin("stale saved targets re-resolve before another socket");
  Scene s;
  scene_init(&s);
  char *path = NULL;
  asprintf(&path, "%s/saved.sock", s.root);
  scene_peer(&s, path, snap, "normal");
  char *cache = cache_path(&s);
  char *entry = NULL;
  asprintf(&entry, "{\"entries\":[{\"key\":\"12.5|Ghostty|%s\",\"pane_id\":\"gone\",\"socket\":\"%s\"}]}",
           body, path);
  write_file(cache, entry, strlen(entry));
  char *argv[] = {(char *)exe, "open", "--summary", "Ghostty", "--body", (char *)body, "--ts",
                  "12.5", NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  EXPECT(!r.error && r.status == 0);
  yyjson_doc **calls = NULL;
  size_t nc = load_lines(s.calls, &calls);
  EXPECT(nc == 2);
  if (nc == 2) {
    EXPECT(str_eq(yyjson_obj_get(yyjson_doc_get_root(calls[0]), "method"), "session.snapshot"));
    EXPECT(str_eq(yyjson_obj_get(yyjson_doc_get_root(calls[1]), "method"), "pane.focus"));
    EXPECT(str_eq(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(calls[1]), "params"), "pane_id"),
                  "p2"));
  }
  free_docs(calls, nc);
  proc_free(&r);
  free(entry);
  free(cache);
  free(path);
  scene_free(&s);
}
static void test_other_session(const char *exe) {
  begin("missing cached workspace falls back; renamed tabs focus workspace");
  Scene s;
  scene_init(&s);
  char *old = NULL, *current = NULL;
  asprintf(&old, "%s/old.sock", s.root);
  asprintf(&current, "%s/config/herdr/herdr.sock", s.root);
  scene_peer(&s, old, "{\"workspaces\":[]}", "normal");
  scene_peer(&s, current, snap, "normal");
  char *cache = cache_path(&s);
  char *entry = NULL;
  asprintf(&entry,
           "{\"entries\":[{\"key\":\"12.5|Ghostty|%s\",\"workspace_id\":\"gone\",\"socket\":\"%s\"}]}",
           body, old);
  write_file(cache, entry, strlen(entry));
  char *argv[] = {(char *)exe, "open", "--summary", "Ghostty", "--body", (char *)body, "--ts",
                  "12.5", NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  EXPECT(!r.error && r.status == 0);
  yyjson_doc **calls = NULL;
  size_t nc = load_lines(s.calls, &calls);
  EXPECT(nc > 0 && str_eq(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(calls[nc - 1]), "params"),
                                         "pane_id"),
                          "p2"));
  free_docs(calls, nc);
  proc_free(&r);
  char *renamed = replace_one(body, "2 hadleigh review", "renamed");
  char *argv2[] = {(char *)exe, "open", "--summary", "Ghostty", "--body", renamed, "--ts", "12.5",
                   NULL};
  r = scene_run(&s, exe, argv2, 15000);
  EXPECT(!r.error && r.status == 0);
  nc = load_lines(s.calls, &calls);
  EXPECT(nc > 0 && str_eq(yyjson_obj_get(yyjson_doc_get_root(calls[nc - 1]), "method"),
                          "workspace.focus"));
  free_docs(calls, nc);
  proc_free(&r);
  free(renamed);
  free(entry);
  free(cache);
  free(old);
  free(current);
  scene_free(&s);
}
static void test_sender(const char *exe) {
  begin("unrelated open uses argv without a shell");
  Scene s;
  scene_init(&s);
  const char *address = "0xabc; touch /tmp/not-a-command";
  char *json = NULL;
  asprintf(&json,
           "[{\"pid\":7,\"class\":\"other\",\"address\":\"%s\"},{\"pid\":8,\"class\":\"Ghostty\",\"address\":\"other\"}]",
           address);
  write_clients(&s, json);
  char *argv[] = {(char *)exe, "open", "--summary", "Slack", "--pid", "7", NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  EXPECT(!r.error && r.status == 0);
  yyjson_doc **calls = NULL;
  EXPECT(load_lines(s.calls, &calls) == 0);
  free_docs(calls, 0);
  char *actions_path = NULL;
  asprintf(&actions_path, "%s/actions", s.root);
  yyjson_doc **actions = NULL;
  size_t na = load_lines(actions_path, &actions);
  EXPECT(na > 0);
  if (na) {
    yyjson_val *last = yyjson_doc_get_root(actions[na - 1]);
    EXPECT(str_eq(yyjson_arr_get(last, 0), "dispatch"));
    EXPECT(str_eq(yyjson_arr_get(last, 1), "focuswindow"));
    char *want = NULL;
    asprintf(&want, "address:%s", address);
    EXPECT(str_eq(yyjson_arr_get(last, 2), want));
    free(want);
  }
  free_docs(actions, na);
  free(actions_path);
  proc_free(&r);
  free(json);
  scene_free(&s);
}
static void test_framing(const char *exe) {
  begin("fragmented replies, EOF, BOM and trailing messages");
  const char *modes[] = {"chunks", "eof", "bom"};
  char *pad = malloc(200001);
  memset(pad, 'x', 200000);
  pad[200000] = 0;
  char *snapshot = NULL;
  asprintf(&snapshot, "{\"workspaces\":%s,\"padding\":\"%s\"}",
           "{\"workspaces\":[{\"workspace_id\":\"w3\"}]}", pad);
  /* The padding case must still resolve the real snapshot, so use snap plus padding. */
  free(snapshot);
  asprintf(&snapshot,
           "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\",\"number\":1},"
           "{\"workspace_id\":\"w3\",\"label\":\"admin\",\"number\":3}],\"tabs\":["
           "{\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"label\":\"2 hadleigh review\"}],"
           "\"agents\":[{\"agent\":\"pi\",\"agent_status\":\"done\",\"pane_id\":\"p2\","
           "\"tab_id\":\"t2\",\"workspace_id\":\"w3\",\"state_change_seq\":9}],\"padding\":\"%s\"}",
           pad);
  for (int i = 0; i < 3; i++) {
    Scene s;
    scene_init(&s);
    char *path = NULL;
    asprintf(&path, "%s/config/herdr/herdr.sock", s.root);
    scene_peer(&s, path, snapshot, modes[i]);
    char *argv[] = {(char *)exe, "remember", "--summary", "Ghostty", "--body", (char *)body,
                    "--ts", "12.5", NULL};
    Proc r = scene_run(&s, exe, argv, 20000);
    if (r.error || r.status != 0)
      FAIL("%s status %d %s", modes[i], r.status, r.err ? (char *)r.err : "");
    yyjson_doc **calls = NULL;
    EXPECT(load_lines(s.calls, &calls) == 1);
    free_docs(calls, 1);
    char *cache = cache_path(&s);
    size_t n = 0;
    char *saved = read_file(cache, &n);
    yyjson_doc *doc = saved ? yyjson_read(saved, n, 0) : NULL;
    yyjson_val *entry =
        doc ? yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "entries"), 0) : NULL;
    EXPECT(str_eq(yyjson_obj_get(entry, "pane_id"), "p2"));
    yyjson_doc_free(doc);
    free(saved);
    free(cache);
    proc_free(&r);
    free(path);
    scene_free(&s);
  }
  free(snapshot);
  free(pad);
}
static void test_progress(const char *exe) {
  begin("progressing socket replies exceed two seconds");
  Scene s;
  scene_init(&s);
  char *path = NULL;
  asprintf(&path, "%s/config/herdr/herdr.sock", s.root);
  scene_peer(&s, path,
             "{\"workspaces\":[{\"workspace_id\":\"w3\",\"label\":\"admin\",\"number\":3}],\"tabs\":[],\"agents\":[]}",
             "progress");
  char *argv[] = {(char *)exe, "remember", "--summary", "Ghostty", "--body", (char *)body, "--ts",
                  "12.5", NULL};
  int64_t before = now_ms();
  Proc r = scene_run(&s, exe, argv, 15000);
  EXPECT(now_ms() - before > 2000);
  EXPECT(!r.error && r.status == 0);
  char *cache = cache_path(&s);
  size_t n = 0;
  char *saved = read_file(cache, &n);
  yyjson_doc *doc = saved ? yyjson_read(saved, n, 0) : NULL;
  yyjson_val *entry =
      doc ? yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "entries"), 0) : NULL;
  EXPECT(str_eq(yyjson_obj_get(entry, "workspace_id"), "w3"));
  yyjson_doc_free(doc);
  free(saved);
  free(cache);
  proc_free(&r);
  free(path);
  scene_free(&s);
}
static void test_bad_peers(const char *exe) {
  begin("empty peers are harmless; malformed replies and connection errors stop");
  const char *modes[] = {"empty", "bad"};
  for (int i = 0; i < 2; i++) {
    Scene s;
    scene_init(&s);
    char *path = NULL;
    asprintf(&path, "%s/config/herdr/herdr.sock", s.root);
    scene_peer(&s, path, snap, modes[i]);
    char *argv[] = {(char *)exe, "remember", "--summary", "Ghostty", "--body", (char *)body,
                    "--ts", "12.5", NULL};
    Proc r = scene_run(&s, exe, argv, 15000);
    EXPECT(!r.error && r.status == (!strcmp(modes[i], "empty") ? 0 : 1));
    char *cache = cache_path(&s);
    EXPECT(access(cache, F_OK) != 0);
    free(cache);
    proc_free(&r);
    free(path);
    scene_free(&s);
  }
  Scene s;
  scene_init(&s);
  char *path = NULL;
  asprintf(&path, "%s/config/herdr/herdr.sock", s.root);
  write_file(path, "not a socket", 12);
  char *argv[] = {(char *)exe, "remember", "--summary", "Ghostty", "--body", (char *)body, "--ts",
                  "12.5", NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  EXPECT(!r.error && r.status == 1);
  proc_free(&r);
  free(path);
  scene_free(&s);
}
static void test_idle(const char *exe) {
  begin("socket idle timeout retains existing cache");
  Scene s;
  scene_init(&s);
  char *path = NULL;
  asprintf(&path, "%s/config/herdr/herdr.sock", s.root);
  scene_peer(&s, path, snap, "stall");
  char *cache = cache_path(&s);
  write_file(cache, "{\"entries\":[{\"key\":\"old\",\"pane_id\":\"old\"}]}",
             strlen("{\"entries\":[{\"key\":\"old\",\"pane_id\":\"old\"}]}"));
  char *argv[] = {(char *)exe, "remember", "--summary", "Ghostty", "--body", (char *)body, "--ts",
                  "12.5", NULL};
  int64_t before = now_ms();
  Proc r = scene_run(&s, exe, argv, 10000);
  int64_t elapsed = now_ms() - before;
  EXPECT(!r.error && r.status == 1);
  EXPECT(elapsed >= 1800 && elapsed < 4500);
  size_t n = 0;
  char *saved = read_file(cache, &n);
  yyjson_doc *doc = yyjson_read(saved, n, 0);
  EXPECT(str_eq(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "entries"), 0),
                               "key"),
                "old"));
  yyjson_doc_free(doc);
  free(saved);
  proc_free(&r);
  free(cache);
  free(path);
  scene_free(&s);
}
static void test_hypr(const char *exe) {
  begin("Hyprland probe failures and focus timeout");
  const char *modes[] = {"bad", "nul", "invalid", "exit", "stall", "dispatch-stall"};
  for (size_t i = 0; i < 6; i++) {
    Scene s;
    scene_init(&s);
    free(s.mode_env);
    asprintf(&s.mode_env, "HYPR_FAKE_MODE=%s", modes[i]);
    write_clients(&s, "[{\"pid\":7,\"class\":\"Ghostty\",\"address\":\"a\"}]");
    char *argv[] = {(char *)exe, "open", "--pid", "7", NULL};
    Proc r = scene_run(&s, exe, argv, 12000);
    int want = (!strcmp(modes[i], "dispatch-stall") || !strcmp(modes[i], "invalid")) ? 1 : 0;
    if (r.error || r.status != want)
      FAIL("mode %s status %d wanted %d %s", modes[i], r.status, want,
           r.err ? (char *)r.err : "");
    proc_free(&r);
    scene_free(&s);
  }
}
static void test_nul_address(const char *exe) {
  begin("embedded NUL compositor addresses fail before dispatch");
  Scene s;
  scene_init(&s);
  write_clients(&s, "[{\"pid\":7,\"class\":\"Ghostty\",\"address\":\"0xabc\\u0000suffix\"}]");
  char *argv[] = {(char *)exe, "open", "--pid", "7", NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  if (r.error || r.status != 1)
    FAIL("nul address status %d %s", r.status, r.err ? (char *)r.err : "");
  char *actions_path = NULL;
  asprintf(&actions_path, "%s/actions", s.root);
  yyjson_doc **actions = NULL;
  size_t na = load_lines(actions_path, &actions);
  EXPECT(na == 1 && str_eq(yyjson_arr_get(yyjson_doc_get_root(actions[0]), 0), "clients"));
  free_docs(actions, na);
  free(actions_path);
  proc_free(&r);
  scene_free(&s);
}
static void test_bad_pid(const char *exe) {
  begin("invalid digit-like sender PIDs fail");
  Scene s;
  scene_init(&s);
  write_clients(&s, "[{\"pid\":7,\"class\":\"Ghostty\",\"address\":\"a\"}]");
  char *argv[] = {(char *)exe, "open", "--pid", "²", NULL};
  Proc r = scene_run(&s, exe, argv, 15000);
  EXPECT(!r.error && r.status == 1);
  char *actions_path = NULL;
  asprintf(&actions_path, "%s/actions", s.root);
  EXPECT(access(actions_path, F_OK) != 0);
  free(actions_path);
  proc_free(&r);
  scene_free(&s);
}
static void test_syntax(const char *exe) {
  begin("CLI syntax and option abbreviations");
  struct {
    int n;
    const char *args[8];
    int status;
  } cases[] = {
      {0, {NULL}, 2},
      {1, {"bad"}, 2},
      {2, {"remember", "--bad"}, 2},
      {2, {"remember", "--body"}, 2},
      {3, {"remember", "--pid", "-g"}, 2},
      {1, {"--help"}, 0},
      {2, {"remember", "--help"}, 0},
      {5, {"remember", "--sum=Slack", "--bo=foo", "--t=3", "--p=-1"}, 0},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    Scene s;
    scene_init(&s);
    char *argv[10];
    argv[0] = (char *)exe;
    for (int j = 0; j < cases[i].n; j++)
      argv[j + 1] = (char *)cases[i].args[j];
    argv[cases[i].n + 1] = NULL;
    Proc r = scene_run(&s, exe, argv, 15000);
    if (r.error || r.status != cases[i].status)
      FAIL("syntax %zu status %d wanted %d %s", i, r.status, cases[i].status,
           r.err ? (char *)r.err : "");
    proc_free(&r);
    scene_free(&s);
  }
}
static void each(void (*fn)(const char *)) {
  fn(binary);
  const char *legacy = getenv("HERDR_FOCUS_LEGACY");
  if (legacy && *legacy)
    fn(legacy);
}
int main(void) {
  binary = sibling("HERDR_FOCUS_BIN", "herdr-notification-focus");
  fixture = sibling("HERDR_FOCUS_FIXTURE", "focus-fixture");
  peer_bin = sibling(NULL, "focus-peer");
  hypr_bin = sibling(NULL, "hyprfake");
  if (!binary || !fixture || !peer_bin || !hypr_bin || access(binary, X_OK) ||
      access(fixture, X_OK) || access(peer_bin, X_OK) || access(hypr_bin, X_OK)) {
    fprintf(stderr, "missing herdr focus binaries\n");
    return 2;
  }
  test_formats();
  test_whitespace();
  test_preference();
  test_workspace();
  test_fallback();
  test_numbers();
  test_malformed_groups();
  test_alive_payload_choose();
  test_cache();
  test_paths_proc();
  begin("remember resolves through ordered sockets");
  each(test_cli);
  each(test_wrapped);
  each(test_unrelated);
  each(test_open_live);
  each(test_stale);
  each(test_other_session);
  each(test_sender);
  each(test_framing);
  each(test_progress);
  each(test_bad_peers);
  each(test_idle);
  each(test_hypr);
  each(test_nul_address);
  each(test_bad_pid);
  each(test_syntax);
  free(binary);
  free(fixture);
  free(peer_bin);
  free(hypr_bin);
  if (!failures)
    fprintf(stderr, "ok herdr-focus-checks\n");
  return failures ? 1 : 0;
}
