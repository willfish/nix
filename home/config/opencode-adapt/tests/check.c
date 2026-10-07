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

static const char *current_test;
static int failures;
static char *bin_path;
static char *failures_path;
static const char *note =
    "Load these skills when the task reaches their trigger: ";

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

static char *sibling(const char *env, const char *name) {
  const char *value = getenv(env);
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
  unsigned char *out;
  size_t out_len;
  unsigned char *err;
  size_t err_len;
  bool spawn_error;
} Proc;

static void proc_clear(Proc *p) {
  g_free(p->out);
  g_free(p->err);
  memset(p, 0, sizeof *p);
  p->status = -1;
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool grow(unsigned char **buf, size_t *len, size_t *cap, const void *add,
                 size_t n) {
  if (*len > SIZE_MAX - n)
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

static Proc proc_run(const char *cmd, char *const *argv, const void *input,
                     size_t input_len, const char *const *env_set,
                     const char *cwd, int timeout_ms) {
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
    if (env_set) {
      for (size_t i = 0; env_set[i]; i++)
        putenv((char *)env_set[i]);
    }
    signal(SIGPIPE, SIG_DFL);
    execvp(cmd, argv);
    _exit(127);
  }
  close(inp[0]);
  close(outp[1]);
  close(errp[1]);
  signal(SIGPIPE, SIG_IGN);
  size_t in_off = 0, oc = 0, ec = 0, ocap = 0, ecap = 0;
  int64_t deadline = now_ms() + timeout_ms;
  bool child_done = false;
  int status = 0;
  while (!child_done || in_off < input_len) {
    if (now_ms() > deadline) {
      kill(pid, SIGKILL);
      p.spawn_error = true;
      break;
    }
    struct pollfd fds[3];
    nfds_t nfd = 0;
    int in_i = -1, out_i = -1, err_i = -1;
    if (in_off < input_len) {
      in_i = (int)nfd;
      fds[nfd++] = (struct pollfd){inp[1], POLLOUT, 0};
    }
    if (outp[0] >= 0) {
      out_i = (int)nfd;
      fds[nfd++] = (struct pollfd){outp[0], POLLIN, 0};
    }
    if (errp[0] >= 0) {
      err_i = (int)nfd;
      fds[nfd++] = (struct pollfd){errp[0], POLLIN, 0};
    }
    int left = (int)(deadline - now_ms());
    if (left < 0)
      left = 0;
    if (left > 50)
      left = 50;
    poll(fds, nfd, left);
    if (in_i >= 0 && (fds[in_i].revents & POLLOUT)) {
      ssize_t w = write(inp[1], (const char *)input + in_off, input_len - in_off);
      if (w > 0)
        in_off += (size_t)w;
      else if (w < 0 && errno != EINTR)
        in_off = input_len;
    }
    unsigned char tmp[4096];
    if (out_i >= 0 && (fds[out_i].revents & (POLLIN | POLLHUP))) {
      ssize_t r = read(outp[0], tmp, sizeof tmp);
      if (r > 0) {
        if (!grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r))
          p.spawn_error = true;
      } else if (r == 0) {
        close(outp[0]);
        outp[0] = -1;
      }
    }
    (void)oc;
    (void)ec;
    if (err_i >= 0 && (fds[err_i].revents & (POLLIN | POLLHUP))) {
      ssize_t r = read(errp[0], tmp, sizeof tmp);
      if (r > 0) {
        if (!grow(&p.err, &p.err_len, &ecap, tmp, (size_t)r))
          p.spawn_error = true;
      } else if (r == 0) {
        close(errp[0]);
        errp[0] = -1;
      }
    }
    int wstat = 0;
    pid_t got = waitpid(pid, &wstat, WNOHANG);
    if (got == pid) {
      child_done = true;
      status = WIFEXITED(wstat) ? WEXITSTATUS(wstat) : 128 + WTERMSIG(wstat);
      if (in_off >= input_len || !input_len)
        break;
    }
  }
  if (in_off >= input_len || !input)
    close(inp[1]);
  if (!child_done) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    p.spawn_error = true;
  } else
    p.status = status;
  close(inp[1]);
  if (outp[0] >= 0) {
    unsigned char tmp[4096];
    ssize_t r;
    while ((r = read(outp[0], tmp, sizeof tmp)) > 0)
      grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r);
    close(outp[0]);
  }
  if (errp[0] >= 0) {
    unsigned char tmp[4096];
    ssize_t r;
    while ((r = read(errp[0], tmp, sizeof tmp)) > 0)
      grow(&p.err, &p.err_len, &ecap, tmp, (size_t)r);
    close(errp[0]);
  }
  if (!p.out)
    grow(&p.out, &p.out_len, &ocap, "", 0);
  if (!p.err)
    grow(&p.err, &p.err_len, &ecap, "", 0);
  if (!child_done)
    waitpid(pid, NULL, 0);
  return p;
}

static bool sane(const Proc *p) {
  const char *needles[] = {"ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
                           "runtime error:", "DEADLYSIGNAL"};
  for (size_t i = 0; i < G_N_ELEMENTS(needles); i++) {
    size_t n = strlen(needles[i]);
    if (p->err_len >= n) {
      for (size_t j = 0; j + n <= p->err_len; j++)
        if (!memcmp(p->err + j, needles[i], n))
          return false;
    }
  }
  return true;
}

static bool bytes_eq(const void *a, size_t an, const void *b, size_t bn) {
  return an == bn && (an == 0 || !memcmp(a, b, an));
}

static void expect_status(Proc *p, int status) {
  if (p->spawn_error)
    FAIL("spawn failed");
  if (!sane(p))
    FAIL("sanitizer diagnostic: %s", p->err);
  if (p->status != status) {
    FAIL("status %d != %d\nstderr: %s\nstdout: %s", p->status, status, p->err,
         p->out);
  }
}

static char *temp_root(void) {
  char *tmpl = g_build_filename(g_get_tmp_dir(), "opencode-adapt-XXXXXX", NULL);
  char *made = g_mkdtemp(tmpl);
  if (!made) {
    g_free(tmpl);
    return NULL;
  }
  return made;
}

