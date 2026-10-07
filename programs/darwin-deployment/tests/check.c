#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>
#include <stdbool.h>
#include <yyjson.h>

static const char *current_test;
static int failures;
static char *fixture;
static char *bin_dir;
static char *root;
static int seqno;

static void failf(const char *file, int line, const char *fmt, ...) {
  fprintf(stderr, "FAIL %s (%s:%d): ", current_test, file, line);
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

static int began;
static void begin(const char *name) {
  current_test = name;
  began = failures;
  printf("RUN %s\n", name);
  fflush(stdout);
}
static void end_case(void) {
  if (failures == began)
    printf("PASS %s\n", current_test);
}

static char *sibling_dir(void) {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n < 0)
    return NULL;
  exe[n] = 0;
  return g_path_get_dirname(exe);
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct {
  int status;
  char *out;
  size_t out_len;
  char *err;
  size_t err_len;
  bool spawn_error;
} Proc;

static void proc_clear(Proc *p) {
  g_free(p->out);
  g_free(p->err);
  memset(p, 0, sizeof *p);
}

static bool grow(char **buf, size_t *len, size_t *cap, const void *add, size_t n) {
  if (*len > SIZE_MAX - n - 1)
    return false;
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1) {
      if (next > SIZE_MAX / 2)
        return false;
      next *= 2;
    }
    char *p = g_try_realloc(*buf, next);
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

static Proc proc_run(const char *cmd, char *const *argv, const void *input,
                     size_t input_len, int timeout_ms) {
  Proc p = {.status = -1};
  int outp[2], errp[2], inp[2];
  if (pipe(outp) || pipe(errp) || pipe(inp)) {
    p.spawn_error = true;
    return p;
  }
  pid_t pid = fork();
  if (pid < 0) {
    p.spawn_error = true;
    return p;
  }
  if (pid == 0) {
    dup2(inp[0], 0);
    dup2(outp[1], 1);
    dup2(errp[1], 2);
    close(inp[0]);
    close(inp[1]);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    execvp(cmd, argv);
    _exit(127);
  }
  close(inp[0]);
  close(outp[1]);
  close(errp[1]);
  if (!input || input_len == 0) {
    close(inp[1]);
    inp[1] = -1;
  }
  size_t in_off = 0, ocap = 0, ecap = 0;
  int64_t deadline = now_ms() + timeout_ms;
  bool done = false;
  while (!done) {
    if (now_ms() > deadline) {
      kill(pid, SIGKILL);
      p.spawn_error = true;
      break;
    }
    struct pollfd fds[3];
    nfds_t nfd = 0;
    int ii = -1, oi = -1, ei = -1;
    if (inp[1] >= 0 && input && in_off < input_len) {
      ii = (int)nfd;
      fds[nfd++] = (struct pollfd){inp[1], POLLOUT, 0};
    }
    if (outp[0] >= 0) {
      oi = (int)nfd;
      fds[nfd++] = (struct pollfd){outp[0], POLLIN, 0};
    }
    if (errp[0] >= 0) {
      ei = (int)nfd;
      fds[nfd++] = (struct pollfd){errp[0], POLLIN, 0};
    }
    int left = (int)(deadline - now_ms());
    if (left < 0)
      left = 0;
    if (left > 50)
      left = 50;
    poll(fds, nfd, left);
    if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
      ssize_t w = write(inp[1], (const char *)input + in_off, input_len - in_off);
      if (w > 0)
        in_off += (size_t)w;
      else
        in_off = input_len;
    } else if (inp[1] >= 0 && (!input || in_off >= input_len)) {
      close(inp[1]);
      inp[1] = -1;
    }
    char tmp[4096];
    if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP))) {
      ssize_t r = read(outp[0], tmp, sizeof tmp);
      if (r > 0)
        grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r);
      else {
        close(outp[0]);
        outp[0] = -1;
      }
    }
    if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP))) {
      ssize_t r = read(errp[0], tmp, sizeof tmp);
      if (r > 0)
        grow(&p.err, &p.err_len, &ecap, tmp, (size_t)r);
      else {
        close(errp[0]);
        errp[0] = -1;
      }
    }
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == pid) {
      done = true;
      p.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
  }
  if (inp[1] >= 0)
    close(inp[1]);
  if (!done) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
  if (outp[0] >= 0) {
    char tmp[4096];
    ssize_t r;
    while ((r = read(outp[0], tmp, sizeof tmp)) > 0)
      grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r);
    close(outp[0]);
  }
  if (errp[0] >= 0) {
    char tmp[4096];
    ssize_t r;
    while ((r = read(errp[0], tmp, sizeof tmp)) > 0)
      grow(&p.err, &p.err_len, &ecap, tmp, (size_t)r);
    close(errp[0]);
  }
  if (!p.out)
    grow(&p.out, &p.out_len, &ocap, "", 0);
  if (!p.err)
    grow(&p.err, &p.err_len, &ecap, "", 0);
  return p;
}

static char *directory(void) {
  char *p = g_strdup_printf("%s/%d", root, seqno++);
  if (g_mkdir_with_parents(p, 0700) != 0) {
    g_free(p);
    return NULL;
  }
  return p;
}

static bool write_file(const char *path, const void *data, size_t len, mode_t mode) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0)
    return false;
  const char *s = data;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, s + off, len - off);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      return false;
    }
    off += (size_t)n;
  }
  if (fchmod(fd, mode) != 0) {
    close(fd);
    return false;
  }
  return close(fd) == 0;
}

static bool read_file(const char *path, char **data, size_t *len) {
  gsize n = 0;
  if (!g_file_get_contents(path, data, &n, NULL))
    return false;
  *len = n;
  return true;
}

static bool contains_len(const char *hay, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (m > n)
    return false;
  for (size_t i = 0; i + m <= n; i++)
    if (!memcmp(hay + i, needle, m))
      return true;
  return false;
}

static bool json_eq(yyjson_val *a, yyjson_val *b, const char *skip);

static bool json_eq(yyjson_val *a, yyjson_val *b, const char *skip) {
  if (!a || !b)
    return false;
  if (yyjson_is_null(a) || yyjson_is_null(b))
    return yyjson_is_null(a) && yyjson_is_null(b);
  if (yyjson_is_bool(a) || yyjson_is_bool(b))
    return yyjson_is_bool(a) && yyjson_is_bool(b) &&
           yyjson_get_bool(a) == yyjson_get_bool(b);
  if (yyjson_is_num(a) || yyjson_is_num(b)) {
    if (!yyjson_is_num(a) || !yyjson_is_num(b))
      return false;
    if (yyjson_is_real(a) || yyjson_is_real(b))
      return yyjson_get_real(a) == yyjson_get_real(b);
    return yyjson_get_sint(a) == yyjson_get_sint(b);
  }
  if (yyjson_is_str(a) || yyjson_is_str(b)) {
    if (!yyjson_is_str(a) || !yyjson_is_str(b))
      return false;
    size_t an = yyjson_get_len(a), bn = yyjson_get_len(b);
    return an == bn && !memcmp(yyjson_get_str(a), yyjson_get_str(b), an);
  }
  if (yyjson_is_arr(a) || yyjson_is_arr(b)) {
    if (!yyjson_is_arr(a) || !yyjson_is_arr(b) || yyjson_arr_size(a) != yyjson_arr_size(b))
      return false;
    size_t i, n;
    yyjson_val *x, *y;
    yyjson_arr_foreach(a, i, n, x) {
      y = yyjson_arr_get(b, i);
      if (!json_eq(x, y, NULL))
        return false;
    }
    return true;
  }
  if (!yyjson_is_obj(a) || !yyjson_is_obj(b))
    return false;
  size_t i, n, count = 0;
  yyjson_val *k, *v;
  yyjson_obj_foreach(a, i, n, k, v) {
    if (skip && yyjson_is_str(k) && !strcmp(yyjson_get_str(k), skip))
      continue;
    count++;
    yyjson_val *other = yyjson_obj_getn(b, yyjson_get_str(k), yyjson_get_len(k));
    if (!other || !json_eq(v, other, NULL))
      return false;
  }
  size_t j, m, other_count = 0;
  yyjson_obj_foreach(b, j, m, k, v) {
    if (skip && yyjson_is_str(k) && !strcmp(yyjson_get_str(k), skip))
      continue;
    other_count++;
  }
  return count == other_count;
}

