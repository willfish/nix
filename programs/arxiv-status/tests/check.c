#define _POSIX_C_SOURCE 200809L
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

static const char *current_test;
static int failures;
static char *binary;

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

static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    perror("malloc");
    exit(1);
  }
  return p;
}
static char *xstrdup(const char *s) {
  size_t n = strlen(s);
  char *out = xmalloc(n + 1);
  memcpy(out, s, n + 1);
  return out;
}
static char *join_path(const char *a, const char *b) {
  size_t na = strlen(a), nb = strlen(b);
  int slash = na && a[na - 1] != '/';
  char *out = xmalloc(na + (size_t)slash + nb + 1);
  memcpy(out, a, na);
  if (slash)
    out[na++] = '/';
  memcpy(out + na, b, nb + 1);
  return out;
}
static void write_all(int fd, const void *data, size_t n) {
  const char *p = data;
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      perror("write");
      exit(1);
    }
    off += (size_t)w;
  }
}
static void write_file(const char *path, const void *data, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    perror(path);
    exit(1);
  }
  write_all(fd, data, n);
  if (close(fd) < 0) {
    perror("close");
    exit(1);
  }
}
static int mkdir_p(const char *path) {
  char *copy = xstrdup(path);
  for (char *p = copy + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    if (mkdir(copy, 0755) < 0 && errno != EEXIST) {
      free(copy);
      return -1;
    }
    *p = '/';
  }
  int rc = mkdir(copy, 0755);
  free(copy);
  return rc < 0 && errno != EEXIST ? -1 : 0;
}
static void rm_rf(const char *path) {
  pid_t pid = fork();
  if (pid < 0)
    return;
  if (!pid) {
    execlp("rm", "rm", "-rf", "--", path, (char *)NULL);
    _exit(127);
  }
  int status;
  waitpid(pid, &status, 0);
}
static char *sibling(const char *env, const char *name) {
  const char *v = getenv(env);
  if (v && *v)
    return xstrdup(v);
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n < 0)
    return NULL;
  exe[n] = 0;
  char *slash = strrchr(exe, '/');
  if (!slash)
    return xstrdup(name);
  *slash = 0;
  return join_path(exe, name);
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
  bool error;
} Proc;
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
static Proc run_bin(char *const *argv, const char *cwd, const char *xdg_state,
                    const char *xdg_config, bool set_xdg, bool no_home,
                    const char *home) {
  Proc p = {.status = -1};
  int outp[2] = {-1, -1}, errp[2] = {-1, -1};
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
    dup2(outp[1], 1);
    dup2(errp[1], 2);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    if (cwd && chdir(cwd) < 0)
      _exit(127);
    unsetenv("XDG_STATE_HOME");
    unsetenv("XDG_CONFIG_HOME");
    if (no_home)
      unsetenv("HOME");
    else if (home && setenv("HOME", home, 1) < 0)
      _exit(127);
    if (set_xdg) {
      if (setenv("XDG_STATE_HOME", xdg_state ? xdg_state : "", 1) < 0 ||
          setenv("XDG_CONFIG_HOME", xdg_config ? xdg_config : "", 1) < 0)
        _exit(127);
    }
    execv(binary, argv);
    _exit(127);
  }
  close(outp[1]);
  close(errp[1]);
  size_t ocap = 0, elen = 0, ecap = 0;
  char *err = NULL;
  int64_t deadline = now_ms() + 5000;
  while (outp[0] >= 0 || errp[0] >= 0) {
    if (now_ms() > deadline) {
      kill(pid, SIGKILL);
      p.error = true;
      break;
    }
    struct pollfd fds[2];
    nfds_t nfd = 0;
    int oi = -1, ei = -1;
    if (outp[0] >= 0) {
      oi = (int)nfd;
      fds[nfd++] = (struct pollfd){outp[0], POLLIN, 0};
    }
    if (errp[0] >= 0) {
      ei = (int)nfd;
      fds[nfd++] = (struct pollfd){errp[0], POLLIN, 0};
    }
    poll(fds, nfd, 50);
    char tmp[4096];
    if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t r = read(outp[0], tmp, sizeof tmp);
      if (r > 0) {
        if (!grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r))
          p.error = true;
      } else {
        close(outp[0]);
        outp[0] = -1;
      }
    }
    if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t r = read(errp[0], tmp, sizeof tmp);
      if (r > 0) {
        if (!grow(&err, &elen, &ecap, tmp, (size_t)r))
          p.error = true;
      } else {
        close(errp[0]);
        errp[0] = -1;
      }
    }
  }
  int status;
  if (waitpid(pid, &status, 0) < 0)
    p.error = true;
  else if (WIFEXITED(status))
    p.status = WEXITSTATUS(status);
  else
    p.error = true;
  free(err);
  return p;
}
static void proc_clear(Proc *p) {
  free(p->out);
  memset(p, 0, sizeof *p);
}
static const unsigned char flask[] = {0xef, 0x83, 0x83};
static const char *hint = " · set interests in the panel";
static const char *scan = " · right-click scans now";
static yyjson_mut_val *mut_text(yyjson_mut_doc *doc, const void *bytes, size_t n) {
  return yyjson_mut_strncpy(doc, bytes, n);
}
static void expect_status(yyjson_val *got, const void *text, size_t text_len,
                          const char *tooltip, size_t tip_len, const char *cls) {
  yyjson_val *t = yyjson_obj_get(got, "text");
  yyjson_val *tip = yyjson_obj_get(got, "tooltip");
  yyjson_val *c = yyjson_obj_get(got, "class");
  EXPECT(t && yyjson_is_str(t) && yyjson_get_len(t) == text_len &&
         !memcmp(yyjson_get_str(t), text, text_len));
  EXPECT(tip && yyjson_is_str(tip) && yyjson_get_len(tip) == tip_len &&
         !memcmp(yyjson_get_str(tip), tooltip, tip_len));
  EXPECT(c && yyjson_is_str(c) && yyjson_get_len(c) == strlen(cls) &&
         !memcmp(yyjson_get_str(c), cls, strlen(cls)));
}
static char *scratch(void) {
  char tmpl[] = "/tmp/arxiv-status-XXXXXX";
  char *path = mkdtemp(tmpl);
  if (!path) {
    perror("mkdtemp");
    exit(1);
  }
  return xstrdup(path);
}
static void place(const char *root, const char *key, const char *value,
                  const char *location) {
  const char *name = !strcmp(key, "state")    ? "state.json"
                     : !strcmp(key, "viewed") ? "last_viewed.json"
                                              : "config.json";
  char *base;
  if (location && !strcmp(location, "empty"))
    base = xstrdup(root);
  else if (location && !strcmp(location, "default"))
    base = join_path(root, !strcmp(key, "config") ? ".config" : ".local/state");
  else
    base = join_path(root, !strcmp(key, "config") ? "config" : "state");
  char *dir = join_path(base, "omarchy-arxiv-scanner");
  if (mkdir_p(dir) < 0) {
    perror(dir);
    exit(1);
  }
  char *path = join_path(dir, name);
  write_file(path, value, strlen(value));
  free(path);
  free(dir);
  free(base);
}
static yyjson_val *run_case(const char *root, char *const *argv, const char *location,
                            bool no_home, size_t *out_len, yyjson_doc **doc) {
  char *state = join_path(root, "state");
  char *config = join_path(root, "config");
  bool set_xdg = !location || strcmp(location, "default");
  const char *xdg_state = location && !strcmp(location, "empty") ? "" : state;
  const char *xdg_config = location && !strcmp(location, "empty") ? "" : config;
  Proc p = run_bin(argv, root, xdg_state, xdg_config, set_xdg, no_home, root);
  free(state);
  free(config);
  EXPECT(!p.error);
  EXPECT(p.status == 0);
  EXPECT(p.out && p.out_len && p.out[p.out_len - 1] == '\n');
  size_t lines = 0;
  for (size_t i = 0; i < p.out_len; i++)
    if (p.out[i] == '\n')
      lines++;
  EXPECT(lines == 1);
  *doc = yyjson_read(p.out, p.out_len, 0);
  EXPECT(*doc);
  yyjson_val *rootv = *doc ? yyjson_doc_get_root(*doc) : NULL;
  *out_len = p.out_len;
  proc_clear(&p);
  return rootv;
}
static void prepare_dirs(const char *root) {
  const char *tails[] = {"state/omarchy-arxiv-scanner",
                         "config/omarchy-arxiv-scanner",
                         "omarchy-arxiv-scanner",
                         ".local/state/omarchy-arxiv-scanner",
                         ".config/omarchy-arxiv-scanner",
                         NULL};
  for (size_t i = 0; tails[i]; i++) {
    char *path = join_path(root, tails[i]);
    if (mkdir_p(path) < 0) {
      perror(path);
      exit(1);
    }
    free(path);
  }
}
static char *tip(const char *label, const char *middle, bool interests) {
  size_t n = strlen(label) + strlen(middle) + strlen(scan) +
             (interests ? strlen(hint) : 0) + 1;
  char *out = xmalloc(n);
  snprintf(out, n, "%s%s%s%s", label, middle, interests ? hint : "", scan);
  return out;
}
static void begin(const char *name) {
  current_test = name;
  printf("RUN %s\n", name);
  fflush(stdout);
}
int main(void) {
  binary = sibling("ARXIV_STATUS_BIN", "hypr-arxiv-status");
  if (!binary) {
    fprintf(stderr, "Set ARXIV_STATUS_BIN to the candidate executable\n");
    return 1;
  }
  char *argv0[] = {binary, NULL};
  char *argv_ignored[] = {binary, "--ignored", NULL};
  begin("missing, malformed and non-object files retain the idle flask and hints");
  const char *states[] = {NULL, "{broken", "[]", "null", "42", "true"};
  for (size_t i = 0; i < 6; i++) {
    char *root = scratch();
    prepare_dirs(root);
    if (states[i]) {
      place(root, "state", states[i], NULL);
      place(root, "config", "[1]", NULL);
      place(root, "viewed", "\"text\"", NULL);
    }
    yyjson_doc *doc = NULL;
    size_t unused = 0;
    yyjson_val *got = run_case(root, argv0, NULL, false, &unused, &doc);
    char *tooltip = tip("arXiv", ": 0 match(es)", true);
    if (got)
      expect_status(got, flask, sizeof flask, tooltip, strlen(tooltip), "idle");
    free(tooltip);
    yyjson_doc_free(doc);
    rm_rf(root);
    free(root);
  }
  begin("matching view timestamps remove the unseen marker and total both arrays");
  {
    char *root = scratch();
    prepare_dirs(root);
    place(root, "state",
          "{\"area_matches\":[1,2],\"watched_matches\":[null],\"updated_at\":\"now\"}",
          NULL);
    place(root, "viewed", "{\"viewed_at\":\"now\"}", NULL);
    place(root, "config",
          "{\"category\":\"cs.AI\",\"interestAreas\":[\"agents\"]}", NULL);
    yyjson_doc *doc = NULL;
    size_t unused = 0;
    yyjson_val *got = run_case(root, argv0, NULL, false, &unused, &doc);
    char *tooltip = tip("cs.AI", ": 3 match(es)", false);
    if (got)
      expect_status(got, "3", 1, tooltip, strlen(tooltip), "idle");
    free(tooltip);
    yyjson_doc_free(doc);
    rm_rf(root);
    free(root);
  }
  begin("a changed timestamp marks unseen even with no matches");
  for (int count = 0; count <= 2; count += 2) {
    char *root = scratch();
    prepare_dirs(root);
    char state[128];
    snprintf(state, sizeof state,
             "{\"area_matches\":%s,\"updated_at\":\"new\"}",
             count ? "[0,0]" : "[]");
    place(root, "state", state, NULL);
    place(root, "viewed", "{\"viewed_at\":\"old\"}", NULL);
    place(root, "config", "{\"watchedAuthors\":[null]}", NULL);
    yyjson_doc *doc = NULL;
    size_t unused = 0;
    yyjson_val *got = run_case(root, argv0, NULL, false, &unused, &doc);
    char middle[80];
    snprintf(middle, sizeof middle,
             ": %d match(es) · not opened since the last scan", count);
    char *tooltip = tip("arXiv", middle, false);
    char text[8];
    if (count)
      snprintf(text, sizeof text, "!%d", count);
    if (got)
      expect_status(got, count ? text : (const char *)flask,
                    count ? strlen(text) : sizeof flask, tooltip, strlen(tooltip),
                    "unseen");
    free(tooltip);
    yyjson_doc_free(doc);
    rm_rf(root);
    free(root);
  }
  begin("only arrays and strings count; empty categories fall back and non-string timestamps are idle");
  const char *categories[] = {"\"\"", "null", "true", "12", "{}", "[]"};
  for (size_t i = 0; i < 6; i++) {
    char *root = scratch();
    prepare_dirs(root);
    place(root, "state",
          "{\"area_matches\":{\"a\":1},\"watched_matches\":\"papers\",\"updated_at\":100}",
          NULL);
    char config[160];
    snprintf(config, sizeof config,
             "{\"category\":%s,\"interestAreas\":\"not an array\",\"watchedAuthors\":{}}",
             categories[i]);
    place(root, "config", config, NULL);
    yyjson_doc *doc = NULL;
    size_t unused = 0;
    yyjson_val *got = run_case(root, argv0, NULL, false, &unused, &doc);
    char *tooltip = tip("arXiv", ": 0 match(es)", true);
    if (got)
      expect_status(got, flask, sizeof flask, tooltip, strlen(tooltip), "idle");
    free(tooltip);
    yyjson_doc_free(doc);
    rm_rf(root);
    free(root);
  }
  begin("Unicode, quotes, line breaks and embedded NULs remain JSON-safe and distinct");
  {
    char *root = scratch();
    prepare_dirs(root);
    const char category[] = "数学 \"AI\"\n";
    char state[64];
    snprintf(state, sizeof state, "{\"updated_at\":\"a\\u0000b\"}");
    place(root, "state", state, NULL);
    place(root, "viewed", "{\"viewed_at\":\"a\\u0000c\"}", NULL);
    yyjson_mut_doc *cfg = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *croot = yyjson_mut_obj(cfg);
    yyjson_mut_doc_set_root(cfg, croot);
    char cat[32];
    size_t cat_len = 0;
    memcpy(cat, category, sizeof category - 1);
    cat_len = sizeof category - 1;
    cat[cat_len++] = 0;
    yyjson_mut_obj_add_val(cfg, croot, "category", mut_text(cfg, cat, cat_len));
    size_t cfg_len = 0;
    char *cfg_json = yyjson_mut_write(cfg, 0, &cfg_len);
    if (!cfg_json) {
      FAIL("config allocation failed");
      return 1;
    }
    char *base = join_path(root, "config/omarchy-arxiv-scanner");
    char *path = join_path(base, "config.json");
    write_file(path, cfg_json, cfg_len);
    free(path);
    free(base);
    free(cfg_json);
    yyjson_mut_doc_free(cfg);
    yyjson_doc *doc = NULL;
    size_t unused = 0;
    yyjson_val *got = run_case(root, argv0, NULL, false, &unused, &doc);
    const char *mid = ": 0 match(es) · not opened since the last scan";
    size_t tip_len = cat_len + strlen(mid) + strlen(hint) + strlen(scan);
    char *tooltip = xmalloc(tip_len);
    memcpy(tooltip, cat, cat_len);
    memcpy(tooltip + cat_len, mid, strlen(mid));
    memcpy(tooltip + cat_len + strlen(mid), hint, strlen(hint));
    memcpy(tooltip + cat_len + strlen(mid) + strlen(hint), scan, strlen(scan));
    if (got)
      expect_status(got, flask, sizeof flask, tooltip, tip_len, "unseen");
    free(tooltip);
    yyjson_doc_free(doc);
    rm_rf(root);
    free(root);
  }
  begin("HOME defaults, unset HOME and explicitly empty XDG variables preserve relative paths");
  const char *locations[] = {"default", "default", "empty"};
  bool no_home[] = {false, true, false};
  for (size_t i = 0; i < 3; i++) {
    char *root = scratch();
    prepare_dirs(root);
    place(root, "state", "{\"area_matches\":[1]}", locations[i]);
    place(root, "config", "{\"interestAreas\":[1]}", locations[i]);
    yyjson_doc *doc = NULL;
    size_t unused = 0;
    yyjson_val *got =
        run_case(root, argv0, locations[i], no_home[i], &unused, &doc);
    yyjson_val *text = got ? yyjson_obj_get(got, "text") : NULL;
    EXPECT(text && yyjson_get_len(text) == 1 && yyjson_get_str(text)[0] == '1');
    yyjson_doc_free(doc);
    rm_rf(root);
    free(root);
  }
  begin("duplicate keys keep the last value and NaN/Infinity in arrays do not lose records");
  {
    char *root = scratch();
    prepare_dirs(root);
    place(root, "state",
          "{\"area_matches\":[],\"area_matches\":[NaN,Infinity,-Infinity],\"updated_at\":\"old\",\"updated_at\":\"\"}",
          NULL);
    place(root, "config", "{\"category\":\"old\",\"category\":\"new\"}", NULL);
    yyjson_doc *doc = NULL;
    size_t unused = 0;
    yyjson_val *got = run_case(root, argv_ignored, NULL, false, &unused, &doc);
    char *tooltip = tip("new", ": 3 match(es)", true);
    if (got)
      expect_status(got, "3", 1, tooltip, strlen(tooltip), "idle");
    free(tooltip);
    yyjson_doc_free(doc);
    rm_rf(root);
    free(root);
  }
  free(binary);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("ok");
  return 0;
}