static void rm_rf(const char *path) {
  if (!path)
    return;
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
    if (!lstat(child, &st) && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
      rm_rf(child);
    else
      unlink(child);
    g_free(child);
  }
  closedir(dir);
  rmdir(path);
}

static bool write_all(const char *path, const void *data, size_t len, mode_t mode) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0)
    return false;
  const char *p = data;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, p + off, len - off);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      return false;
    }
    off += (size_t)n;
  }
  return close(fd) == 0;
}

static bool read_all(const char *path, unsigned char **data, size_t *len) {
  gchar *buf = NULL;
  gsize n = 0;
  if (!g_file_get_contents(path, &buf, &n, NULL))
    return false;
  *data = (unsigned char *)buf;
  *len = n;
  return true;
}

static char *preload_value(void) {
  const char *asan = getenv("OPENCODE_ADAPT_ASAN_RT");
  GString *s = g_string_new(NULL);
  if (asan && *asan) {
    g_string_append(s, asan);
    g_string_append_c(s, ':');
  }
  g_string_append(s, failures_path);
  return g_string_free(s, false);
}

static Proc adapt_run(const char *src, const char *dst, const char *kind,
                      bool old, const char *const *extra_env) {
  const char *python = getenv("OPENCODE_ADAPT_PYTHON");
  if (!python || !*python)
    python = "python3";
  const char *legacy = getenv("OPENCODE_ADAPT_LEGACY");
  char *argv_store[8];
  int argc = 0;
  const char *cmd;
  if (old) {
    cmd = python;
    argv_store[argc++] = (char *)python;
    argv_store[argc++] = (char *)legacy;
  } else {
    cmd = bin_path;
    argv_store[argc++] = bin_path;
  }
  argv_store[argc++] = "--kind";
  argv_store[argc++] = (char *)kind;
  argv_store[argc++] = (char *)src;
  argv_store[argc++] = (char *)dst;
  argv_store[argc] = NULL;
  char *preload = NULL;
  const char *env_local[8];
  size_t en = 0;
  if (extra_env) {
    preload = preload_value();
    char *ld = g_strconcat("LD_PRELOAD=", preload, NULL);
    env_local[en++] = ld;
    for (size_t i = 0; extra_env[i] && en < 6; i++)
      env_local[en++] = extra_env[i];
    env_local[en] = NULL;
    Proc p = proc_run(cmd, argv_store, NULL, 0, env_local, NULL, 10000);
    g_free((char *)env_local[0]);
    g_free(preload);
    return p;
  }
  return proc_run(cmd, argv_store, NULL, 0, NULL, NULL, 10000);
}

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

static bool legacy_enabled(void) {
  const char *v = getenv("OPENCODE_ADAPT_LEGACY");
  return v && *v;
}

static void text_case(const void *input, size_t input_len, const char *kind,
                      const void *expected, size_t expected_len, bool has_expected) {
  unsigned char *native = NULL;
  size_t native_len = 0;
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_root();
    EXPECT(root);
    if (!root)
      return;
    char *src = g_build_filename(root, "source", NULL);
    char *dst = g_build_filename(root, "dest", NULL);
    EXPECT(g_mkdir_with_parents(src, 0700) == 0);
    char *sample = g_build_filename(src, "sample.md", NULL);
    EXPECT(write_all(sample, input, input_len, 0644));
    Proc r = adapt_run(src, dst, kind, old, NULL);
    expect_status(&r, 0);
    char *out_path = g_build_filename(dst, "sample.md", NULL);
    unsigned char *data = NULL;
    size_t n = 0;
    if (!read_all(out_path, &data, &n))
      FAIL("missing output");
    else {
      if (has_expected && !bytes_eq(data, n, expected, expected_len))
        FAIL("output mismatch for %s (%s)", kind, old ? "legacy" : "native");
      if (old) {
        if (!bytes_eq(data, n, native, native_len))
          FAIL("legacy output differs");
      } else {
        g_free(native);
        native = data;
        native_len = n;
        data = NULL;
      }
    }
    g_free(data);
    proc_clear(&r);
    g_free(out_path);
    g_free(sample);
    g_free(src);
    g_free(dst);
    rm_rf(root);
    g_free(root);
  }
  g_free(native);
}

static void text_str(const char *input, const char *kind, const char *expected) {
  text_case(input, strlen(input), kind, expected, expected ? strlen(expected) : 0,
            expected != NULL);
}

static char *hex_of(const unsigned char *data, size_t n) {
  char *s = g_try_malloc(n * 2 + 1);
  if (!s)
    return NULL;
  for (size_t i = 0; i < n; i++)
    snprintf(s + i * 2, 3, "%02x", data[i]);
  return s;
}

typedef struct {
  char *name_hex;
  char *data_hex;
} TreeItem;

static int tree_cmp(const void *a, const void *b) {
  return strcmp(((const TreeItem *)a)->name_hex, ((const TreeItem *)b)->name_hex);
}

static bool tree_of(const char *path, TreeItem **out, size_t *count) {
  DIR *dir = opendir(path);
  if (!dir)
    return false;
  size_t n = 0, cap = 0;
  TreeItem *items = NULL;
  struct dirent *ent;
  while ((ent = readdir(dir))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    char *child = g_build_filename(path, ent->d_name, NULL);
    unsigned char *data = NULL;
    size_t len = 0;
    if (!read_all(child, &data, &len)) {
      g_free(child);
      closedir(dir);
      g_free(data);
      for (size_t i = 0; i < n; i++) {
        g_free(items[i].name_hex);
        g_free(items[i].data_hex);
      }
      g_free(items);
      return false;
    }
    if (n == cap) {
      cap = cap ? cap * 2 : 8;
      TreeItem *next = g_try_realloc(items, cap * sizeof *items);
      if (!next) {
        g_free(child);
        g_free(data);
        closedir(dir);
        return false;
      }
      items = next;
    }
    items[n].name_hex = hex_of((const unsigned char *)ent->d_name, strlen(ent->d_name));
    items[n].data_hex = hex_of(data, len);
    n++;
    g_free(data);
    g_free(child);
  }
  closedir(dir);
  qsort(items, n, sizeof *items, tree_cmp);
  *out = items;
  *count = n;
  return true;
}