static yyjson_mut_val *resp(yyjson_mut_doc *d, int code, const char *out, bool error) {
  yyjson_mut_val *o = yyjson_mut_obj(d);
  yyjson_mut_obj_add_int(d, o, "code", code);
  yyjson_mut_obj_add_strcpy(d, o, "out", out);
  if (error)
    yyjson_mut_obj_add_bool(d, o, "error", true);
  return o;
}

static yyjson_mut_doc *health_doc(void) {
  yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *rootv = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, rootv);
  yyjson_mut_obj_add_strcpy(d, rootv, "op", "health");
  yyjson_mut_val *config = yyjson_mut_obj(d);
  yyjson_mut_obj_add_val(d, rootv, "config", config);
  yyjson_mut_val *services = yyjson_mut_arr(d);
  const char *labels[] = {"app", "sops", "ssh"};
  const char *kinds[] = {"running", "oneshot", "socket"};
  for (size_t i = 0; i < 3; i++) {
    yyjson_mut_val *s = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, s, "label", labels[i]);
    yyjson_mut_obj_add_strcpy(d, s, "kind", kinds[i]);
    yyjson_mut_arr_add_val(services, s);
  }
  yyjson_mut_obj_add_val(d, config, "services", services);
  yyjson_mut_val *ready = yyjson_mut_arr(d);
  yyjson_mut_arr_add_strcpy(d, ready, "wait");
  yyjson_mut_arr_add_strcpy(d, ready, "--timeout");
  yyjson_mut_arr_add_strcpy(d, ready, "0");
  yyjson_mut_obj_add_val(d, config, "readiness", ready);
  yyjson_mut_val *responses = yyjson_mut_obj(d);
  yyjson_mut_obj_add_val(d, rootv, "responses", responses);
  yyjson_mut_obj_add_val(d, responses, "/bin/launchctl print system/app",
                         resp(d, 0,
                              "state = running\nlast exit code = 75: EX_TEMPFAIL\nEnvironmentVariables = { TOKEN = mock-sensitive-data; }",
                              false));
  yyjson_mut_obj_add_val(d, responses, "/bin/launchctl print system/sops",
                         resp(d, 0, "state = not running\nlast exit code = 0\n", false));
  yyjson_mut_obj_add_val(d, responses, "/bin/launchctl print system/ssh",
                         resp(d, 0, "state = not running\nlast exit code = 1\n", false));
  yyjson_mut_obj_add_val(d, responses, "wait --timeout 0",
                         resp(d, 0, "private readiness output", false));
  yyjson_mut_obj_add_val(d, responses, "/usr/bin/vm_stat",
                         resp(d, 0,
                              "Pages free: 42.\nPages active: 99.\nPages wired down: 10.\nPages occupied by compressor: 11.\nIgnore: private metric\n",
                              false));
  yyjson_mut_obj_add_val(d, responses, "/usr/sbin/sysctl -n vm.swapusage",
                         resp(d, 0, "total = 0.00M used = 0.00M free = 0.00M", false));
  return d;
}

static yyjson_doc *invoke_doc(yyjson_mut_doc *input, const char *stdin_data) {
  char *file = g_strdup_printf("%s/request-%d.json", root, seqno++);
  size_t len = 0;
  char *text = yyjson_mut_write(input, 0, &len);
  if (!text || !write_file(file, text, len, 0600)) {
    free(text);
    g_free(file);
    return NULL;
  }
  free(text);
  char *argv[] = {fixture, file, NULL};
  Proc r = proc_run(fixture, argv, stdin_data, stdin_data ? strlen(stdin_data) : 0, 15000);
  g_free(file);
  if (r.spawn_error || r.status != 0 || r.err_len != 0) {
    FAIL("fixture status %d stderr %s", r.status, r.err ? r.err : "");
    proc_clear(&r);
    return NULL;
  }
  yyjson_doc *doc = yyjson_read(r.out, r.out_len, 0);
  if (!doc)
    FAIL("fixture JSON parse failed");
  const char *ref = getenv("DARWIN_REFERENCE");
  yyjson_mut_val *in = yyjson_mut_doc_get_root(input);
  const char *op = yyjson_mut_get_str(yyjson_mut_obj_get(in, "op"));
  bool record = yyjson_mut_get_bool(yyjson_mut_obj_get(in, "record"));
  if (ref && *ref && !record && op &&
      (!strcmp(op, "health") || !strcmp(op, "preflight") || !strcmp(op, "diff"))) {
    char *file2 = g_strdup_printf("%s/request-%d.json", root, seqno++);
    char *again = yyjson_mut_write(input, 0, &len);
    write_file(file2, again, len, 0600);
    free(again);
    char *argv2[] = {(char *)ref, file2, NULL};
    Proc rr = proc_run(ref, argv2, NULL, 0, 15000);
    if (rr.spawn_error || rr.status != 0)
      FAIL("reference status %d %s", rr.status, rr.err ? rr.err : "");
    else {
      yyjson_doc *expected = yyjson_read(rr.out, rr.out_len, 0);
      yyjson_val *got = doc ? yyjson_doc_get_root(doc) : NULL;
      yyjson_val *exp = expected ? yyjson_doc_get_root(expected) : NULL;
      if (!strcmp(op, "health")) {
        if (!json_eq(yyjson_obj_get(got, "snapshot"), yyjson_obj_get(exp, "snapshot"), "time"))
          FAIL("health reference snapshot mismatch");
      } else if (!strcmp(op, "preflight")) {
        if (!json_eq(yyjson_obj_get(got, "error"), yyjson_obj_get(exp, "error"), NULL))
          FAIL("preflight reference error mismatch");
        if (got && !yyjson_get_bool(yyjson_obj_get(got, "error")) &&
            !json_eq(yyjson_obj_get(got, "errors"), yyjson_obj_get(exp, "errors"), NULL))
          FAIL("preflight reference errors mismatch");
      } else {
        if (!json_eq(yyjson_obj_get(got, "ok"), yyjson_obj_get(exp, "ok"), NULL))
          FAIL("diff reference ok mismatch");
        if (got && yyjson_get_bool(yyjson_obj_get(got, "ok")) &&
            !json_eq(yyjson_obj_get(got, "diff"), yyjson_obj_get(exp, "diff"), NULL))
          FAIL("diff reference mismatch");
      }
      yyjson_doc_free(expected);
    }
    proc_clear(&rr);
    g_free(file2);
  }
  proc_clear(&r);
  return doc;
}

static bool num_eq(yyjson_val *v, int64_t want) {
  return v && yyjson_is_num(v) && !yyjson_is_real(v) && yyjson_get_sint(v) == want;
}

static bool str_eq(yyjson_val *v, const char *want) {
  size_t n = strlen(want);
  return v && yyjson_is_str(v) && yyjson_get_len(v) == n &&
         !memcmp(yyjson_get_str(v), want, n);
}

static bool time_ok(const char *s) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
    return false;
  return y >= 1970 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h <= 23 &&
         mi >= 0 && mi <= 59 && se >= 0 && se <= 60;
}

static void test_health_classify(void) {
  begin("health classifies running, oneshot and idle socket without retaining raw output");
  yyjson_mut_doc *in = health_doc();
  yyjson_doc *doc = invoke_doc(in, NULL);
  yyjson_val *snap = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "snapshot") : NULL;
  EXPECT(snap && yyjson_get_bool(yyjson_obj_get(snap, "healthy")));
  EXPECT(snap && yyjson_get_bool(yyjson_obj_get(snap, "ready")));
  yyjson_val *services = snap ? yyjson_obj_get(snap, "services") : NULL;
  EXPECT(num_eq(yyjson_obj_get(yyjson_obj_get(services, "app"), "lastExit"), 75));
  EXPECT(services && yyjson_get_bool(yyjson_obj_get(yyjson_obj_get(services, "ssh"), "healthy")));
  EXPECT(str_eq(yyjson_obj_get(snap, "swapUsed"), "0.00M"));
  yyjson_val *pages = snap ? yyjson_obj_get(snap, "memoryPages") : NULL;
  EXPECT(num_eq(yyjson_obj_get(pages, "Pages free"), 42));
  EXPECT(num_eq(yyjson_obj_get(pages, "Pages active"), 99));
  EXPECT(num_eq(yyjson_obj_get(pages, "Pages wired down"), 10));
  EXPECT(num_eq(yyjson_obj_get(pages, "Pages occupied by compressor"), 11));
  EXPECT(snap && yyjson_get_bool(yyjson_obj_get(snap, "metricsAvailable")));
  size_t slen = 0;
  char *serial = snap ? yyjson_val_write(snap, 0, &slen) : NULL;
  EXPECT(serial && !contains_len(serial, slen, "private") &&
         !contains_len(serial, slen, "mock-sensitive"));
  free(serial);
  yyjson_val *calls = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "calls") : NULL;
  size_t i, n;
  yyjson_val *call;
  EXPECT(calls && yyjson_arr_size(calls) > 0);
  yyjson_arr_foreach(calls, i, n, call) EXPECT(num_eq(yyjson_obj_get(call, "timeout"), 5000));
  EXPECT(snap && time_ok(yyjson_get_str(yyjson_obj_get(snap, "time"))));
  yyjson_doc_free(doc);
  yyjson_mut_doc_free(in);
  end_case();
}

static void set_resp(yyjson_mut_doc *d, const char *key, int code, const char *out) {
  yyjson_mut_val *responses =
      yyjson_mut_obj_get(yyjson_mut_doc_get_root(d), "responses");
  yyjson_mut_obj_remove_key(responses, key);
  yyjson_mut_obj_add_val(d, responses, key, resp(d, code, out, false));
}

static void test_unhealthy(void) {
  begin("failed readiness, missing socket and failed/running oneshots are unhealthy");
  const char *keys[] = {"wait --timeout 0", "/bin/launchctl print system/ssh",
                        "/bin/launchctl print system/sops",
                        "/bin/launchctl print system/sops"};
  const char *outs[] = {"private failure", "", "state = not running\nlast exit code = 1\n",
                        "state = running\nlast exit code = 0\n"};
  int codes[] = {1, 113, 0, 0};
  for (size_t i = 0; i < 4; i++) {
    yyjson_mut_doc *in = health_doc();
    set_resp(in, keys[i], codes[i], outs[i]);
    yyjson_doc *doc = invoke_doc(in, NULL);
    yyjson_val *snap = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "snapshot") : NULL;
    EXPECT(snap && yyjson_is_bool(yyjson_obj_get(snap, "healthy")) &&
           !yyjson_get_bool(yyjson_obj_get(snap, "healthy")));
    yyjson_doc_free(doc);
    yyjson_mut_doc_free(in);
  }
  end_case();
}

static void test_metrics(void) {
  begin("metrics failure is separate from readiness; unknown state and Unicode integers remain typed");
  yyjson_mut_doc *in = health_doc();
  set_resp(in, "/usr/bin/vm_stat", 1, "Pages free: ٧.\n");
  set_resp(in, "/usr/sbin/sysctl -n vm.swapusage", 1, "not numeric");
  set_resp(in, "/bin/launchctl print system/sops", 0, "last exit code = -٠\n");
  yyjson_doc *doc = invoke_doc(in, NULL);
  yyjson_val *snap = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "snapshot") : NULL;
  yyjson_val *sops = snap ? yyjson_obj_get(yyjson_obj_get(snap, "services"), "sops") : NULL;
  EXPECT(snap && yyjson_get_bool(yyjson_obj_get(snap, "healthy")));
  EXPECT(str_eq(yyjson_obj_get(sops, "state"), "unknown"));
  EXPECT(num_eq(yyjson_obj_get(sops, "lastExit"), 0));
  EXPECT(num_eq(yyjson_obj_get(yyjson_obj_get(snap, "memoryPages"), "Pages free"), 7));
  EXPECT(snap && yyjson_is_null(yyjson_obj_get(snap, "swapUsed")));
  EXPECT(snap && yyjson_is_bool(yyjson_obj_get(snap, "metricsAvailable")) &&
         !yyjson_get_bool(yyjson_obj_get(snap, "metricsAvailable")));
  yyjson_doc_free(doc);
  yyjson_mut_doc_free(in);
  end_case();
}

static void test_duplicate(void) {
  begin("duplicate labels aggregate the final service record");
  yyjson_mut_doc *in = health_doc();
  yyjson_mut_val *config = yyjson_mut_obj_get(yyjson_mut_doc_get_root(in), "config");
  yyjson_mut_obj_remove_key(config, "services");
  yyjson_mut_val *services = yyjson_mut_arr(in);
  for (int i = 0; i < 2; i++) {
    yyjson_mut_val *s = yyjson_mut_obj(in);
    yyjson_mut_obj_add_strcpy(in, s, "label", "app");
    yyjson_mut_obj_add_strcpy(in, s, "kind", i ? "socket" : "running");
    yyjson_mut_arr_add_val(services, s);
  }
  yyjson_mut_obj_add_val(in, config, "services", services);
  set_resp(in, "/bin/launchctl print system/app", 0, "state = not running\n");
  yyjson_doc *doc = invoke_doc(in, NULL);
  yyjson_val *snap = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "snapshot") : NULL;
  EXPECT(snap && yyjson_get_bool(yyjson_obj_get(snap, "healthy")));
  yyjson_doc_free(doc);
  yyjson_mut_doc_free(in);
  end_case();
}

static int rotated_of(yyjson_doc *doc) {
  yyjson_val *v = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "rotated") : NULL;
  return v && yyjson_is_num(v) ? (int)yyjson_get_sint(v) : -999;
}

static yyjson_doc *rotate(const char *path, int limit) {
  yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *o = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, o);
  yyjson_mut_obj_add_strcpy(d, o, "op", "rotate");
  yyjson_mut_obj_add_strcpy(d, o, "path", path);
  yyjson_mut_obj_add_int(d, o, "limit", limit);
  yyjson_doc *doc = invoke_doc(d, NULL);
  yyjson_mut_doc_free(d);
  return doc;
}

static void test_copytruncate(void) {
  begin("copytruncate preserves the writer inode and two private bounded archives");
  char *dir = directory();
  char *p = g_build_filename(dir, "service.log", NULL);
  int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0600);
  EXPECT(fd >= 0);
  struct stat st;
  EXPECT(fstat(fd, &st) == 0);
  ino_t ino = st.st_ino;
  const char *chars[] = {"A", "B", "C"};
  for (size_t i = 0; i < 3; i++) {
    char buf[20];
    memset(buf, chars[i][0], 20);
    EXPECT(write(fd, buf, 20) == 20);
    yyjson_doc *doc = rotate(p, 10);
    EXPECT(rotated_of(doc) == 1);
    yyjson_doc_free(doc);
    EXPECT(stat(p, &st) == 0 && st.st_ino == ino);
  }
  EXPECT(write(fd, "live", 4) == 4);
  close(fd);
  char *data = NULL;
  size_t n = 0;
  EXPECT(read_file(p, &data, &n) && n == 4 && !memcmp(data, "live", 4));
  g_free(data);
  char *p1 = g_strconcat(p, ".1", NULL);
  char *p2 = g_strconcat(p, ".2", NULL);
  EXPECT(read_file(p1, &data, &n) && n == 10 && !memcmp(data, "CCCCCCCCCC", 10));
  g_free(data);
  EXPECT(read_file(p2, &data, &n) && n == 10 && !memcmp(data, "BBBBBBBBBB", 10));
  g_free(data);
  EXPECT(stat(p1, &st) == 0 && (st.st_mode & 0777) == 0600);
  g_free(p1);
  g_free(p2);
  g_free(p);
  g_free(dir);
  end_case();
}