static void free_tree(TreeItem *items, size_t n) {
  for (size_t i = 0; i < n; i++) {
    g_free(items[i].name_hex);
    g_free(items[i].data_hex);
  }
  g_free(items);
}

static bool trees_eq(TreeItem *a, size_t an, TreeItem *b, size_t bn) {
  if (an != bn)
    return false;
  for (size_t i = 0; i < an; i++)
    if (strcmp(a[i].name_hex, b[i].name_hex) || strcmp(a[i].data_hex, b[i].data_hex))
      return false;
  return true;
}

static void utf8(gunichar cp, char out[5], size_t *n) {
  *n = (size_t)g_unichar_to_utf8(cp, out);
  out[*n] = 0;
}

static void test_agent_allowlist(void) {
  begin("agent allowlist, field order, default mode and skill-note construction retain exact output");
  char *expected = g_strconcat(
      "---\ndescription: Do work\nmode: subagent\nmodel: xai/grok-4.7\n---\nDo the work.\n\n",
      note, "verification-before-completion.\n", NULL);
  text_str("---\nname: builder\ndescription: Do work\ntools: read, bash\nskills: [verification-before-completion]\nmodel: xai/grok-4.7\nthinking: medium\n---\nDo the work.\n",
           "agent", expected);
  g_free(expected);
  end_case();
}

static void test_command_whitelist(void) {
  begin("command whitelist and global body-only argument replacement retain raw values");
  text_str("---\nsubtask: true\nmodel: x/y\nagent: architect\ndescription: Keep $@ here\nargument-hint: \"<brief>\"\nmode: primary\nskills: [none]\n---\nUse $@ $@, \\$@ and $ARGUMENTS.",
           "command",
           "---\ndescription: Keep $@ here\nagent: architect\nmodel: x/y\nsubtask: true\n---\nUse $ARGUMENTS $ARGUMENTS, \\$ARGUMENTS and $ARGUMENTS.");
  end_case();
}

static void test_modes(void) {
  begin("absent and empty modes differ; last duplicate fields win including empty strings");
  const char *bodies[] = {"---\nmode:\n---\nBody", "---\nmode: primary\nmode: \n---\nBody",
                          "---\nmode: null\n---\nBody",
                          "---\ndescription: first\nmodel: old\ndescription: last\nmodel: new\n---\nBody",
                          "---\nDescription: ignored\nmodel: \"quoted\"\n---\nBody"};
  for (size_t i = 0; i < G_N_ELEMENTS(bodies); i++)
    text_str(bodies[i], "agent", NULL);
  text_str("---\nmode:\n---\nBody", "agent", "---\n---\nBody");
  text_str("Body", "agent", "---\nmode: subagent\n---\nBody");
  end_case();
}

static void test_frontmatter_anchors(void) {
  begin("frontmatter requires exact opening and first closing anchors, including the empty-header edge");
  const char *values[] = {"",
                          "Body\n",
                          "\n---\ndescription: x\n---\nBody",
                          "---",
                          "---\ndescription: x\n---",
                          "---\n---\nBody",
                          "---\n\n---\nBody",
                          "---\ndescription: first\n---\n---\ndescription: second\n---\nBody",
                          "\uFEFF---\ndescription: x\n---\nBody"};
  const char *kinds[] = {"agent", "command"};
  for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
    for (size_t k = 0; k < 2; k++)
      text_str(values[i], kinds[k], NULL);
  end_case();
}

static void test_crlf(void) {
  begin("reads normalize CRLF and lone CR before frontmatter recognition and preserve final-newline choices");
  const char *values[] = {"---\r\ndescription: x\r\n---\r\n\r\nBody\r\n",
                          "---\rdescription: x\r---\rBody",
                          "---\ndescription: x\n---\n\n\nBody",
                          "\n\nBody",
                          "\n\n",
                          "Body\n\n"};
  const char *kinds[] = {"agent", "command"};
  for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
    for (size_t k = 0; k < 2; k++)
      text_str(values[i], kinds[k], NULL);
  end_case();
}

static void test_splitlines(void) {
  begin("frontmatter splitlines covers Python line separators, but not every whitespace code point");
  gunichar cps[] = {10, 11, 12, 13, 28, 29, 30, 0x85, 0x2028, 0x2029};
  for (size_t i = 0; i < G_N_ELEMENTS(cps); i++) {
    char sep[5];
    size_t n = 0;
    utf8(cps[i], sep, &n);
    char *input = g_strconcat("---\ndescription: first", sep, "model: x/y\n---\nBody", NULL);
    text_str(input, "agent",
             "---\ndescription: first\nmode: subagent\nmodel: x/y\n---\nBody");
    g_free(input);
  }
  text_str("---\ndescription: first\x1fmodel: x/y\n---\nBody", "agent",
           "---\ndescription: first\x1fmodel: x/y\nmode: subagent\n---\nBody");
  end_case();
}

static void test_indent_strip(void) {
  begin("only ASCII-space-indented fields are skipped; keys and values use exact Unicode stripping");
  text_str("---\n description: ignored\n\tdescription\t: \u2003kept\xC2\x85\n\u2003model: x/y\nmode :  primary\n---\nBody",
           "agent", "---\ndescription: kept\nmode: primary\nmodel: x/y\n---\nBody");
  gunichar ws[] = {9, 11, 12, 28, 29, 30, 31, 32, 0x85, 0xa0, 0x1680, 0x2000, 0x2001,
                   0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a,
                   0x2028, 0x2029, 0x202f, 0x205f, 0x3000};
  char *expected = g_strconcat("---\nmode: subagent\n---\nBody\n\n", note, "one.\n", NULL);
  for (size_t i = 0; i < G_N_ELEMENTS(ws); i++) {
    char sep[5];
    size_t n = 0;
    utf8(ws[i], sep, &n);
    char *input = g_strconcat("---\nskills: [one]\n---\nBody", sep, NULL);
    text_str(input, "agent", expected);
    g_free(input);
  }
  g_free(expected);
  end_case();
}