static void test_rotate_reject(void) {
  begin("rotation skips missing/small logs and rejects symlinks, directories and FIFOs");
  char *dir = directory();
  char *p = g_build_filename(dir, "log", NULL);
  char *link = g_build_filename(dir, "link", NULL);
  char *fifo = g_build_filename(dir, "fifo", NULL);
  yyjson_doc *doc = rotate(p, 10);
  EXPECT(rotated_of(doc) == 0);
  yyjson_doc_free(doc);
  EXPECT(write_file(p, "small", 5, 0644));
  doc = rotate(p, 10);
  EXPECT(rotated_of(doc) == 0);
  yyjson_doc_free(doc);
  EXPECT(symlink(p, link) == 0);
  EXPECT(mkfifo(fifo, 0600) == 0);
  const char *paths[] = {dir, link, fifo};
  for (size_t i = 0; i < 3; i++) {
    doc = rotate(paths[i], 1);
    EXPECT(rotated_of(doc) == -1);
    yyjson_doc_free(doc);
  }
  char *data = NULL;
  size_t n = 0;
  EXPECT(read_file(p, &data, &n) && n == 5 && !memcmp(data, "small", 5));
  g_free(data);
  g_free(p);
  g_free(link);
  g_free(fifo);
  g_free(dir);
  end_case();
}

static void test_rotate_publish_fail(void) {
  begin("failed archive publication never truncates the live log");
  char *dir = directory();
  char *p = g_build_filename(dir, "log", NULL);
  EXPECT(write_file(p, "retain this", 11, 0644));
  char *p1 = g_strconcat(p, ".1", NULL);
  char *p2 = g_strconcat(p, ".2", NULL);
  EXPECT(write_file(p1, "old", 3, 0644));
  EXPECT(g_mkdir_with_parents(p2, 0700) == 0);
  yyjson_doc *doc = rotate(p, 3);
  EXPECT(rotated_of(doc) == -1);
  yyjson_doc_free(doc);
  char *data = NULL;
  size_t n = 0;
  EXPECT(read_file(p, &data, &n) && n == 11 && !memcmp(data, "retain this", 11));
  g_free(data);
  g_free(p1);
  g_free(p2);
  g_free(p);
  g_free(dir);
  end_case();
}

typedef struct {
  char *home;
  yyjson_mut_doc *doc;
} Pref;

static Pref preflight(void) {
  Pref p = {directory(), yyjson_mut_doc_new(NULL)};
  char *ssh = g_build_filename(p.home, ".ssh", NULL);
  g_mkdir_with_parents(ssh, 0700);
  char *key = g_build_filename(ssh, "id_ed25519", NULL);
  write_file(key, "fixture, not a key", 18, 0600);
  yyjson_mut_val *rootv = yyjson_mut_obj(p.doc);
  yyjson_mut_doc_set_root(p.doc, rootv);
  yyjson_mut_obj_add_strcpy(p.doc, rootv, "op", "preflight");
  yyjson_mut_val *config = yyjson_mut_obj(p.doc);
  yyjson_mut_obj_add_val(p.doc, rootv, "config", config);
  yyjson_mut_obj_add_strcpy(p.doc, config, "architecture", "arm64");
  yyjson_mut_obj_add_strcpy(p.doc, config, "host", "relay");
  yyjson_mut_obj_add_strcpy(p.doc, config, "user", "william");
  yyjson_mut_obj_add_strcpy(p.doc, config, "home", p.home);
  yyjson_mut_val *user_jobs = yyjson_mut_arr(p.doc);
  yyjson_mut_arr_add_strcpy(p.doc, user_jobs, "old.user");
  yyjson_mut_obj_add_val(p.doc, config, "legacyUserJobs", user_jobs);
  yyjson_mut_val *sys_jobs = yyjson_mut_arr(p.doc);
  yyjson_mut_arr_add_strcpy(p.doc, sys_jobs, "old.system");
  yyjson_mut_obj_add_val(p.doc, config, "legacySystemJobs", sys_jobs);
  yyjson_mut_obj_add_val(p.doc, config, "requiredFiles", yyjson_mut_arr(p.doc));
  yyjson_mut_obj_add_val(p.doc, config, "executables", yyjson_mut_arr(p.doc));
  char *sysdir = g_build_filename(p.home, "system", NULL);
  char *secrets = g_build_filename(p.home, "run-secrets", NULL);
  char *installed = g_build_filename(p.home, "installed.json", NULL);
  yyjson_mut_obj_add_strcpy(p.doc, config, "legacySystemDirectory", sysdir);
  yyjson_mut_obj_add_strcpy(p.doc, config, "systemSecrets", secrets);
  yyjson_mut_obj_add_strcpy(p.doc, config, "installedConfig", installed);
  yyjson_mut_val *id = yyjson_mut_obj(p.doc);
  yyjson_mut_obj_add_val(p.doc, rootv, "identity", id);
  yyjson_mut_obj_add_strcpy(p.doc, id, "system", "Darwin");
  yyjson_mut_obj_add_strcpy(p.doc, id, "machine", "arm64");
  yyjson_mut_obj_add_bool(p.doc, id, "found", true);
  yyjson_mut_obj_add_uint(p.doc, id, "uid", (uint64_t)getuid());
  yyjson_mut_obj_add_strcpy(p.doc, id, "home", p.home);
  yyjson_mut_val *responses = yyjson_mut_obj(p.doc);
  yyjson_mut_obj_add_val(p.doc, rootv, "responses", responses);
  yyjson_mut_obj_add_val(p.doc, responses, "/bin/hostname -s", resp(p.doc, 0, " RELAY\n", false));
  yyjson_mut_obj_add_val(
      p.doc, responses,
      "/usr/bin/defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser",
      resp(p.doc, 1, "", false));
  g_free(ssh);
  g_free(key);
  g_free(sysdir);
  g_free(secrets);
  g_free(installed);
  return p;
}

static void pref_free(Pref *p) {
  yyjson_mut_doc_free(p->doc);
  g_free(p->home);
}

static bool errors_eq(yyjson_val *errors, const char *const *want, size_t n) {
  if (!errors || !yyjson_is_arr(errors) || yyjson_arr_size(errors) != n)
    return false;
  for (size_t i = 0; i < n; i++)
    if (!str_eq(yyjson_arr_get(errors, i), want[i]))
      return false;
  return true;
}

static void collect(const char *dir, const char *rel, GPtrArray *out) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    char *child = g_build_filename(dir, ent->d_name, NULL);
    char *name = rel ? g_build_filename(rel, ent->d_name, NULL) : g_strdup(ent->d_name);
    g_ptr_array_add(out, name);
    struct stat st;
    if (!lstat(child, &st) && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
      collect(child, name, out);
    g_free(child);
  }
  closedir(d);
}

static bool listings_eq(const char *path, GPtrArray *before) {
  GPtrArray *after = g_ptr_array_new_with_free_func(g_free);
  collect(path, NULL, after);
  bool ok = before->len == after->len;
  for (guint i = 0; ok && i < before->len; i++)
    ok = !strcmp(before->pdata[i], after->pdata[i]);
  g_ptr_array_free(after, TRUE);
  return ok;
}

static void test_clean_preflight(void) {
  begin("clean preflight is read-only and uses ten-second command deadlines");
  Pref p = preflight();
  GPtrArray *before = g_ptr_array_new_with_free_func(g_free);
  collect(p.home, NULL, before);
  yyjson_doc *doc = invoke_doc(p.doc, NULL);
  yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(errors_eq(yyjson_obj_get(rootv, "errors"), NULL, 0));
  EXPECT(rootv && yyjson_is_bool(yyjson_obj_get(rootv, "error")) &&
         !yyjson_get_bool(yyjson_obj_get(rootv, "error")));
  EXPECT(listings_eq(p.home, before));
  yyjson_val *calls = rootv ? yyjson_obj_get(rootv, "calls") : NULL;
  size_t i, n;
  yyjson_val *call;
  yyjson_arr_foreach(calls, i, n, call) EXPECT(num_eq(yyjson_obj_get(call, "timeout"), 10000));
  g_ptr_array_free(before, TRUE);
  yyjson_doc_free(doc);
  pref_free(&p);
  end_case();
}

static void test_hostname_ws(void) {
  begin("hostname normalization retains Unicode whitespace stripping");
  Pref p = preflight();
  yyjson_mut_val *responses = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "responses");
  yyjson_mut_obj_remove_key(responses, "/bin/hostname -s");
  yyjson_mut_obj_add_val(p.doc, responses, "/bin/hostname -s",
                         resp(p.doc, 0, "\xC2\x85\x1c RELAY\u2003\xC2\x85", false));
  yyjson_doc *doc = invoke_doc(p.doc, NULL);
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), NULL, 0));
  yyjson_doc_free(doc);
  pref_free(&p);
  end_case();
}