static void test_skills_split(void) {
  begin("skills use character-set stripping and comma splitting, not YAML parsing");
  const char *skills[] = {"[]", "[ ]", "[[[a]]]", "[a, b,, a]", "\"[a,b]\"", "[ , , ]",
                          "[\u2003]", "[one\x00two]", "[,first,,last,]"};
  for (size_t i = 0; i < G_N_ELEMENTS(skills); i++) {
    if (i == 7) {
      const char prefix[] = "---\nskills: [one";
      const char suffix[] = "two]\n---\nBody \n";
      size_t n = sizeof prefix - 1 + 1 + sizeof suffix - 1;
      char *input = g_malloc(n);
      memcpy(input, prefix, sizeof prefix - 1);
      input[sizeof prefix - 1] = 0;
      memcpy(input + sizeof prefix - 1 + 1, suffix, sizeof suffix - 1);
      text_case(input, n, "agent", NULL, 0, false);
      g_free(input);
      continue;
    }
    char *input = g_strconcat("---\nskills: ", skills[i], "\n---\nBody \n", NULL);
    text_str(input, "agent", NULL);
    g_free(input);
  }
  char *expected = g_strconcat("---\nmode: subagent\n---\nBody\n\n", note, ".\n", NULL);
  text_str("---\nskills: [ , , ]\n---\nBody", "agent", expected);
  g_free(expected);
  end_case();
}

static void test_skill_dedup(void) {
  begin("skill notes are deduplicated by exact trimmed substring anywhere in the body");
  char *sentence = g_strconcat(note, "one, two.", NULL);
  char *bodies[] = {g_strdup(sentence),
                    g_strconcat("Prefix ", sentence, " suffix\n", NULL),
                    g_strconcat(sentence, "\n\n", NULL), NULL};
  GString *lower = g_string_new("Different case: ");
  for (const char *p = sentence; *p; p++)
    g_string_append_c(lower, g_ascii_tolower(*p));
  bodies[3] = g_string_free(lower, false);
  for (size_t i = 0; i < 4; i++) {
    char *input = g_strconcat("---\nskills: [one, two]\n---\n", bodies[i], NULL);
    text_str(input, "agent", NULL);
    g_free(input);
    g_free(bodies[i]);
  }
  char *input = g_strconcat("---\nskills: [one, two]\n---\nPrefix ", sentence, " suffix \n", NULL);
  char *expected = g_strconcat("---\nmode: subagent\n---\nPrefix ", sentence, " suffix \n", NULL);
  text_str(input, "agent", expected);
  g_free(input);
  g_free(expected);
  g_free(sentence);
  end_case();
}

static void test_nul_astral(void) {
  begin("valid embedded NUL and astral characters remain data in keys, values, notes and bodies");
  const char head[] = "---\ndescription: 尾🙂";
  const char mid[] = "value\nmodel";
  const char tail[] = ": ignored\nskills: [first";
  const char tail2[] = "skill, last]\n---\nBefore";
  const char tail3[] = "$@ after🙂\n";
  size_t n = sizeof head - 1 + 1 + sizeof mid - 1 + 1 + sizeof tail - 1 + 1 +
             sizeof tail2 - 1 + 1 + sizeof tail3 - 1;
  char *input = g_malloc(n);
  size_t at = 0;
  memcpy(input + at, head, sizeof head - 1);
  at += sizeof head - 1;
  input[at++] = 0;
  memcpy(input + at, mid, sizeof mid - 1);
  at += sizeof mid - 1;
  input[at++] = 0;
  memcpy(input + at, tail, sizeof tail - 1);
  at += sizeof tail - 1;
  input[at++] = 0;
  memcpy(input + at, tail2, sizeof tail2 - 1);
  at += sizeof tail2 - 1;
  input[at++] = 0;
  memcpy(input + at, tail3, sizeof tail3 - 1);
  const char *kinds[] = {"agent", "command"};
  for (size_t k = 0; k < 2; k++)
    text_case(input, n, kinds[k], NULL, 0, false);
  g_free(input);
  char *note_a = g_strconcat(note, "a.", NULL);
  size_t en = strlen("---\nmode: subagent\n---\n") + 1 + strlen(note_a) + 1;
  char *expected = g_malloc(en);
  size_t e = 0;
  memcpy(expected + e, "---\nmode: subagent\n---\n", strlen("---\nmode: subagent\n---\n"));
  e += strlen("---\nmode: subagent\n---\n");
  expected[e++] = 0;
  memcpy(expected + e, note_a, strlen(note_a));
  e += strlen(note_a);
  expected[e++] = 0;
  size_t in = strlen("---\nskills: [a]\n---\n") + 1 + strlen(note_a) + 1;
  char *body = g_malloc(in);
  size_t b = 0;
  memcpy(body + b, "---\nskills: [a]\n---\n", strlen("---\nskills: [a]\n---\n"));
  b += strlen("---\nskills: [a]\n---\n");
  body[b++] = 0;
  memcpy(body + b, note_a, strlen(note_a));
  b += strlen(note_a);
  body[b++] = 0;
  text_case(body, in, "agent", expected, en, true);
  g_free(body);
  g_free(expected);
  g_free(note_a);
  end_case();
}

static void test_invalid_utf8(void) {
  begin("invalid UTF-8 anywhere in a source fails before truncating that destination");
  const unsigned char samples[][3] = {{255}, {0, 255}, {0xed, 0xa0, 0x80}};
  const size_t lens[] = {1, 2, 3};
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    for (size_t i = 0; i < 3; i++) {
      char *root = temp_root();
      EXPECT(root);
      if (!root)
        return;
      char *src = g_build_filename(root, "src", NULL);
      char *dst = g_build_filename(root, "dst", NULL);
      g_mkdir_with_parents(src, 0700);
      g_mkdir_with_parents(dst, 0700);
      char *in = g_build_filename(src, "x.md", NULL);
      char *out = g_build_filename(dst, "x.md", NULL);
      EXPECT(write_all(in, samples[i], lens[i], 0644));
      EXPECT(write_all(out, "keep", 4, 0644));
      Proc r = adapt_run(src, dst, "agent", old, NULL);
      expect_status(&r, 1);
      unsigned char *data = NULL;
      size_t n = 0;
      EXPECT(read_all(out, &data, &n));
      EXPECT(bytes_eq(data, n, "keep", 4));
      g_free(data);
      proc_clear(&r);
      g_free(in);
      g_free(out);
      g_free(src);
      g_free(dst);
      rm_rf(root);
      g_free(root);
    }
  }
  end_case();
}