static void test_mismatches(void) {
  begin("platform, architecture, hostname and auto-login mismatches block");
  Pref p = preflight();
  yyjson_mut_val *id = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "identity");
  yyjson_mut_obj_remove_key(id, "system");
  yyjson_mut_obj_remove_key(id, "machine");
  yyjson_mut_obj_add_strcpy(p.doc, id, "system", "Linux");
  yyjson_mut_obj_add_strcpy(p.doc, id, "machine", "x86_64");
  yyjson_mut_val *responses = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "responses");
  yyjson_mut_obj_remove_key(responses, "/bin/hostname -s");
  yyjson_mut_obj_add_val(p.doc, responses, "/bin/hostname -s", resp(p.doc, 0, "other", false));
  const char *login =
      "/usr/bin/defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser";
  yyjson_mut_obj_remove_key(responses, login);
  yyjson_mut_obj_add_val(p.doc, responses, login, resp(p.doc, 0, "", false));
  yyjson_doc *doc = invoke_doc(p.doc, NULL);
  const char *want[] = {"Target requires macOS",
                        "Target architecture differs from the running machine",
                        "Register the correct node identity before deployment",
                        "Disable automatic graphical login before deployment"};
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), want, 4));
  yyjson_doc_free(doc);
  pref_free(&p);
  end_case();
}

static void test_account(void) {
  begin("missing account stops checks early; wrong account home is rejected");
  Pref p = preflight();
  yyjson_mut_val *id = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "identity");
  yyjson_mut_obj_remove_key(id, "found");
  yyjson_mut_obj_add_bool(p.doc, id, "found", false);
  yyjson_doc *doc = invoke_doc(p.doc, NULL);
  const char *missing[] = {"Provision the account before deployment"};
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), missing, 1));
  EXPECT(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "calls")) == 1);
  yyjson_doc_free(doc);
  yyjson_mut_obj_remove_key(id, "found");
  yyjson_mut_obj_add_bool(p.doc, id, "found", true);
  yyjson_mut_obj_remove_key(id, "home");
  char *other = g_strconcat(p.home, "/other", NULL);
  yyjson_mut_obj_add_strcpy(p.doc, id, "home", other);
  doc = invoke_doc(p.doc, NULL);
  const char *home[] = {"Account home differs from the target"};
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), home, 1));
  yyjson_doc_free(doc);
  g_free(other);
  pref_free(&p);
  end_case();
}

static void test_legacy_jobs(void) {
  begin("installed, broken-link and loaded legacy jobs are all blocked");
  Pref p = preflight();
  char *path = g_build_filename(p.home, "Library/LaunchAgents/old.user.plist", NULL);
  char *parent = g_path_get_dirname(path);
  g_mkdir_with_parents(parent, 0700);
  EXPECT(symlink("/missing", path) == 0);
  yyjson_mut_val *responses = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "responses");
  yyjson_mut_obj_add_val(p.doc, responses, "/bin/launchctl print system/old.system",
                         resp(p.doc, 0, "private environment", false));
  yyjson_doc *doc = invoke_doc(p.doc, NULL);
  const char *want[] = {"Retire legacy user job: old.user", "Retire legacy system job: old.system"};
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), want, 2));
  yyjson_doc_free(doc);
  g_free(parent);
  g_free(path);
  pref_free(&p);
  end_case();
}

static void test_key(void) {
  begin("private key type, permissions and ownership, runtime files and execute bits are checked");
  Pref p = preflight();
  char *key = g_build_filename(p.home, ".ssh/id_ed25519", NULL);
  EXPECT(chmod(key, 0644) == 0);
  yyjson_mut_val *config = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "config");
  char *model = g_build_filename(p.home, "model", NULL);
  yyjson_mut_obj_remove_key(config, "requiredFiles");
  yyjson_mut_obj_remove_key(config, "executables");
  yyjson_mut_val *req = yyjson_mut_arr(p.doc);
  yyjson_mut_arr_add_strcpy(p.doc, req, model);
  yyjson_mut_obj_add_val(p.doc, config, "requiredFiles", req);
  yyjson_mut_val *exe = yyjson_mut_arr(p.doc);
  yyjson_mut_arr_add_strcpy(p.doc, exe, key);
  yyjson_mut_obj_add_val(p.doc, config, "executables", exe);
  yyjson_doc *doc = invoke_doc(p.doc, NULL);
  EXPECT(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "errors")) == 3);
  yyjson_doc_free(doc);
  EXPECT(chmod(key, 0700) == 0);
  yyjson_mut_obj_remove_key(config, "requiredFiles");
  yyjson_mut_obj_remove_key(config, "executables");
  yyjson_mut_obj_add_val(p.doc, config, "requiredFiles", yyjson_mut_arr(p.doc));
  yyjson_mut_obj_add_val(p.doc, config, "executables", yyjson_mut_arr(p.doc));
  yyjson_mut_val *id = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "identity");
  uint64_t uid = yyjson_mut_get_uint(yyjson_mut_obj_get(id, "uid"));
  yyjson_mut_obj_remove_key(id, "uid");
  yyjson_mut_obj_add_uint(p.doc, id, "uid", uid + 1);
  doc = invoke_doc(p.doc, NULL);
  const char *own[] = {"Shared SSH key must be private and user-owned"};
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), own, 1));
  yyjson_doc_free(doc);
  unlink(key);
  doc = invoke_doc(p.doc, NULL);
  const char *prov[] = {"Provision the approved shared identity before deployment"};
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), prov, 1));
  yyjson_doc_free(doc);
  g_mkdir_with_parents(key, 0700);
  doc = invoke_doc(p.doc, NULL);
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), own, 1));
  yyjson_doc_free(doc);
  g_free(model);
  g_free(key);
  pref_free(&p);
  end_case();
}

static void test_secrets(void) {
  begin("unmanaged secret directories and system-secret ownership cannot be adopted");
  Pref p = preflight();
  char *alias = g_build_filename(p.home, ".config/sops-nix/secrets", NULL);
  g_mkdir_with_parents(alias, 0700);
  char *secrets = g_build_filename(p.home, "run-secrets", NULL);
  g_mkdir_with_parents(secrets, 0700);
  yyjson_doc *doc = invoke_doc(p.doc, NULL);
  const char *want[] = {"Inspect the unmanaged secret directory before migration",
                        "Existing system secrets are not owned by this profile"};
  EXPECT(errors_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "errors"), want, 2));
  yyjson_doc_free(doc);
  g_free(alias);
  g_free(secrets);
  pref_free(&p);
  end_case();
}

static void test_preflight_timeout(void) {
  begin("preflight spawn/timeout failures stop before later commands");
  Pref p = preflight();
  yyjson_mut_val *responses = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "responses");
  yyjson_mut_obj_remove_key(responses, "/bin/hostname -s");
  yyjson_mut_obj_add_val(p.doc, responses, "/bin/hostname -s", resp(p.doc, 124, "", true));
  yyjson_doc *doc = invoke_doc(p.doc, NULL);
  yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(rootv && yyjson_get_bool(yyjson_obj_get(rootv, "error")));
  EXPECT(yyjson_arr_size(yyjson_obj_get(rootv, "calls")) == 1);
  yyjson_doc_free(doc);
  pref_free(&p);
  end_case();
}

static void test_diff(void) {
  begin("policy diff is sorted, ignores unknown installed fields and never displays their values");
  Pref p = preflight();
  yyjson_mut_val *config = yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "config");
  char *installed = g_strdup(yyjson_mut_get_str(yyjson_mut_obj_get(config, "installedConfig")));
  yyjson_mut_doc *old = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *copy = yyjson_mut_obj(old);
  yyjson_mut_doc_set_root(old, copy);
  size_t i, max;
  yyjson_mut_val *k, *v;
  yyjson_mut_obj_foreach(config, i, max, k, v)
      yyjson_mut_obj_add_val(old, copy, yyjson_mut_get_str(k), yyjson_mut_val_mut_copy(old, v));
  yyjson_mut_obj_remove_key(copy, "host");
  yyjson_mut_obj_add_strcpy(old, copy, "host", "old");
  yyjson_mut_val *priv = yyjson_mut_obj(old);
  yyjson_mut_obj_add_strcpy(old, priv, "TOKEN", "mock-sensitive-data");
  yyjson_mut_obj_add_val(old, copy, "privateEnvironment", priv);
  size_t len = 0;
  char *text = yyjson_mut_write(old, 0, &len);
  EXPECT(write_file(installed, text, len, 0644));
  free(text);
  yyjson_mut_doc_free(old);
  yyjson_mut_doc *diff_in = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *rootv = yyjson_mut_obj(diff_in);
  yyjson_mut_doc_set_root(diff_in, rootv);
  yyjson_mut_obj_add_strcpy(diff_in, rootv, "op", "diff");
  yyjson_mut_obj_add_val(diff_in, rootv, "config", yyjson_mut_val_mut_copy(diff_in, config));
  yyjson_doc *doc = invoke_doc(diff_in, NULL);
  const char *diff = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "diff"));
  size_t dn = diff ? yyjson_get_len(yyjson_obj_get(yyjson_doc_get_root(doc), "diff")) : 0;
  EXPECT(diff && contains_len(diff, dn, "--- installed server policy"));
  EXPECT(diff && contains_len(diff, dn, "+++ candidate server policy"));
  EXPECT(diff && contains_len(diff, dn, "-  \"host\": \"old\""));
  EXPECT(diff && contains_len(diff, dn, "+  \"host\": \"relay\""));
  EXPECT(diff && !contains_len(diff, dn, "mock-sensitive") &&
         !contains_len(diff, dn, "privateEnvironment"));
  yyjson_doc_free(doc);
  yyjson_mut_doc_free(diff_in);
  old = yyjson_mut_doc_new(NULL);
  copy = yyjson_mut_obj(old);
  yyjson_mut_doc_set_root(old, copy);
  yyjson_mut_obj_foreach(config, i, max, k, v)
      yyjson_mut_obj_add_val(old, copy, yyjson_mut_get_str(k), yyjson_mut_val_mut_copy(old, v));
  yyjson_mut_obj_add_strcpy(old, copy, "ignored", "mock-sensitive-data");
  text = yyjson_mut_write(old, 0, &len);
  EXPECT(write_file(installed, text, len, 0644));
  free(text);
  yyjson_mut_doc_free(old);
  diff_in = yyjson_mut_doc_new(NULL);
  rootv = yyjson_mut_obj(diff_in);
  yyjson_mut_doc_set_root(diff_in, rootv);
  yyjson_mut_obj_add_strcpy(diff_in, rootv, "op", "diff");
  yyjson_mut_obj_add_val(diff_in, rootv, "config", yyjson_mut_val_mut_copy(diff_in, config));
  doc = invoke_doc(diff_in, NULL);
  EXPECT(str_eq(yyjson_obj_get(yyjson_doc_get_root(doc), "diff"), ""));
  yyjson_doc_free(doc);
  yyjson_mut_doc_free(diff_in);
  const char *invalid[] = {"broken json", "null", "123"};
  for (size_t n = 0; n < 3; n++) {
    EXPECT(write_file(installed, invalid[n], strlen(invalid[n]), 0644));
    diff_in = yyjson_mut_doc_new(NULL);
    rootv = yyjson_mut_obj(diff_in);
    yyjson_mut_doc_set_root(diff_in, rootv);
    yyjson_mut_obj_add_strcpy(diff_in, rootv, "op", "diff");
    yyjson_mut_obj_add_val(diff_in, rootv, "config", yyjson_mut_val_mut_copy(diff_in, config));
    doc = invoke_doc(diff_in, NULL);
    yyjson_val *ok = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "ok") : NULL;
    EXPECT(ok && yyjson_is_bool(ok) && !yyjson_get_bool(ok));
    yyjson_doc_free(doc);
    yyjson_mut_doc_free(diff_in);
  }
  g_free(installed);
  pref_free(&p);
  end_case();
}

typedef struct {
  char *dir, *target, *old, *other, *profile, *current, *home_profile, *state;
  yyjson_mut_doc *doc;
} Dep;

static Dep deployment(const char *mode) {
  Dep d = {0};
  d.dir = directory();
  d.target = g_build_filename(d.dir, "new", NULL);
  d.old = g_build_filename(d.dir, "old", NULL);
  d.other = g_build_filename(d.dir, "other", NULL);
  d.profile = g_build_filename(d.dir, "profile", NULL);
  d.current = g_build_filename(d.dir, "current", NULL);
  d.home_profile = g_build_filename(d.dir, "home-profile", NULL);
  d.state = g_build_filename(d.dir, "state", NULL);
  g_mkdir_with_parents(d.target, 0700);
  g_mkdir_with_parents(d.old, 0700);
  g_mkdir_with_parents(d.other, 0700);
  if (symlink(d.old, d.profile) != 0 || symlink(d.old, d.current) != 0)
    return d;
  char *missing = g_build_filename(d.dir, "missing-home-generation", NULL);
  if (symlink(missing, d.home_profile) != 0) {
    g_free(missing);
    return d;
  }
  g_free(missing);
  d.doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *o = yyjson_mut_obj(d.doc);
  yyjson_mut_doc_set_root(d.doc, o);
  yyjson_mut_obj_add_strcpy(d.doc, o, "op", "deploy");
  yyjson_mut_obj_add_strcpy(d.doc, o, "mode", mode ? mode : "");
  yyjson_mut_obj_add_strcpy(d.doc, o, "target", d.target);
  yyjson_mut_obj_add_strcpy(d.doc, o, "old", d.old);
  yyjson_mut_obj_add_strcpy(d.doc, o, "other", d.other);
  yyjson_mut_obj_add_strcpy(d.doc, o, "profile", d.profile);
  yyjson_mut_obj_add_strcpy(d.doc, o, "current", d.current);
  yyjson_mut_obj_add_strcpy(d.doc, o, "homeProfile", d.home_profile);
  yyjson_mut_obj_add_strcpy(d.doc, o, "state", d.state);
  return d;
}

static void dep_free(Dep *d) {
  yyjson_mut_doc_free(d->doc);
  g_free(d->dir);
  g_free(d->target);
  g_free(d->old);
  g_free(d->other);
  g_free(d->profile);
  g_free(d->current);
  g_free(d->home_profile);
  g_free(d->state);
}

static char *pointer(const char *path) {
  char buf[4096];
  ssize_t n = readlink(path, buf, sizeof buf - 1);
  if (n < 0)
    return NULL;
  buf[n] = 0;
  return g_strdup(buf);
}

static yyjson_doc *recover(Dep *d) {
  DIR *dir = opendir(d->state);
  EXPECT(dir);
  if (!dir)
    return NULL;
  char *found = NULL;
  struct dirent *ent;
  int n = 0;
  while ((ent = readdir(dir))) {
    if (g_str_has_prefix(ent->d_name, "rollout-")) {
      n++;
      g_free(found);
      found = g_strdup(ent->d_name);
    }
  }
  closedir(dir);
  EXPECT(n == 1);
  char *roll = g_build_filename(d->state, found, NULL);
  char *file = g_build_filename(roll, "generations.json", NULL);
  struct stat st;
  EXPECT(stat(file, &st) == 0 && (st.st_mode & 0777) == 0600);
  EXPECT(stat(roll, &st) == 0 && (st.st_mode & 0777) == 0700);
  char *data = NULL;
  size_t len = 0;
  EXPECT(read_file(file, &data, &len));
  yyjson_doc *doc = data ? yyjson_read(data, len, 0) : NULL;
  EXPECT(doc);
  g_free(data);
  g_free(file);
  g_free(roll);
  g_free(found);
  return doc;
}

static bool msg_has(yyjson_val *messages, const char *needle) {
  size_t i, n;
  yyjson_val *m;
  if (!messages)
    return false;
  yyjson_arr_foreach(messages, i, n, m) {
    if (yyjson_is_str(m) && contains_len(yyjson_get_str(m), yyjson_get_len(m), needle))
      return true;
  }
  return false;
}