static bool names_eq(char **names, size_t n, const char *const *expected, size_t en) {
  if (n != en)
    return false;
  for (size_t i = 0; i < n; i++)
    if (strcmp(names[i], expected[i]))
      return false;
  return true;
}

static char **dir_names(const char *path, size_t *count, bool sort) {
  DIR *dir = opendir(path);
  if (!dir)
    return NULL;
  char **names = NULL;
  size_t n = 0, cap = 0;
  struct dirent *ent;
  while ((ent = readdir(dir))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 8;
      char **next = g_try_realloc(names, cap * sizeof *names);
      if (!next) {
        closedir(dir);
        return NULL;
      }
      names = next;
    }
    names[n++] = g_strdup(ent->d_name);
  }
  closedir(dir);
  if (sort) {
    for (size_t i = 0; i < n; i++)
      for (size_t j = i + 1; j < n; j++)
        if (strcmp(names[j], names[i]) < 0) {
          char *tmp = names[i];
          names[i] = names[j];
          names[j] = tmp;
        }
  }
  *count = n;
  return names;
}

static void test_glob(void) {
  begin("shallow glob includes hidden .md names and excludes uppercase extensions and nested markdown");
  int passes = legacy_enabled() ? 2 : 1;
  const char *expected[] = {".hidden.md", ".md", "a.md"};
  for (int old = 0; old < passes; old++) {
    char *root = temp_root();
    char *src = g_build_filename(root, "src", NULL);
    char *dst = g_build_filename(root, "dst", NULL);
    g_mkdir_with_parents(src, 0700);
    const char *files[] = {"a.md", ".hidden.md", ".md", "UPPER.MD", "readme.txt"};
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++) {
      char *p = g_build_filename(src, files[i], NULL);
      EXPECT(write_all(p, "Body", 4, 0644));
      g_free(p);
    }
    char *nested = g_build_filename(src, "nested", NULL);
    g_mkdir_with_parents(nested, 0700);
    char *deep = g_build_filename(nested, "deep.md", NULL);
    EXPECT(write_all(deep, "Ignore", 6, 0644));
    Proc r = adapt_run(src, dst, "command", old, NULL);
    expect_status(&r, 0);
    size_t n = 0;
    char **names = dir_names(dst, &n, true);
    EXPECT(names && names_eq(names, n, expected, 3));
    for (size_t i = 0; i < n; i++)
      g_free(names[i]);
    g_free(names);
    proc_clear(&r);
    g_free(deep);
    g_free(nested);
    g_free(src);
    g_free(dst);
    rm_rf(root);
    g_free(root);
  }
  end_case();
}

static void test_empty_globs(void) {
  begin("missing, non-directory and unsearchable sources yield empty globs after destination creation");
  int passes = legacy_enabled() ? 2 : 1;
  const char *types[3];
  size_t nt = 0;
  types[nt++] = "missing";
  types[nt++] = "file";
  if (geteuid() != 0)
    types[nt++] = "unreadable";
  for (int old = 0; old < passes; old++) {
    for (size_t t = 0; t < nt; t++) {
      char *root = temp_root();
      char *src = g_build_filename(root, "src", NULL);
      char *dst = g_build_filename(root, "new/sub", NULL);
      if (!strcmp(types[t], "file"))
        EXPECT(write_all(src, "not a directory", 15, 0644));
      if (!strcmp(types[t], "unreadable")) {
        g_mkdir_with_parents(src, 0700);
        char *hidden = g_build_filename(src, "a.md", NULL);
        EXPECT(write_all(hidden, "hidden", 6, 0644));
        g_free(hidden);
        EXPECT(chmod(src, 0) == 0);
      }
      Proc r = adapt_run(src, dst, "agent", old, NULL);
      expect_status(&r, 0);
      size_t n = 0;
      char **names = dir_names(dst, &n, false);
      EXPECT(n == 0);
      g_free(names);
      if (!strcmp(types[t], "unreadable"))
        chmod(src, 0700);
      proc_clear(&r);
      g_free(src);
      g_free(dst);
      rm_rf(root);
      g_free(root);
    }
  }
  end_case();
}

static void test_later_failure(void) {
  begin("file ordering preserves completed outputs when a later directory or dangling link fails");
  int passes = legacy_enabled() ? 2 : 1;
  const char *types[] = {"directory", "dangling"};
  for (int old = 0; old < passes; old++) {
    for (size_t t = 0; t < 2; t++) {
      char *root = temp_root();
      char *src = g_build_filename(root, "src", NULL);
      char *dst = g_build_filename(root, "dst", NULL);
      g_mkdir_with_parents(src, 0700);
      char *z = g_build_filename(src, "z.md", NULL);
      char *a = g_build_filename(src, "a.md", NULL);
      char *m = g_build_filename(src, "m.md", NULL);
      EXPECT(write_all(z, "last", 4, 0644));
      EXPECT(write_all(a, "first", 5, 0644));
      if (!strcmp(types[t], "directory"))
        g_mkdir_with_parents(m, 0700);
      else
        EXPECT(symlink("absent", m) == 0);
      Proc r = adapt_run(src, dst, "command", old, NULL);
      expect_status(&r, 1);
      size_t n = 0;
      char **names = dir_names(dst, &n, false);
      EXPECT(names && n == 1 && !strcmp(names[0], "a.md"));
      char *out = g_build_filename(dst, "a.md", NULL);
      unsigned char *data = NULL;
      size_t len = 0;
      EXPECT(read_all(out, &data, &len));
      EXPECT(bytes_eq(data, len, "---\n---\nfirst", strlen("---\n---\nfirst")));
      g_free(data);
      for (size_t i = 0; i < n; i++)
        g_free(names[i]);
      g_free(names);
      proc_clear(&r);
      g_free(out);
      g_free(z);
      g_free(a);
      g_free(m);
      g_free(src);
      g_free(dst);
      rm_rf(root);
      g_free(root);
    }
  }
  end_case();
}

static void test_filename_order(void) {
  begin("filename ordering decodes valid UTF-8 and uses surrogateescape for raw bytes");
  TreeItem *expected = NULL;
  size_t expected_n = 0;
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_root();
    char *src = g_build_filename(root, "src", NULL);
    char *dst = g_build_filename(root, "dst", NULL);
    g_mkdir_with_parents(src, 0700);
    size_t sl = strlen(src);
    char *raw = g_malloc(sl + 1 + 1 + 3 + 1);
    memcpy(raw, src, sl);
    raw[sl] = '/';
    raw[sl + 1] = (char)0xff;
    memcpy(raw + sl + 2, ".md", 4);
    EXPECT(write_all(raw, "raw first", 9, 0644));
    char *u = g_build_filename(src, "\uE000.md", NULL);
    g_mkdir_with_parents(u, 0700);
    char *smile = g_build_filename(src, "🙂.md", NULL);
    EXPECT(write_all(smile, "must stay absent", 16, 0644));
    Proc r = adapt_run(src, dst, "agent", old, NULL);
    expect_status(&r, 1);
    TreeItem *files = NULL;
    size_t n = 0;
    EXPECT(tree_of(dst, &files, &n));
    EXPECT(n == 1);
    if (n == 1)
      EXPECT(!strcmp(files[0].name_hex, "ff2e6d64"));
    if (old)
      EXPECT(trees_eq(files, n, expected, expected_n));
    else {
      free_tree(expected, expected_n);
      expected = files;
      expected_n = n;
      files = NULL;
    }
    free_tree(files, files ? n : 0);
    proc_clear(&r);
    g_free(raw);
    g_free(u);
    g_free(smile);
    g_free(src);
    g_free(dst);
    rm_rf(root);
    g_free(root);
  }
  free_tree(expected, expected_n);
  end_case();
}

static void test_links(void) {
  begin("source and destination links are followed; direct writes preserve inode and mode");
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_root();
    char *src = g_build_filename(root, "src", NULL);
    char *dst = g_build_filename(root, "dst", NULL);
    g_mkdir_with_parents(src, 0700);
    g_mkdir_with_parents(dst, 0700);
    char *input = g_build_filename(root, "input", NULL);
    char *output = g_build_filename(root, "output", NULL);
    EXPECT(write_all(input, "Body", 4, 0644));
    char *src_link = g_build_filename(src, "a.md", NULL);
    EXPECT(symlink("../input", src_link) == 0);
    EXPECT(write_all(output, "old", 3, 0644));
    EXPECT(chmod(output, 0640) == 0);
    struct stat st;
    EXPECT(stat(output, &st) == 0);
    ino_t ino = st.st_ino;
    char *dst_link = g_build_filename(dst, "a.md", NULL);
    EXPECT(symlink("../output", dst_link) == 0);
    char *source_link = g_build_filename(root, "source-link", NULL);
    char *dest_link = g_build_filename(root, "destination-link", NULL);
    EXPECT(symlink("src", source_link) == 0);
    EXPECT(symlink("dst", dest_link) == 0);
    Proc r = adapt_run(source_link, dest_link, "command", old, NULL);
    expect_status(&r, 0);
    struct stat lst;
    EXPECT(lstat(dst_link, &lst) == 0 && S_ISLNK(lst.st_mode));
    EXPECT(stat(output, &st) == 0);
    EXPECT(st.st_ino == ino);
    EXPECT((st.st_mode & 0777) == 0640);
    unsigned char *data = NULL;
    size_t n = 0;
    EXPECT(read_all(output, &data, &n));
    EXPECT(bytes_eq(data, n, "---\n---\nBody", strlen("---\n---\nBody")));
    g_free(data);
    proc_clear(&r);
    g_free(src);
    g_free(dst);
    g_free(input);
    g_free(output);
    g_free(src_link);
    g_free(dst_link);
    g_free(source_link);
    g_free(dest_link);
    rm_rf(root);
    g_free(root);
  }
  end_case();
}

static void test_overlay(void) {
  begin("repeated source overlays and in-place conversions preserve current mutation boundaries");
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_root();
    char *src = g_build_filename(root, "src", NULL);
    char *other = g_build_filename(root, "other", NULL);
    char *dst = g_build_filename(root, "dst", NULL);
    g_mkdir_with_parents(src, 0700);
    g_mkdir_with_parents(other, 0700);
    char *same = g_build_filename(src, "same.md", NULL);
    char *unique = g_build_filename(src, "unique.md", NULL);
    char *other_same = g_build_filename(other, "same.md", NULL);
    EXPECT(write_all(same, "first", 5, 0644));
    EXPECT(write_all(unique, "unique", 6, 0644));
    EXPECT(write_all(other_same, "second $@", 9, 0644));
    Proc a = adapt_run(src, dst, "command", old, NULL);
    expect_status(&a, 0);
    Proc b = adapt_run(other, dst, "command", old, NULL);
    expect_status(&b, 0);
    char *out_same = g_build_filename(dst, "same.md", NULL);
    char *out_unique = g_build_filename(dst, "unique.md", NULL);
    unsigned char *data = NULL;
    size_t n = 0;
    EXPECT(read_all(out_same, &data, &n));
    EXPECT(bytes_eq(data, n, "---\n---\nsecond $ARGUMENTS",
                    strlen("---\n---\nsecond $ARGUMENTS")));
    g_free(data);
    data = NULL;
    EXPECT(read_all(out_unique, &data, &n));
    EXPECT(bytes_eq(data, n, "---\n---\nunique", strlen("---\n---\nunique")));
    g_free(data);
    Proc c = adapt_run(src, src, "command", old, NULL);
    expect_status(&c, 0);
    data = NULL;
    EXPECT(read_all(same, &data, &n));
    EXPECT(bytes_eq(data, n, "---\n---\nfirst", strlen("---\n---\nfirst")));
    g_free(data);
    proc_clear(&a);
    proc_clear(&b);
    proc_clear(&c);
    g_free(same);
    g_free(unique);
    g_free(other_same);
    g_free(out_same);
    g_free(out_unique);
    g_free(src);
    g_free(other);
    g_free(dst);
    rm_rf(root);
    g_free(root);
  }
  end_case();
}