static void test_deploy_ok(void) {
  begin("successful deployment double-checks preflight before registration and saves paired recovery");
  Dep d = deployment("");
  yyjson_doc *doc = invoke_doc(d.doc, NULL);
  yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(rootv && yyjson_get_bool(yyjson_obj_get(rootv, "ok")));
  yyjson_val *calls = rootv ? yyjson_obj_get(rootv, "calls") : NULL;
  EXPECT(calls && yyjson_arr_size(calls) == 4);
  EXPECT(calls && yyjson_is_bool(yyjson_obj_get(yyjson_arr_get(calls, 0), "stateExists")) &&
         !yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(calls, 0), "stateExists")));
  EXPECT(calls && yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(calls, 1), "stateExists")));
  yyjson_val *args = calls ? yyjson_obj_get(yyjson_arr_get(calls, 2), "args") : NULL;
  const char *arg0 = args ? yyjson_get_str(yyjson_arr_get(args, 0)) : NULL;
  EXPECT(arg0 && g_str_has_suffix(arg0, "/nix-env"));
  yyjson_doc *rec = recover(&d);
  char *parent = g_path_get_dirname(d.home_profile);
  char *home = g_build_filename(parent, "missing-home-generation", NULL);
  yyjson_val *rv = rec ? yyjson_doc_get_root(rec) : NULL;
  EXPECT(str_eq(yyjson_obj_get(rv, "target"), d.target));
  EXPECT(str_eq(yyjson_obj_get(rv, "previousProfile"), d.old));
  EXPECT(str_eq(yyjson_obj_get(rv, "previousActive"), d.old));
  EXPECT(str_eq(yyjson_obj_get(rv, "previousHome"), home));
  char *pp = pointer(d.profile);
  char *cp = pointer(d.current);
  EXPECT(pp && !strcmp(pp, d.target));
  EXPECT(cp && !strcmp(cp, d.target));
  struct stat st;
  EXPECT(stat(d.state, &st) == 0 && (st.st_mode & 0777) == 0700);
  char *lock = g_build_filename(d.state, "deploy.lock", NULL);
  EXPECT(stat(lock, &st) == 0 && (st.st_mode & 0777) == 0600);
  g_free(lock);
  g_free(pp);
  g_free(cp);
  g_free(parent);
  g_free(home);
  yyjson_doc_free(rec);
  yyjson_doc_free(doc);
  dep_free(&d);
  end_case();
}

static void test_first_preflight(void) {
  begin("first preflight failure creates no state and never touches the profile");
  Dep d = deployment("preflight");
  yyjson_doc *doc = invoke_doc(d.doc, NULL);
  yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(rootv && yyjson_is_bool(yyjson_obj_get(rootv, "ok")) &&
         !yyjson_get_bool(yyjson_obj_get(rootv, "ok")));
  EXPECT(yyjson_arr_size(yyjson_obj_get(rootv, "calls")) == 1);
  EXPECT(!g_file_test(d.state, G_FILE_TEST_EXISTS));
  char *pp = pointer(d.profile);
  EXPECT(pp && !strcmp(pp, d.old));
  g_free(pp);
  yyjson_doc_free(doc);
  dep_free(&d);
  end_case();
}

static void test_second_lock(void) {
  begin("second preflight failure and lock contention create no recovery or registration");
  const char *modes[] = {"preflight-locked", "locked"};
  for (size_t m = 0; m < 2; m++) {
    Dep d = deployment(modes[m]);
    yyjson_doc *doc = invoke_doc(d.doc, NULL);
    yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
    EXPECT(rootv && !yyjson_get_bool(yyjson_obj_get(rootv, "ok")));
    EXPECT(yyjson_arr_size(yyjson_obj_get(rootv, "calls")) == (m == 1 ? 1 : 2));
    size_t n = 0;
    DIR *dir = opendir(d.state);
    EXPECT(dir);
    struct dirent *ent;
    bool only = true;
    int count = 0;
    while (dir && (ent = readdir(dir))) {
      if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
        continue;
      count++;
      if (strcmp(ent->d_name, "deploy.lock"))
        only = false;
    }
    if (dir)
      closedir(dir);
    EXPECT(only && count == 1);
    (void)n;
    char *pp = pointer(d.profile);
    EXPECT(pp && !strcmp(pp, d.old));
    g_free(pp);
    yyjson_doc_free(doc);
    dep_free(&d);
  }
  end_case();
}

static void test_restore(void) {
  begin("failed activation and partial registration restore the previous profile only");
  const char *modes[] = {"activation", "registration", "no-activation"};
  for (size_t m = 0; m < 3; m++) {
    Dep d = deployment(modes[m]);
    yyjson_doc *doc = invoke_doc(d.doc, NULL);
    yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
    EXPECT(rootv && !yyjson_get_bool(yyjson_obj_get(rootv, "ok")));
    char *pp = pointer(d.profile);
    char *cp = pointer(d.current);
    EXPECT(pp && !strcmp(pp, d.old));
    EXPECT(cp && !strcmp(cp, d.old));
    yyjson_doc *rec = recover(&d);
    EXPECT(str_eq(yyjson_obj_get(yyjson_doc_get_root(rec), "previousProfile"), d.old));
    EXPECT(msg_has(yyjson_obj_get(rootv, "messages"), "services may have changed"));
    g_free(pp);
    g_free(cp);
    yyjson_doc_free(rec);
    yyjson_doc_free(doc);
    dep_free(&d);
  }
  end_case();
}

static int nix_calls(yyjson_val *calls) {
  int n = 0;
  size_t i, count;
  yyjson_val *call;
  yyjson_arr_foreach(calls, i, count, call) {
    yyjson_val *args = yyjson_obj_get(call, "args");
    const char *a0 = args ? yyjson_get_str(yyjson_arr_get(args, 0)) : NULL;
    if (a0 && g_str_has_suffix(a0, "/nix-env"))
      n++;
  }
  return n;
}

static void test_race(void) {
  begin("concurrent profile changes survive failed and successful activation exits");
  const char *modes[] = {"race", "race-success"};
  for (size_t m = 0; m < 2; m++) {
    Dep d = deployment(modes[m]);
    yyjson_doc *doc = invoke_doc(d.doc, NULL);
    yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
    EXPECT(rootv && !yyjson_get_bool(yyjson_obj_get(rootv, "ok")));
    char *pp = pointer(d.profile);
    EXPECT(pp && !strcmp(pp, d.other));
    EXPECT(msg_has(yyjson_obj_get(rootv, "messages"), "refusing to overwrite"));
    EXPECT(nix_calls(yyjson_obj_get(rootv, "calls")) == 1);
    g_free(pp);
    yyjson_doc_free(doc);
    dep_free(&d);
  }
  end_case();
}

static void test_first_install(void) {
  begin("failed first install removes only its newly registered profile pointer");
  Dep d = deployment("activation");
  unlink(d.profile);
  yyjson_doc *doc = invoke_doc(d.doc, NULL);
  EXPECT(doc && !yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "ok")));
  EXPECT(!g_file_test(d.profile, G_FILE_TEST_EXISTS));
  EXPECT(g_file_test(d.target, G_FILE_TEST_EXISTS));
  yyjson_doc *rec = recover(&d);
  EXPECT(yyjson_is_null(yyjson_obj_get(yyjson_doc_get_root(rec), "previousProfile")));
  yyjson_doc_free(rec);
  yyjson_doc_free(doc);
  dep_free(&d);
  end_case();
}

static void test_rollback(void) {
  begin("failed recovery never reports successful rollback or changes active generation");
  Dep d = deployment("rollback");
  yyjson_doc *doc = invoke_doc(d.doc, NULL);
  yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(rootv && !yyjson_get_bool(yyjson_obj_get(rootv, "ok")));
  char *pp = pointer(d.profile);
  char *cp = pointer(d.current);
  EXPECT(pp && !strcmp(pp, d.target));
  EXPECT(cp && !strcmp(cp, d.old));
  yyjson_val *calls = yyjson_obj_get(rootv, "calls");
  yyjson_val *last = calls ? yyjson_arr_get(calls, yyjson_arr_size(calls) - 1) : NULL;
  yyjson_val *args = last ? yyjson_obj_get(last, "args") : NULL;
  yyjson_val *last_arg = args ? yyjson_arr_get(args, yyjson_arr_size(args) - 1) : NULL;
  EXPECT(str_eq(last_arg, d.old));
  EXPECT(yyjson_arr_size(yyjson_obj_get(rootv, "messages")) == 1);
  yyjson_doc *rec = recover(&d);
  yyjson_doc_free(rec);
  g_free(pp);
  g_free(cp);
  yyjson_doc_free(doc);
  dep_free(&d);
  end_case();
}