static void test_pathlib(void) {
  begin("destination creation, lexical dot/dot-dot handling and regular-file conflicts preserve Pathlib behavior");
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_root();
    char *src = g_build_filename(root, "src", NULL);
    g_mkdir_with_parents(src, 0700);
    char *a = g_build_filename(src, "a.md", NULL);
    EXPECT(write_all(a, "Body", 4, 0644));
    char *dest = g_strconcat(root, "/missing/../dst//./", NULL);
    Proc r = adapt_run(src, dest, "agent", old, NULL);
    expect_status(&r, 0);
    char *missing = g_build_filename(root, "missing", NULL);
    EXPECT(g_file_test(missing, G_FILE_TEST_IS_DIR));
    char *file = g_build_filename(root, "file", NULL);
    EXPECT(write_all(file, "keep", 4, 0644));
    Proc bad = adapt_run(src, file, "agent", old, NULL);
    expect_status(&bad, 1);
    unsigned char *data = NULL;
    size_t n = 0;
    EXPECT(read_all(file, &data, &n));
    EXPECT(bytes_eq(data, n, "keep", 4));
    g_free(data);
    proc_clear(&r);
    proc_clear(&bad);
    g_free(dest);
    g_free(missing);
    g_free(file);
    g_free(a);
    g_free(src);
    rm_rf(root);
    g_free(root);
  }
  end_case();
}

static void test_io_faults(void) {
  begin("failed open/write/close stops the tree without undoing earlier files");
  const char *modes[] = {"open", "write", "close"};
  const char *left[] = {"old", "---", "---\n---\nBody"};
  for (size_t m = 0; m < 3; m++) {
    char *root = temp_root();
    char *src = g_build_filename(root, "src", NULL);
    char *dst = g_build_filename(root, "dst", NULL);
    g_mkdir_with_parents(src, 0700);
    g_mkdir_with_parents(dst, 0700);
    const char *names[] = {"a.md", "b.md", "c.md"};
    for (size_t i = 0; i < 3; i++) {
      char *p = g_build_filename(src, names[i], NULL);
      EXPECT(write_all(p, "Body", 4, 0644));
      g_free(p);
    }
    char *b = g_build_filename(dst, "b.md", NULL);
    EXPECT(write_all(b, "old", 3, 0644));
    char *fail_path = g_strconcat("ADAPT_FAILURE_PATH=", b, NULL);
    char *fail_mode = g_strconcat("ADAPT_FAIL=", modes[m], NULL);
    const char *env[] = {fail_path, fail_mode, NULL};
    Proc r = adapt_run(src, dst, "command", false, env);
    expect_status(&r, 1);
    char *a = g_build_filename(dst, "a.md", NULL);
    char *c = g_build_filename(dst, "c.md", NULL);
    unsigned char *data = NULL;
    size_t n = 0;
    EXPECT(read_all(a, &data, &n));
    EXPECT(bytes_eq(data, n, "---\n---\nBody", strlen("---\n---\nBody")));
    g_free(data);
    EXPECT(!g_file_test(c, G_FILE_TEST_EXISTS));
    data = NULL;
    EXPECT(read_all(b, &data, &n));
    EXPECT(bytes_eq(data, n, left[m], strlen(left[m])));
    g_free(data);
    proc_clear(&r);
    g_free(fail_path);
    g_free(fail_mode);
    g_free(a);
    g_free(b);
    g_free(c);
    g_free(src);
    g_free(dst);
    rm_rf(root);
    g_free(root);
  }
  end_case();
}

static void test_scan_fault(void) {
  begin("failed directory enumeration discards partial names rather than publishing a partial catalogue");
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_root();
    char *src = g_build_filename(root, "src", NULL);
    char *dst = g_build_filename(root, "dst", NULL);
    g_mkdir_with_parents(src, 0700);
    const char *names[] = {"a.md", "b.md", "c.md"};
    for (size_t i = 0; i < 3; i++) {
      char *p = g_build_filename(src, names[i], NULL);
      EXPECT(write_all(p, "Body", 4, 0644));
      g_free(p);
    }
    char *fail_path = g_strconcat("ADAPT_FAILURE_PATH=", src, NULL);
    const char *env[] = {fail_path, "ADAPT_FAIL=scan", NULL};
    Proc r = adapt_run(src, dst, "agent", old, env);
    expect_status(&r, 0);
    size_t n = 0;
    char **got = dir_names(dst, &n, false);
    EXPECT(n == 0);
    g_free(got);
    proc_clear(&r);
    g_free(fail_path);
    g_free(src);
    g_free(dst);
    rm_rf(root);
    g_free(root);
  }
  end_case();
}

static Proc cli_run(const char *cwd, bool old, char *const *args) {
  const char *python = getenv("OPENCODE_ADAPT_PYTHON");
  if (!python || !*python)
    python = "python3";
  const char *legacy = getenv("OPENCODE_ADAPT_LEGACY");
  char *argv[16];
  int n = 0;
  const char *cmd;
  if (old) {
    cmd = python;
    argv[n++] = (char *)python;
    argv[n++] = (char *)legacy;
  } else {
    cmd = bin_path;
    argv[n++] = bin_path;
  }
  for (size_t i = 0; args[i] && n < 14; i++)
    argv[n++] = args[i];
  argv[n] = NULL;
  return proc_run(cmd, argv, NULL, 0, NULL, cwd, 10000);
}

static void test_cli(void) {
  begin("CLI choices, required kind, abbreviations, equals forms and help retain argparse semantics");
  int passes = legacy_enabled() ? 2 : 1;
  for (int old = 0; old < passes; old++) {
    char *root = temp_root();
    char *src = g_build_filename(root, "src", NULL);
    g_mkdir_with_parents(src, 0700);
    char *bad_sets[][6] = {
        {NULL},
        {"src", "dst", NULL},
        {"--kind", "bad", "src", "dst", NULL},
        {"--kind=agent", "src", NULL},
        {"--kind=agent", "src", "dst", "extra", NULL},
        {"--kind", "--help", NULL},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bad_sets); i++) {
      Proc r = cli_run(root, old, bad_sets[i]);
      if (r.spawn_error)
        FAIL("spawn failed");
      if (r.status != 2)
        FAIL("cli status %d != 2", r.status);
      proc_clear(&r);
    }
    char *help_sets[][4] = {{"-h", NULL}, {"--help", NULL}, {"--h", NULL},
                            {"-hh", NULL}, {"--unknown", "-h", NULL}};
    for (size_t i = 0; i < G_N_ELEMENTS(help_sets); i++) {
      Proc r = cli_run(root, old, help_sets[i]);
      if (r.spawn_error)
        FAIL("spawn failed");
      if (r.status != 0)
        FAIL("help status %d != 0 (%s)", r.status, r.err);
      proc_clear(&r);
    }
    char *ok_sets[][8] = {{"--k=agent", "src", "dst", NULL},
                          {"src", "--kind", "command", "dst", NULL},
                          {"--kind", "agent", "--kind", "command", "src", "dst", NULL}};
    for (size_t i = 0; i < G_N_ELEMENTS(ok_sets); i++) {
      Proc r = cli_run(root, old, ok_sets[i]);
      if (r.spawn_error)
        FAIL("spawn failed");
      if (r.status != 0)
        FAIL("accepted cli status %d (%s)", r.status, r.err);
      proc_clear(&r);
    }
    char *both[] = {"--kind", "bad", "--kind", "agent", "src", "dst", NULL};
    Proc r = cli_run(root, old, both);
    if (r.spawn_error)
      FAIL("spawn failed");
    if (r.status != 2)
      FAIL("invalid then valid kind status %d", r.status);
    proc_clear(&r);
    g_free(src);
    rm_rf(root);
    g_free(root);
  }
  end_case();
}

static char *json_field(const char *json, const char *key) {
  char pat[128];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p)
    return NULL;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  if (*p++ != ':')
    return NULL;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  if (*p++ != '"')
    return NULL;
  GString *out = g_string_new(NULL);
  while (*p && *p != '"') {
    if (*p == '\\') {
      p++;
      if (!*p)
        break;
      if (*p == 'n')
        g_string_append_c(out, '\n');
      else if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
        unsigned v = 0;
        for (int i = 1; i <= 4; i++) {
          char c = p[i];
          v <<= 4;
          if (c >= '0' && c <= '9')
            v += (unsigned)(c - '0');
          else if (c >= 'a' && c <= 'f')
            v += (unsigned)(c - 'a' + 10);
          else if (c >= 'A' && c <= 'F')
            v += (unsigned)(c - 'A' + 10);
        }
        char u[5];
        size_t n = 0;
        utf8(v, u, &n);
        g_string_append_len(out, u, (gssize)n);
        p += 4;
      } else
        g_string_append_c(out, *p);
    } else
      g_string_append_c(out, *p);
    p++;
  }
  return g_string_free(out, false);
}

static void test_bundles(void) {
  begin("all repository agents and local/upstream prompts retain byte-identical generated bundles");
  const char *legacy = getenv("OPENCODE_ADAPT_LEGACY");
  const char *refs = getenv("OPENCODE_ADAPT_REFERENCE_BUNDLES");
  const char *pi = getenv("OPENCODE_ADAPT_PI_ROOT");
  const char *upstream = getenv("OPENCODE_ADAPT_UPSTREAM");
  if ((!legacy && !refs) || !pi || !upstream) {
    printf("SKIP %s\n", current_test);
    return;
  }
  char *ref_json = NULL;
  if (!legacy) {
    gsize n = 0;
    if (!g_file_get_contents(refs, &ref_json, &n, NULL)) {
      FAIL("could not read reference bundles");
      return;
    }
  }
  const char *kinds[] = {"agent", "command"};
  for (size_t k = 0; k < 2; k++) {
    char *root = temp_root();
    char *native = g_build_filename(root, "native", NULL);
    char *legacy_dir = g_build_filename(root, "legacy", NULL);
    const char *sources[2];
    size_t ns = 0;
    if (!strcmp(kinds[k], "agent"))
      sources[ns++] = g_build_filename(pi, "agents", NULL);
    else {
      sources[ns++] = g_build_filename(pi, "prompts", NULL);
      sources[ns++] = g_strdup(upstream);
    }
    for (size_t s = 0; s < ns; s++) {
      Proc r = adapt_run(sources[s], native, kinds[k], false, NULL);
      expect_status(&r, 0);
      proc_clear(&r);
      if (legacy) {
        Proc old = adapt_run(sources[s], legacy_dir, kinds[k], true, NULL);
        expect_status(&old, 0);
        proc_clear(&old);
      }
    }
    char *reference = NULL;
    if (legacy)
      reference = g_strdup(legacy_dir);
    else
      reference = json_field(ref_json, !strcmp(kinds[k], "agent") ? "agents" : "commands");
    TreeItem *a = NULL, *b = NULL;
    size_t an = 0, bn = 0;
    EXPECT(reference && tree_of(native, &a, &an) && tree_of(reference, &b, &bn));
    EXPECT(trees_eq(a, an, b, bn));
    size_t names = 0;
    char **list = dir_names(native, &names, false);
    EXPECT(list && names > 0);
    for (size_t i = 0; i < names; i++)
      g_free(list[i]);
    g_free(list);
    free_tree(a, an);
    free_tree(b, bn);
    g_free(reference);
    for (size_t s = 0; s < ns; s++)
      g_free((char *)sources[s]);
    g_free(native);
    g_free(legacy_dir);
    rm_rf(root);
    g_free(root);
  }
  g_free(ref_json);
  end_case();
}

int main(void) {
  umask(022);
  bin_path = sibling("OPENCODE_ADAPT_BIN", "opencode-adapt-markdown");
  failures_path = sibling("OPENCODE_ADAPT_FAILURES", "opencode-adapt-failures.so");
  if (!bin_path || !failures_path || access(bin_path, X_OK) || access(failures_path, R_OK)) {
    fprintf(stderr, "Set OPENCODE_ADAPT_BIN and OPENCODE_ADAPT_FAILURES\n");
    return 1;
  }
  test_agent_allowlist();
  test_command_whitelist();
  test_modes();
  test_frontmatter_anchors();
  test_crlf();
  test_splitlines();
  test_indent_strip();
  test_skills_split();
  test_skill_dedup();
  test_nul_astral();
  test_invalid_utf8();
  test_glob();
  test_empty_globs();
  test_later_failure();
  test_filename_order();
  test_links();
  test_overlay();
  test_pathlib();
  test_io_faults();
  test_scan_fault();
  test_cli();
  test_bundles();
  g_free(bin_path);
  g_free(failures_path);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("ok");
  return 0;
}