static yyjson_doc *command_op(const char **args, const char *stdin_data, int timeout) {
  yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *o = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, o);
  yyjson_mut_obj_add_strcpy(d, o, "op", "command");
  yyjson_mut_val *arr = yyjson_mut_arr(d);
  for (size_t i = 0; args[i]; i++)
    yyjson_mut_arr_add_strcpy(d, arr, args[i]);
  yyjson_mut_obj_add_val(d, o, "args", arr);
  yyjson_mut_obj_add_int(d, o, "timeout", timeout);
  if (stdin_data)
    yyjson_mut_obj_add_strcpy(d, o, "stdin", stdin_data);
  yyjson_doc *doc = invoke_doc(d, stdin_data);
  yyjson_mut_doc_free(d);
  return doc;
}

static void test_command(void) {
  begin("captured commands normalize newlines, suppress stderr, return failure and time out");
  const char *args1[] = {"/bin/sh", "-c",
                         "printf 'one\\r\\ntwo\\r'; printf 'private-error' >&2; exit 7", NULL};
  yyjson_doc *doc = command_op(args1, NULL, 1000);
  yyjson_val *rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(num_eq(yyjson_obj_get(rootv, "code"), 7));
  EXPECT(str_eq(yyjson_obj_get(rootv, "out"), "one\ntwo\n"));
  EXPECT(rootv && yyjson_is_bool(yyjson_obj_get(rootv, "error")) &&
         !yyjson_get_bool(yyjson_obj_get(rootv, "error")));
  yyjson_doc_free(doc);
  const char *args2[] = {"/bin/sh", "-c", "IFS= read -r line; printf \"%s\" \"$line\"", NULL};
  doc = command_op(args2, "inherited input\n", 1000);
  rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(str_eq(yyjson_obj_get(rootv, "out"), "inherited input"));
  EXPECT(num_eq(yyjson_obj_get(rootv, "code"), 0));
  yyjson_doc_free(doc);
  int64_t start = now_ms();
  const char *args3[] = {"/bin/sh", "-c", "exec sleep 20", NULL};
  doc = command_op(args3, NULL, 30);
  rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(rootv && yyjson_get_bool(yyjson_obj_get(rootv, "error")));
  EXPECT(num_eq(yyjson_obj_get(rootv, "code"), 124));
  EXPECT(now_ms() - start < 3000);
  yyjson_doc_free(doc);
  const char *args4[] = {"/missing/darwin-fixture-command", NULL};
  doc = command_op(args4, NULL, 100);
  rootv = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(rootv && yyjson_get_bool(yyjson_obj_get(rootv, "error")));
  EXPECT(num_eq(yyjson_obj_get(rootv, "code"), 124));
  yyjson_doc_free(doc);
  end_case();
}

static void test_cli(void) {
  begin("public CLI retains usage, store-path and explicit root gates without writing");
  const char *names[] = {"health", "preflight", "deploy"};
  for (size_t i = 0; i < 3; i++) {
    char *path = g_strdup_printf("%s/darwin-%s", bin_dir, names[i]);
    char *argv[] = {path, "--help", NULL};
    Proc r = proc_run(path, argv, NULL, 0, 5000);
    EXPECT(!r.spawn_error && r.status == 0);
    proc_clear(&r);
    g_free(path);
  }
  EXPECT(getuid() != 0);
  char *deploy = g_build_filename(bin_dir, "darwin-deploy", NULL);
  char *argv[] = {deploy, root, NULL};
  Proc r = proc_run(deploy, argv, NULL, 0, 5000);
  EXPECT(r.status == 2);
  EXPECT(contains_len(r.err, r.err_len, "explicit sudo authorization"));
  proc_clear(&r);
  Pref p = preflight();
  char *file = g_build_filename(root, "cli-config", NULL);
  size_t len = 0;
  char *text = yyjson_mut_val_write(
      yyjson_mut_obj_get(yyjson_mut_doc_get_root(p.doc), "config"), 0, &len);
  EXPECT(write_file(file, text, len, 0644));
  free(text);
  char *pre = g_build_filename(bin_dir, "darwin-preflight", NULL);
  char *pargv[] = {pre, "--config", file, "--target", root, NULL};
  r = proc_run(pre, pargv, NULL, 0, 5000);
  EXPECT(r.status == 2);
  EXPECT(contains_len(r.err, r.err_len, "already-built Nix store path"));
  proc_clear(&r);
  char *status = g_build_filename(root, "not-recorded", NULL);
  char *health_json = g_strdup_printf(
      "{\"services\":[],\"readiness\":[\"/bin/sh\",\"-c\",\"exit 0\"],\"logs\":[],\"logLimitBytes\":5,\"statusFile\":%s}",
      yyjson_mut_get_str(yyjson_mut_str(p.doc, status)) ? "" : "");
  /* write via yyjson to escape the path */
  g_free(health_json);
  yyjson_mut_doc *hd = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *ho = yyjson_mut_obj(hd);
  yyjson_mut_doc_set_root(hd, ho);
  yyjson_mut_obj_add_val(hd, ho, "services", yyjson_mut_arr(hd));
  yyjson_mut_val *ready = yyjson_mut_arr(hd);
  yyjson_mut_arr_add_strcpy(hd, ready, "/bin/sh");
  yyjson_mut_arr_add_strcpy(hd, ready, "-c");
  yyjson_mut_arr_add_strcpy(hd, ready, "exit 0");
  yyjson_mut_obj_add_val(hd, ho, "readiness", ready);
  yyjson_mut_obj_add_val(hd, ho, "logs", yyjson_mut_arr(hd));
  yyjson_mut_obj_add_int(hd, ho, "logLimitBytes", 5);
  yyjson_mut_obj_add_strcpy(hd, ho, "statusFile", status);
  text = yyjson_mut_write(hd, 0, &len);
  EXPECT(write_file(file, text, len, 0644));
  free(text);
  yyjson_mut_doc_free(hd);
  char *health = g_build_filename(bin_dir, "darwin-health", NULL);
  char *hargv[] = {health, "--config", file, "--record", NULL};
  r = proc_run(health, hargv, NULL, 0, 5000);
  EXPECT(r.status == 2);
  EXPECT(contains_len(r.err, r.err_len, "require root"));
  EXPECT(!g_file_test(status, G_FILE_TEST_EXISTS));
  proc_clear(&r);
  g_free(health);
  g_free(status);
  g_free(pre);
  g_free(file);
  g_free(deploy);
  pref_free(&p);
  end_case();
}

int main(void) {
  umask(022);
  const char *env_fix = getenv("DARWIN_FIXTURE");
  const char *env_bin = getenv("DARWIN_BIN");
  char *dir = sibling_dir();
  fixture = env_fix && *env_fix ? g_strdup(env_fix) : g_build_filename(dir, "darwin-fixture", NULL);
  bin_dir = env_bin && *env_bin ? g_strdup(env_bin) : g_strdup(dir);
  g_free(dir);
  if (!fixture || !bin_dir || access(fixture, X_OK)) {
    fprintf(stderr, "set DARWIN_FIXTURE and DARWIN_BIN\n");
    return 1;
  }
  char tmpl[] = "/tmp/darwin-tools-XXXXXX";
  char *made = g_mkdtemp(tmpl);
  if (!made)
    return 1;
  char *resolved = realpath(made, NULL);
  root = resolved ? resolved : g_strdup(made);
  test_health_classify();
  test_unhealthy();
  test_metrics();
  test_duplicate();
  test_copytruncate();
  test_rotate_reject();
  test_rotate_publish_fail();
  test_clean_preflight();
  test_hostname_ws();
  test_mismatches();
  test_account();
  test_legacy_jobs();
  test_key();
  test_secrets();
  test_preflight_timeout();
  test_diff();
  test_deploy_ok();
  test_first_preflight();
  test_second_lock();
  test_restore();
  test_race();
  test_first_install();
  test_rollback();
  test_command();
  test_cli();
  char *rm = g_strconcat("rm -rf ", root, NULL);
  if (system(rm) != 0)
    fprintf(stderr, "cleanup failed\n");
  g_free(rm);
  g_free(root);
  g_free(fixture);
  g_free(bin_dir);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("ok");
  return 0;
}
