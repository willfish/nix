#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yyjson.h>

static int failures;
static const char *test_name;
static GPtrArray *temps;

static void fail(int line, const char *msg) {
  fprintf(stderr, "FAIL %s:%d: %s\n", test_name, line, msg);
  failures++;
}
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fail(__LINE__, #cond);                                                   \
      return;                                                                  \
    }                                                                          \
  } while (0)

static void cleanup_temps(void) {
  if (!temps)
    return;
  for (guint i = 0; i < temps->len; i++) {
    char *argv[] = {"rm", "-rf", "--", temps->pdata[i], NULL};
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                 NULL, NULL);
  }
}
static char *temp_dir(const char *prefix) {
  if (!temps) {
    temps = g_ptr_array_new();
    atexit(cleanup_temps);
  }
  char *tmpl = g_strdup_printf("%s/%sXXXXXX", g_get_tmp_dir(), prefix);
  if (!g_mkdtemp(tmpl)) {
    g_free(tmpl);
    return NULL;
  }
  g_ptr_array_add(temps, tmpl);
  return tmpl;
}
static char *self_exe(void) {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0)
    return g_strdup("repo-checks");
  buf[n] = 0;
  return g_strdup(buf);
}
static char *sibling(const char *name) {
  const char *env = NULL;
  if (!strcmp(name, "check-flake-lock-update"))
    env = getenv("LOCK_POLICY_BIN");
  else if (!strcmp(name, "import-omarchy-community"))
    env = getenv("COMMUNITY_IMPORT_BIN");
  else if (!strcmp(name, "audit-skills"))
    env = getenv("SKILL_AUDIT_BIN");
  else if (!strcmp(name, "nix-storage-report"))
    env = getenv("STORAGE_REPORT_BIN");
  if (env && *env)
    return g_strdup(env);
  char *exe = self_exe(), *dir = g_path_get_dirname(exe);
  char *bin = g_build_filename(dir, name, NULL);
  g_free(dir);
  g_free(exe);
  return bin;
}
static char *repo_root(void) {
  char *dir = g_strdup(FIXTURE_SOURCE);
  for (int i = 0; i < 8; i++) {
    char *flake = g_build_filename(dir, "flake.nix", NULL);
    char *script = g_build_filename(dir, "scripts/nix-storage-costs", NULL);
    bool ok = g_file_test(flake, G_FILE_TEST_IS_REGULAR) &&
              g_file_test(script, G_FILE_TEST_IS_REGULAR);
    g_free(flake);
    g_free(script);
    if (ok)
      return dir;
    char *parent = g_path_get_dirname(dir);
    if (!strcmp(parent, dir)) {
      g_free(parent);
      break;
    }
    g_free(dir);
    dir = parent;
  }
  g_free(dir);
  return NULL;
}
static bool write_file(const char *path, const void *data, size_t len, mode_t mode) {
  char *dir = g_path_get_dirname(path);
  int mk = g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  if (mk != 0)
    return false;
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
  bool ok = fchmod(fd, mode) == 0;
  if (close(fd) != 0)
    ok = false;
  return ok;
}
static char *read_file(const char *path, size_t *len) {
  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &contents, &length, NULL))
    return NULL;
  if (len)
    *len = length;
  return contents;
}
static bool file_eq(const char *path, const void *data, size_t len) {
  size_t n = 0;
  char *got = read_file(path, &n);
  if (!got)
    return false;
  bool ok = n == len && memcmp(got, data, len) == 0;
  g_free(got);
  return ok;
}
static bool mode_is(const char *path, mode_t mode) {
  struct stat st;
  return stat(path, &st) == 0 && (st.st_mode & 0777) == mode;
}
static ino_t inode_of(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? st.st_ino : 0;
}
typedef struct {
  int code, signal;
  char *out, *err;
  size_t out_len, err_len;
  bool failed;
} Proc;
static void proc_free(Proc *p) {
  g_free(p->out);
  g_free(p->err);
  p->out = p->err = NULL;
}
static Proc proc_run(const char *file, char *const *argv, const char *cwd,
                     char **envp, int timeout_ms) {
  Proc p = {0};
  p.code = -1;
  p.failed = true;
  int outp[2], errp[2];
  if (pipe(outp) || pipe(errp))
    return p;
  pid_t pid = fork();
  if (pid < 0) {
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    return p;
  }
  if (pid == 0) {
    setpgid(0, 0);
    dup2(outp[1], STDOUT_FILENO);
    dup2(errp[1], STDERR_FILENO);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    for (int fd = 3; fd < 64; fd++)
      close(fd);
    if (cwd && chdir(cwd) != 0)
      _exit(127);
    execvpe(file, argv, envp ? envp : environ);
    _exit(127);
  }
  setpgid(pid, pid);
  close(outp[1]);
  close(errp[1]);
  GString *out = g_string_new(NULL), *err = g_string_new(NULL);
  int open_out = outp[0], open_err = errp[0];
  bool timed_out = false;
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  while (open_out >= 0 || open_err >= 0) {
    struct pollfd fds[2];
    int nfd = 0, out_i = -1, err_i = -1;
    if (open_out >= 0) {
      out_i = nfd;
      fds[nfd].fd = open_out;
      fds[nfd].events = POLLIN;
      nfd++;
    }
    if (open_err >= 0) {
      err_i = nfd;
      fds[nfd].fd = open_err;
      fds[nfd].events = POLLIN;
      nfd++;
    }
    int remain = (int)((deadline - g_get_monotonic_time()) / 1000);
    if (remain < 0)
      remain = 0;
    int pr = poll(fds, (nfds_t)nfd, remain);
    if (pr <= 0) {
      if (pr < 0 && errno == EINTR)
        continue;
      timed_out = true;
      kill(-pid, SIGKILL);
      break;
    }
    char buf[4096];
    if (out_i >= 0 && (fds[out_i].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = read(open_out, buf, sizeof buf);
      if (n > 0)
        g_string_append_len(out, buf, (gssize)n);
      else if (n == 0 || (n < 0 && errno != EINTR)) {
        close(open_out);
        open_out = -1;
      }
    }
    if (err_i >= 0 && (fds[err_i].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t n = read(open_err, buf, sizeof buf);
      if (n > 0)
        g_string_append_len(err, buf, (gssize)n);
      else if (n == 0 || (n < 0 && errno != EINTR)) {
        close(open_err);
        open_err = -1;
      }
    }
  }
  if (open_out >= 0)
    close(open_out);
  if (open_err >= 0)
    close(open_err);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  p.out_len = out->len;
  p.err_len = err->len;
  p.out = g_string_free(out, FALSE);
  p.err = g_string_free(err, FALSE);
  if (!timed_out) {
    p.failed = false;
    if (WIFEXITED(status))
      p.code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) {
      p.signal = WTERMSIG(status);
      p.code = -1;
    }
  }
  return p;
}
G_GNUC_UNUSED static char **env_blank(void) { return g_new0(char *, 1); }
static char **env_copy(char **src) {
  size_t n = 0;
  if (src)
    while (src[n])
      n++;
  char **out = g_new0(char *, n + 1);
  for (size_t i = 0; i < n; i++)
    out[i] = g_strdup(src[i]);
  return out;
}
static void env_set(char ***envp, const char *key, const char *value) {
  size_t klen = strlen(key), vlen = value ? strlen(value) : 0;
  char *entry = g_malloc(klen + vlen + 2);
  memcpy(entry, key, klen);
  entry[klen] = '=';
  if (vlen)
    memcpy(entry + klen + 1, value, vlen);
  entry[klen + 1 + vlen] = 0;
  char **env = *envp;
  size_t n = 0;
  if (env)
    for (; env[n]; n++)
      if (!strncmp(env[n], key, klen) && env[n][klen] == '=') {
        g_free(env[n]);
        env[n] = entry;
        return;
      }
  char **next = g_realloc(env, (n + 2) * sizeof *next);
  next[n] = entry;
  next[n + 1] = NULL;
  *envp = next;
}
static void env_free(char **env) {
  if (!env)
    return;
  for (size_t i = 0; env[i]; i++)
    g_free(env[i]);
  g_free(env);
}
static bool val_streq(yyjson_val *v, const char *s) {
  if (!yyjson_is_str(v) || !s)
    return false;
  size_t n = yyjson_get_len(v), m = strlen(s);
  return n == m && memcmp(yyjson_get_str(v), s, n) == 0;
}
static bool json_equal(yyjson_val *a, yyjson_val *b) {
  if (!a || !b)
    return a == b;
  if (yyjson_is_null(a) || yyjson_is_null(b))
    return yyjson_is_null(a) && yyjson_is_null(b);
  if (yyjson_is_bool(a) || yyjson_is_bool(b))
    return yyjson_is_bool(a) && yyjson_is_bool(b) &&
           yyjson_get_bool(a) == yyjson_get_bool(b);
  if (yyjson_is_num(a) || yyjson_is_num(b)) {
    if (!yyjson_is_num(a) || !yyjson_is_num(b))
      return false;
    if (yyjson_is_int(a) && yyjson_is_int(b))
      return yyjson_get_sint(a) == yyjson_get_sint(b);
    return yyjson_get_num(a) == yyjson_get_num(b);
  }
  if (yyjson_is_str(a) || yyjson_is_str(b)) {
    if (!yyjson_is_str(a) || !yyjson_is_str(b))
      return false;
    size_t n = yyjson_get_len(a);
    return n == yyjson_get_len(b) &&
           memcmp(yyjson_get_str(a), yyjson_get_str(b), n) == 0;
  }
  if (yyjson_is_arr(a) || yyjson_is_arr(b)) {
    if (!yyjson_is_arr(a) || !yyjson_is_arr(b) ||
        yyjson_arr_size(a) != yyjson_arr_size(b))
      return false;
    for (size_t i = 0; i < yyjson_arr_size(a); i++)
      if (!json_equal(yyjson_arr_get(a, i), yyjson_arr_get(b, i)))
        return false;
    return true;
  }
  if (!yyjson_is_obj(a) || !yyjson_is_obj(b) ||
      yyjson_obj_size(a) != yyjson_obj_size(b))
    return false;
  size_t idx, max;
  yyjson_val *key, *val;
  yyjson_obj_foreach(a, idx, max, key, val) {
    yyjson_val *other = yyjson_obj_get(b, yyjson_get_str(key));
    if (!other || !json_equal(val, other))
      return false;
  }
  return true;
}
static bool runtime_ok(const char *err) {
  if (!err)
    return true;
  return !strstr(err, "AddressSanitizer") && !strstr(err, "LeakSanitizer") &&
         !strstr(err, "runtime error:") && !strstr(err, "DEADLYSIGNAL");
}
static bool expect_code(Proc *p, int code) {
  if (p->failed || !runtime_ok(p->err) || p->code != code) {
    fprintf(stderr, "expected %d got %d signal %d failed %d\nstderr: %s\nstdout: %s\n",
            code, p->code, p->signal, p->failed, p->err ? p->err : "",
            p->out ? p->out : "");
    return false;
  }
  return true;
}
static char *repeat_char(char c, int n) {
  char *s = g_malloc((size_t)n + 1);
  memset(s, c, (size_t)n);
  s[n] = 0;
  return s;
}
static char *replace_first(const char *text, const char *from, const char *to) {
  const char *found = strstr(text, from);
  if (!found)
    return NULL;
  size_t before = (size_t)(found - text), fl = strlen(from), tl = strlen(to),
         rest = strlen(found + fl);
  char *out = g_malloc(before + tl + rest + 1);
  memcpy(out, text, before);
  memcpy(out + before, to, tl);
  memcpy(out + before + tl, found + fl, rest + 1);
  return out;
}
static char *doc_text(yyjson_mut_doc *doc) { return yyjson_mut_write(doc, 0, NULL); }
static yyjson_mut_doc *clone_doc(yyjson_mut_doc *doc) {
  size_t len = 0;
  yyjson_write_err werr = {0};
  char *text = doc ? yyjson_mut_write_opts(doc, 0, NULL, &len, &werr) : NULL;
  if (!text) {
    fprintf(stderr, "clone write failed %u %s\n", werr.code, werr.msg ? werr.msg : "");
    return NULL;
  }
  yyjson_doc *imm = yyjson_read(text, len, 0);
  yyjson_mut_doc *copy = imm ? yyjson_doc_mut_copy(imm, NULL) : NULL;
  if (!copy)
    fprintf(stderr, "clone read failed: %s\n", text);
  free(text);
  yyjson_doc_free(imm);
  return copy;
}
static yyjson_mut_val *github_node(yyjson_mut_doc *doc, const char *owner,
                                   const char *repo) {
  char *rev = repeat_char('a', 40), *body = repeat_char('A', 43);
  char *hash = g_strconcat("sha256-", body, "=", NULL);
  yyjson_mut_val *node = yyjson_mut_obj(doc);
  yyjson_mut_val *locked = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, locked, "type", "github");
  yyjson_mut_obj_add_strcpy(doc, locked, "owner", owner);
  yyjson_mut_obj_add_strcpy(doc, locked, "repo", repo);
  yyjson_mut_obj_add_strcpy(doc, locked, "rev", rev);
  yyjson_mut_obj_add_strcpy(doc, locked, "narHash", hash);
  yyjson_mut_obj_add_int(doc, locked, "lastModified", 1);
  yyjson_mut_val *original = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, original, "type", "github");
  yyjson_mut_obj_add_strcpy(doc, original, "owner", owner);
  yyjson_mut_obj_add_strcpy(doc, original, "repo", repo);
  yyjson_mut_obj_add_val(doc, node, "locked", locked);
  yyjson_mut_obj_add_val(doc, node, "original", original);
  g_free(rev);
  g_free(body);
  g_free(hash);
  return node;
}
static yyjson_mut_doc *base_doc(void) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_int(doc, root, "version", 7);
  yyjson_mut_obj_add_strcpy(doc, root, "root", "root");
  yyjson_mut_val *nodes = yyjson_mut_obj(doc);
  yyjson_mut_val *root_node = yyjson_mut_obj(doc);
  yyjson_mut_val *inputs = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, inputs, "app", "app");
  yyjson_mut_obj_add_val(doc, root_node, "inputs", inputs);
  yyjson_mut_obj_add_val(doc, nodes, "root", root_node);
  yyjson_mut_val *app = github_node(doc, "willfish", "fixture");
  yyjson_mut_val *app_inputs = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, app_inputs, "dep", "dep");
  yyjson_mut_obj_add_val(doc, app, "inputs", app_inputs);
  yyjson_mut_obj_add_val(doc, nodes, "app", app);
  yyjson_mut_obj_add_val(doc, nodes, "dep", github_node(doc, "NixOS", "nixpkgs"));
  yyjson_mut_obj_add_val(doc, root, "nodes", nodes);
  return doc;
}
static yyjson_mut_val *nodes_of(yyjson_mut_doc *doc) {
  return yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc), "nodes");
}
static yyjson_mut_val *node_of(yyjson_mut_doc *doc, const char *name) {
  return yyjson_mut_obj_get(nodes_of(doc), name);
}
static void set_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                    const char *value) {
  yyjson_mut_obj_remove_key(obj, key);
  yyjson_mut_obj_add_strcpy(doc, obj, key, value);
}
static void set_raw(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                    const char *json) {
  yyjson_doc *imm = json ? yyjson_read(json, strlen(json), 0) : NULL;
  if (!imm || !doc || !obj) {
    fprintf(stderr, "set_raw failed key=%s json=%s\n", key ? key : "", json ? json : "");
    yyjson_doc_free(imm);
    return;
  }
  yyjson_mut_val *val = yyjson_val_mut_copy(doc, yyjson_doc_get_root(imm));
  yyjson_mut_obj_remove_key(obj, key);
  yyjson_mut_obj_add_val(doc, obj, key, val);
  yyjson_doc_free(imm);
}
typedef struct {
  bool ok;
  char *out;
} LockResult;
static LockResult lock_check(const char *before, const char *after, int status,
                             const char *owners) {
  LockResult result = {0};
  if (!owners)
    owners = "willfish\n";
  char *root = temp_dir("lock-policy-");
  if (!root)
    return result;
  char *base = g_build_filename(root, "base", NULL);
  char *head = g_build_filename(root, "head", NULL);
  if (!write_file(base, before, strlen(before), 0600) ||
      !write_file(head, after, strlen(after), 0600)) {
    g_free(base);
    g_free(head);
    return result;
  }
  char *bin = sibling("check-flake-lock-update");
  const char *legacy = getenv("LOCK_POLICY_LEGACY");
  const char *python = getenv("LOCK_POLICY_PYTHON");
  if (!python || !*python)
    python = "python3";
  int rounds = legacy && *legacy ? 2 : 1;
  char *saved = NULL;
  bool ok = true;
  for (int old = 0; old < rounds && ok; old++) {
    char **env = env_copy(environ);
    env_set(&env, "AUTO_MERGE_GITHUB_OWNERS", owners);
    char *argv_new[] = {bin, base, head, NULL};
    char *argv_old[] = {(char *)python, (char *)legacy, base, head, NULL};
    Proc p = proc_run(old ? python : bin, old ? argv_old : argv_new, NULL, env, 10000);
    env_free(env);
    if (!expect_code(&p, status) || (status && (!p.err || !strstr(p.err, "Manual review required:"))))
      ok = false;
    else if (!status && old) {
      if (!saved || !p.out || strcmp(saved, p.out))
        ok = false;
    } else if (!status)
      saved = g_strdup(p.out ? p.out : "");
    if (!ok)
      fprintf(stderr, "lock check failed status %d\n%s\n", status, p.err ? p.err : "");
    proc_free(&p);
  }
  result.ok = ok;
  result.out = saved ? saved : g_strdup("");
  if (!ok) {
    g_free(result.out);
    result.out = NULL;
  }
  g_free(bin);
  g_free(base);
  g_free(head);
  return result;
}
static char *text_of(yyjson_mut_doc *doc) {
  char *text = doc_text(doc);
  yyjson_mut_doc_free(doc);
  return text;
}
static void test_lock_accept(void) {
  test_name = "unchanged valid graphs";
  yyjson_mut_doc *a = base_doc();
  char *at = doc_text(a);
  LockResult r = lock_check(at, at, 0, NULL);
  CHECK(r.ok);
  g_free(r.out);
  yyjson_mut_doc *b = clone_doc(a);
  char *rev = repeat_char('b', 40);
  set_str(b, yyjson_mut_obj_get(node_of(b, "app"), "locked"), "rev", rev);
  char *bt = doc_text(b);
  r = lock_check(at, bt, 0, NULL);
  CHECK(r.ok && r.out && strstr(r.out, "app"));
  g_free(r.out);
  g_free(bt);
  yyjson_mut_doc_free(b);
  yyjson_mut_obj_remove_key(nodes_of(a), "dep");
  yyjson_mut_obj_add_val(a, nodes_of(a), "dep", github_node(a, "willfish", "library"));
  g_free(at);
  at = doc_text(a);
  b = clone_doc(a);
  char *revc = repeat_char('c', 40), *bs = repeat_char('B', 42);
  char *hash = g_strconcat("sha256-", bs, "A=", NULL);
  for (int i = 0; i < 2; i++) {
    yyjson_mut_val *locked =
        yyjson_mut_obj_get(node_of(b, i ? "dep" : "app"), "locked");
    set_str(b, locked, "rev", revc);
    set_str(b, locked, "narHash", hash);
    yyjson_mut_obj_remove_key(locked, "lastModified");
    yyjson_mut_obj_add_int(b, locked, "lastModified", 2);
  }
  bt = doc_text(b);
  r = lock_check(at, bt, 0, NULL);
  CHECK(r.ok && r.out && !strcmp(r.out, "Auto-merge allowed for changed lock node(s): app, dep\n"));
  g_free(r.out);
  g_free(at);
  g_free(bt);
  g_free(rev);
  g_free(revc);
  g_free(bs);
  g_free(hash);
  yyjson_mut_doc_free(a);
  yyjson_mut_doc_free(b);
}
static void test_lock_allow(void) {
  test_name = "allowlisting is exact";
  yyjson_mut_doc *a = base_doc();
  yyjson_mut_doc *b = clone_doc(a);
  char *rev = repeat_char('b', 40);
  set_str(b, yyjson_mut_obj_get(node_of(b, "app"), "locked"), "rev", rev);
  char *at = doc_text(a), *bt = doc_text(b);
  const char *owners[] = {"", "Willfish", "willfish-other", " willfish", "willfish ",
                          "willfish\t"};
  for (size_t i = 0; i < 6; i++) {
    LockResult r = lock_check(at, bt, 1, owners[i]);
    CHECK(r.ok);
    g_free(r.out);
  }
  LockResult r = lock_check(at, bt, 0, "other\vwillfish\xC2\x85last");
  CHECK(r.ok);
  g_free(r.out);
  set_str(b, yyjson_mut_obj_get(node_of(b, "dep"), "locked"), "rev", rev);
  g_free(bt);
  bt = doc_text(b);
  r = lock_check(at, bt, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  yyjson_mut_obj_remove_key(yyjson_mut_obj_get(node_of(a, "app"), "inputs"), "dep");
  g_free(at);
  at = doc_text(a);
  yyjson_mut_doc_free(b);
  b = clone_doc(a);
  set_str(b, yyjson_mut_obj_get(node_of(b, "dep"), "locked"), "rev", rev);
  g_free(bt);
  bt = doc_text(b);
  r = lock_check(at, bt, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(at);
  g_free(bt);
  g_free(rev);
  yyjson_mut_doc_free(a);
  yyjson_mut_doc_free(b);
}
static void reject_pair(yyjson_mut_doc *a, yyjson_mut_doc *b) {
  if (!a || !b) {
    fail(__LINE__, "null lock doc");
    yyjson_mut_doc_free(a);
    yyjson_mut_doc_free(b);
    return;
  }
  char *at = doc_text(a), *bt = doc_text(b);
  LockResult r = lock_check(at, bt, 1, NULL);
  if (!r.ok)
    fail(__LINE__, "expected review");
  g_free(r.out);
  g_free(at);
  g_free(bt);
  yyjson_mut_doc_free(a);
  yyjson_mut_doc_free(b);
}
static void test_lock_identity(void) {
  test_name = "root identity cannot change";
  yyjson_mut_doc *a = base_doc(), *b = clone_doc(a);
  yyjson_mut_obj_add_val(b, nodes_of(b), "extra", github_node(b, "willfish", "fixture"));
  reject_pair(a, b);
  if (failures)
    return;
  a = base_doc();
  b = clone_doc(a);
  yyjson_mut_obj_remove_key(nodes_of(b), "dep");
  reject_pair(a, b);
  if (failures)
    return;
  a = base_doc();
  b = clone_doc(a);
  set_str(b, yyjson_mut_obj_get(node_of(b, "root"), "inputs"), "app", "dep");
  reject_pair(a, b);
  if (failures)
    return;
  a = base_doc();
  b = clone_doc(a);
  yyjson_mut_obj_remove_key(node_of(b, "app"), "inputs");
  yyjson_mut_obj_add_val(b, node_of(b, "app"), "inputs", yyjson_mut_obj(b));
  reject_pair(a, b);
  if (failures)
    return;
  a = base_doc();
  b = clone_doc(a);
  yyjson_mut_obj_add_strcpy(b, yyjson_mut_obj_get(node_of(b, "app"), "inputs"), "extra", "dep");
  reject_pair(a, b);
  if (failures)
    return;
  a = base_doc();
  b = clone_doc(a);
  yyjson_mut_obj_add_strcpy(b, yyjson_mut_obj_get(node_of(b, "app"), "original"), "ref", "branch");
  reject_pair(a, b);
  if (failures)
    return;
  a = base_doc();
  b = clone_doc(a);
  yyjson_mut_obj_add_bool(b, node_of(b, "dep"), "flake", false);
  reject_pair(a, b);
  if (failures)
    return;
  a = base_doc();
  b = clone_doc(a);
  yyjson_mut_val *root_node = yyjson_mut_val_mut_copy(b, node_of(b, "root"));
  yyjson_mut_obj_remove_key(nodes_of(b), "root");
  yyjson_mut_obj_add_val(b, nodes_of(b), "n1", root_node);
  set_str(b, yyjson_mut_doc_get_root(b), "root", "n1");
  reject_pair(a, b);
  if (failures)
    return;
  a = base_doc();
  yyjson_mut_obj_add_strcpy(a, yyjson_mut_obj_get(node_of(a, "root"), "inputs"), "alias", "dep");
  yyjson_mut_val *shared = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, shared, "alias");
  yyjson_mut_obj_add_val(a, yyjson_mut_obj_get(node_of(a, "app"), "inputs"), "shared", shared);
  b = clone_doc(a);
  yyjson_mut_val *changed = yyjson_mut_arr(b);
  yyjson_mut_arr_add_strcpy(b, changed, "app");
  yyjson_mut_arr_add_strcpy(b, changed, "dep");
  yyjson_mut_obj_remove_key(yyjson_mut_obj_get(node_of(b, "app"), "inputs"), "shared");
  yyjson_mut_obj_add_val(b, yyjson_mut_obj_get(node_of(b, "app"), "inputs"), "shared", changed);
  reject_pair(a, b);
}
static void test_lock_attrs(void) {
  test_name = "locked identity attributes";
  const char *keys[] = {"owner", "repo", "host", "dir", "ref", "type"};
  const char *values[] = {"other", "other", "other.example", "subdir", "branch", "gitlab"};
  for (size_t i = 0; i < 6; i++) {
    yyjson_mut_doc *a = base_doc(), *b = clone_doc(a);
    set_str(b, yyjson_mut_obj_get(node_of(b, "app"), "locked"), keys[i], values[i]);
    reject_pair(a, b);
    if (failures)
      return;
  }
  const char *extra_keys[] = {"host", "ref", "revCount", "custom"};
  for (size_t i = 0; i < 4; i++) {
    yyjson_mut_doc *a = base_doc();
    yyjson_mut_val *locked = yyjson_mut_obj_get(node_of(a, "app"), "locked");
    if (i == 0)
      yyjson_mut_obj_add_strcpy(a, locked, "host", "other.example");
    else if (i == 1)
      yyjson_mut_obj_add_strcpy(a, locked, "ref", "main");
    else if (i == 2)
      yyjson_mut_obj_add_int(a, locked, "revCount", 2);
    else
      yyjson_mut_obj_add_bool(a, locked, "custom", true);
    yyjson_mut_doc *b = clone_doc(a);
    char *rev = repeat_char('b', 40);
    set_str(b, yyjson_mut_obj_get(node_of(b, "app"), "locked"), "rev", rev);
    g_free(rev);
    (void)extra_keys;
    reject_pair(a, b);
    if (failures)
      return;
  }
  yyjson_mut_doc *a = base_doc();
  set_str(a, yyjson_mut_obj_get(node_of(a, "app"), "locked"), "type", "gitlab");
  yyjson_mut_doc *b = clone_doc(a);
  char *rev = repeat_char('b', 40);
  set_str(b, yyjson_mut_obj_get(node_of(b, "app"), "locked"), "rev", rev);
  g_free(rev);
  reject_pair(a, b);
}
static char *source_lock(const char *type) {
  yyjson_mut_doc *a = base_doc();
  yyjson_mut_obj_remove_key(node_of(a, "app"), "inputs");
  yyjson_mut_obj_add_val(a, node_of(a, "app"), "inputs", yyjson_mut_obj(a));
  yyjson_mut_val *original = yyjson_mut_obj(a);
  yyjson_mut_obj_add_strcpy(a, original, "type", type);
  if (!strcmp(type, "github") || !strcmp(type, "gitlab") || !strcmp(type, "sourcehut")) {
    yyjson_mut_obj_add_strcpy(a, original, "owner", "vendor");
    yyjson_mut_obj_add_strcpy(a, original, "repo", "repo");
  } else if (!strcmp(type, "path"))
    yyjson_mut_obj_add_strcpy(a, original, "path", "/source");
  else if (!strcmp(type, "indirect"))
    yyjson_mut_obj_add_strcpy(a, original, "id", "nixpkgs");
  else
    yyjson_mut_obj_add_strcpy(a, original, "url", "https://example.test/source");
  yyjson_mut_obj_remove_key(node_of(a, "app"), "original");
  yyjson_mut_obj_add_val(a, node_of(a, "app"), "original", original);
  if (strcmp(type, "indirect")) {
    yyjson_mut_val *locked = yyjson_mut_val_mut_copy(a, original);
    char *body = repeat_char('A', 43);
    char *hash = g_strconcat("sha256-", body, "=", NULL);
    yyjson_mut_obj_add_strcpy(a, locked, "narHash", hash);
    if (!strcmp(type, "github")) {
      char *rev = repeat_char('a', 40);
      yyjson_mut_obj_add_strcpy(a, locked, "rev", rev);
      g_free(rev);
    }
    yyjson_mut_obj_remove_key(node_of(a, "app"), "locked");
    yyjson_mut_obj_add_val(a, node_of(a, "app"), "locked", locked);
    g_free(body);
    g_free(hash);
  }
  return text_of(a);
}
static void test_lock_sources(void) {
  test_name = "supported source types";
  const char *types[] = {"github", "gitlab", "sourcehut", "git", "mercurial",
                         "tarball", "file", "path", "indirect"};
  for (size_t i = 0; i < 9; i++) {
    char *text = source_lock(types[i]);
    LockResult r = lock_check(text, text, 0, NULL);
    CHECK(r.ok);
    g_free(r.out);
    g_free(text);
  }
  const char *fields[] = {"type", "type", "narHash", "narHash", "rev", "rev", "owner",
                          "repo", "lastModified", "lastModified", "lastModified",
                          "revCount", "dir", "ref", "custom"};
  const char *values[] = {"\"future\"", "\"indirect\"", "\"sha256-???\"", NULL,
                          NULL, "null", "\"\"", "\"../repo\"", "true", "-1", "1.5",
                          "-1", "true", "1", "{}"};
  char *rev = repeat_char('A', 40);
  char *sha = g_strconcat("\"sha512-", repeat_char('A', 43), "=\"", NULL);
  const char *dyn_rev = NULL;
  char *rev_json = g_strdup_printf("\"%s\"", rev);
  for (size_t i = 0; i < 15; i++) {
    const char *value = values[i];
    if (i == 3)
      value = sha;
    if (i == 4)
      value = rev_json;
    yyjson_mut_doc *a = base_doc();
    set_raw(a, yyjson_mut_obj_get(node_of(a, "app"), "locked"), fields[i], value);
    char *text = text_of(a);
    LockResult r = lock_check(text, text, 1, NULL);
    CHECK(r.ok);
    g_free(r.out);
    g_free(text);
    (void)dyn_rev;
  }
  g_free(rev);
  g_free(rev_json);
  g_free(sha);
}
static void test_lock_malformed(void) {
  test_name = "malformed roots";
  const char *roots[] = {"null", "[]", "{}"};
  for (size_t i = 0; i < 3; i++) {
    LockResult r = lock_check(roots[i], roots[i], 1, NULL);
    CHECK(r.ok);
    g_free(r.out);
  }
  yyjson_mut_doc *base = base_doc();
  char *text = doc_text(base);
  char *version_true = replace_first(text, "\"version\":7", "\"version\":true");
  char *version_8 = replace_first(text, "\"version\":7", "\"version\":8");
  char *root_missing = replace_first(text, "\"root\":\"root\"", "\"root\":\"missing\"");
  char *nodes_arr = replace_first(text, "\"nodes\":{", "\"nodes\":[{");
  /* nodes: [] is not a simple replace of object; build explicitly */
  g_free(nodes_arr);
  yyjson_mut_doc *nodes_bad = clone_doc(base);
  yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(nodes_bad), "nodes");
  yyjson_mut_obj_add_val(nodes_bad, yyjson_mut_doc_get_root(nodes_bad), "nodes",
                         yyjson_mut_arr(nodes_bad));
  char *nodes_text = text_of(nodes_bad);
  yyjson_mut_doc *extra = clone_doc(base);
  yyjson_mut_obj_add_bool(extra, yyjson_mut_doc_get_root(extra), "extra", true);
  char *extra_text = text_of(extra);
  const char *bad_roots[] = {version_true, version_8, root_missing, nodes_text, extra_text};
  for (size_t i = 0; i < 5; i++) {
    LockResult r = lock_check(bad_roots[i], bad_roots[i], 1, NULL);
    CHECK(r.ok);
    g_free(r.out);
  }
  const char *nodes[] = {"null", "{}", "{\"locked\":null}", "{\"extra\":1}", NULL, NULL, NULL};
  char *github = doc_text(base);
  /* build node variants from a fresh base by replacing app */
  const char *variants[] = {
      "null", "{}", "{\"locked\":null}", "{\"extra\":1}",
      NULL, NULL, NULL};
  (void)nodes;
  (void)variants;
  (void)github;
  yyjson_mut_doc_free(base);
  for (size_t i = 0; i < 7; i++) {
    yyjson_mut_doc *a = base_doc();
    if (i == 0)
      set_raw(a, nodes_of(a), "app", "null");
    else if (i == 1)
      set_raw(a, nodes_of(a), "app", "{}");
    else if (i == 2)
      set_raw(a, nodes_of(a), "app", "{\"locked\":null}");
    else if (i == 3)
      set_raw(a, nodes_of(a), "app", "{\"extra\":1}");
    else if (i == 4) {
      yyjson_mut_val *node = github_node(a, "willfish", "fixture");
      yyjson_mut_obj_add_val(a, node, "inputs", yyjson_mut_arr(a));
      yyjson_mut_obj_remove_key(nodes_of(a), "app");
      yyjson_mut_obj_add_val(a, nodes_of(a), "app", node);
    } else if (i == 5) {
      yyjson_mut_val *node = github_node(a, "willfish", "fixture");
      yyjson_mut_obj_add_int(a, node, "flake", 0);
      yyjson_mut_obj_remove_key(nodes_of(a), "app");
      yyjson_mut_obj_add_val(a, nodes_of(a), "app", node);
    } else {
      yyjson_mut_val *node = github_node(a, "willfish", "fixture");
      yyjson_mut_obj_add_bool(a, node, "flake", false);
      yyjson_mut_val *inputs = yyjson_mut_obj(a);
      yyjson_mut_obj_add_strcpy(a, inputs, "dep", "dep");
      yyjson_mut_obj_add_val(a, node, "inputs", inputs);
      yyjson_mut_obj_remove_key(nodes_of(a), "app");
      yyjson_mut_obj_add_val(a, nodes_of(a), "app", node);
    }
    char *one = text_of(a);
    LockResult r = lock_check(one, one, 1, NULL);
    CHECK(r.ok);
    g_free(r.out);
    g_free(one);
  }
  yyjson_mut_doc *a = base_doc();
  char *raw = text_of(a);
  char *dup_version = replace_first(raw, "\"version\":7", "\"version\":7,\"version\":7");
  char *dup_owner =
      replace_first(raw, "\"owner\":\"willfish\"", "\"owner\":\"willfish\",\"owner\":\"willfish\"");
  LockResult r = lock_check(raw, dup_version, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  r = lock_check(dup_version, raw, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  r = lock_check(raw, dup_owner, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  r = lock_check(raw, "not JSON", 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(raw);
  g_free(dup_version);
  g_free(dup_owner);
  g_free(version_true);
  g_free(version_8);
  g_free(root_missing);
  g_free(nodes_text);
  g_free(extra_text);
}
static void test_lock_cycles(void) {
  test_name = "nested follows";
  yyjson_mut_doc *a = base_doc();
  yyjson_mut_val *root_inputs = yyjson_mut_obj_get(node_of(a, "root"), "inputs");
  yyjson_mut_val *alias = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, alias, "app");
  yyjson_mut_arr_add_strcpy(a, alias, "dep");
  yyjson_mut_obj_add_val(a, root_inputs, "alias", alias);
  yyjson_mut_val *second = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, second, "alias");
  yyjson_mut_obj_add_val(a, root_inputs, "second", second);
  yyjson_mut_val *app_inputs = yyjson_mut_obj_get(node_of(a, "app"), "inputs");
  yyjson_mut_val *shared = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, shared, "second");
  yyjson_mut_obj_add_val(a, app_inputs, "shared", shared);
  yyjson_mut_obj_add_val(a, app_inputs, "parent", yyjson_mut_arr(a));
  char *text = text_of(a);
  LockResult r = lock_check(text, text, 0, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(text);
  a = base_doc();
  yyjson_mut_val *dep_inputs = yyjson_mut_obj(a);
  yyjson_mut_obj_add_strcpy(a, dep_inputs, "back", "app");
  yyjson_mut_obj_add_val(a, node_of(a, "dep"), "inputs", dep_inputs);
  text = text_of(a);
  r = lock_check(text, text, 0, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(text);
  a = base_doc();
  yyjson_mut_val *loop = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, loop, "loop");
  yyjson_mut_obj_add_val(a, yyjson_mut_obj_get(node_of(a, "root"), "inputs"), "loop", loop);
  yyjson_mut_val *use = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, use, "loop");
  yyjson_mut_obj_add_val(a, yyjson_mut_obj_get(node_of(a, "app"), "inputs"), "shared", use);
  text = text_of(a);
  r = lock_check(text, text, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(text);
  a = base_doc();
  yyjson_mut_val *x = yyjson_mut_arr(a), *y = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, x, "y");
  yyjson_mut_arr_add_strcpy(a, y, "x");
  yyjson_mut_obj_add_val(a, yyjson_mut_obj_get(node_of(a, "root"), "inputs"), "x", x);
  yyjson_mut_obj_add_val(a, yyjson_mut_obj_get(node_of(a, "root"), "inputs"), "y", y);
  use = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, use, "x");
  yyjson_mut_obj_add_val(a, yyjson_mut_obj_get(node_of(a, "app"), "inputs"), "shared", use);
  text = text_of(a);
  r = lock_check(text, text, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(text);
  a = base_doc();
  x = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, x, "app");
  yyjson_mut_arr_add_strcpy(a, x, "shared");
  yyjson_mut_obj_add_val(a, yyjson_mut_obj_get(node_of(a, "root"), "inputs"), "x", x);
  use = yyjson_mut_arr(a);
  yyjson_mut_arr_add_strcpy(a, use, "x");
  yyjson_mut_obj_add_val(a, yyjson_mut_obj_get(node_of(a, "app"), "inputs"), "shared", use);
  text = text_of(a);
  r = lock_check(text, text, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(text);
}
static void test_lock_dangling(void) {
  test_name = "dangling references";
  const char *targets[] = {"\"absent\"", "[\"absent\"]", "[\"app\",\"missing\"]", "[1]",
                           "null", "1", "true", "{\"follows\":\"app\"}", "[\"\"]",
                           "[\"bad\\nname\"]"};
  for (size_t i = 0; i < 10; i++) {
    yyjson_mut_doc *a = base_doc();
    yyjson_mut_obj_remove_key(node_of(a, "root"), "inputs");
    yyjson_mut_obj_add_val(a, node_of(a, "root"), "inputs", yyjson_mut_obj(a));
    set_raw(a, yyjson_mut_obj_get(node_of(a, "app"), "inputs"), "dep", targets[i]);
    char *text = text_of(a);
    LockResult r = lock_check(text, text, 1, NULL);
    CHECK(r.ok);
    g_free(r.out);
    g_free(text);
  }
  yyjson_mut_doc *a = base_doc();
  char *text = text_of(a);
  char *nulled = replace_first(text, "\"dep\":\"dep\"", "\"dep\":\"dep\\u0000suffix\"");
  CHECK(nulled);
  LockResult r = lock_check(nulled, nulled, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(nulled);
  g_free(text);
}
static void test_lock_types(void) {
  test_name = "source integer/bool types";
  yyjson_mut_doc *a = base_doc();
  yyjson_mut_obj_add_bool(a, yyjson_mut_obj_get(node_of(a, "app"), "original"), "custom", true);
  yyjson_mut_doc *b = clone_doc(a);
  yyjson_mut_obj_remove_key(yyjson_mut_obj_get(node_of(b, "app"), "original"), "custom");
  yyjson_mut_obj_add_int(b, yyjson_mut_obj_get(node_of(b, "app"), "original"), "custom", 1);
  char *at = doc_text(a), *bt = doc_text(b);
  LockResult r = lock_check(at, bt, 1, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(at);
  g_free(bt);
  yyjson_mut_doc_free(a);
  yyjson_mut_doc_free(b);
  a = base_doc();
  at = doc_text(a);
  yyjson_doc *imm = yyjson_read(at, strlen(at), 0);
  yyjson_mut_doc *d = yyjson_doc_mut_copy(imm, NULL);
  yyjson_doc_free(imm);
  yyjson_mut_val *original = yyjson_mut_obj(d);
  yyjson_mut_obj_add_strcpy(d, original, "repo", "fixture");
  yyjson_mut_obj_add_strcpy(d, original, "owner", "willfish");
  yyjson_mut_obj_add_strcpy(d, original, "type", "github");
  yyjson_mut_obj_remove_key(node_of(d, "app"), "original");
  yyjson_mut_obj_add_val(d, node_of(d, "app"), "original", original);
  bt = text_of(d);
  r = lock_check(at, bt, 0, NULL);
  CHECK(r.ok);
  g_free(r.out);
  char *neg = replace_first(at, "\"lastModified\":1", "\"lastModified\":-0");
  char *zero = replace_first(at, "\"lastModified\":1", "\"lastModified\":0");
  r = lock_check(neg, zero, 0, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(neg);
  g_free(zero);
  g_free(at);
  g_free(bt);
  yyjson_mut_doc_free(a);
}
static void test_lock_repo(void) {
  test_name = "current repository lock";
  char *root = repo_root();
  CHECK(root);
  char *path = g_build_filename(root, "flake.lock", NULL);
  size_t n = 0;
  char *text = read_file(path, &n);
  CHECK(text);
  yyjson_doc *imm = yyjson_read(text, n, 0);
  CHECK(imm);
  yyjson_mut_doc *b = yyjson_doc_mut_copy(imm, NULL);
  yyjson_doc_free(imm);
  bool found = false;
  size_t idx, max;
  yyjson_mut_val *key, *val;
  yyjson_mut_obj_foreach(nodes_of(b), idx, max, key, val) {
    yyjson_mut_val *locked = yyjson_mut_obj_get(val, "locked");
    if (!locked)
      continue;
    const char *type = yyjson_mut_get_str(yyjson_mut_obj_get(locked, "type"));
    const char *owner = yyjson_mut_get_str(yyjson_mut_obj_get(locked, "owner"));
    if (type && owner && !strcmp(type, "github") && !strcmp(owner, "willfish")) {
      char *rev = repeat_char('b', 40);
      set_str(b, locked, "rev", rev);
      g_free(rev);
      found = true;
      break;
    }
  }
  CHECK(found);
  char *changed = doc_text(b);
  LockResult r = lock_check(text, text, 0, NULL);
  CHECK(r.ok);
  g_free(r.out);
  r = lock_check(text, changed, 0, NULL);
  CHECK(r.ok);
  g_free(r.out);
  g_free(changed);
  yyjson_mut_doc_free(b);
  g_free(text);
  g_free(path);
  g_free(root);
}

static const char *import_start = "    # BEGIN generated Omarchy community inputs";
static const char *import_end = "    # END generated Omarchy community inputs";
static const char *import_source = "https://example.test/pinned/commit/themes.html";
static char *import_initial(void) {
  return g_strdup_printf("before\n%s\nold block\n%s\nafter\n", import_start, import_end);
}
static char *figure(const char *slug, const char *label, const char *url) {
  if (!slug)
    slug = "blue";
  if (!label)
    label = "Blue";
  if (!url)
    url = "https://github.com/owner/blue";
  return g_strdup_printf(
      "<figure><img src=\"/themes/%s.png\"><figcaption><a href=\"%s\">%s</a></figcaption></figure>",
      slug, url, label);
}
typedef void (*VerifyFn)(const char *root);
typedef struct {
  bool ok;
  char *flake;
  yyjson_doc *catalogue;
  char *stdout;
} ImportResult;
static ImportResult import_check(const char *page, int status, const char *unavailable,
                                 const char *flake, VerifyFn verify) {
  ImportResult result = {0};
  if (!unavailable)
    unavailable = "{}";
  char *owned_flake = NULL;
  if (!flake) {
    owned_flake = import_initial();
    flake = owned_flake;
  }
  const char *legacy = getenv("COMMUNITY_IMPORT_LEGACY");
  const char *python = getenv("COMMUNITY_IMPORT_PYTHON");
  if (!python || !*python)
    python = "python3";
  int rounds = legacy && *legacy ? 2 : 1;
  char *bin = sibling("import-omarchy-community");
  bool ok = true;
  for (int old = 0; old < rounds && ok; old++) {
    char *root = temp_dir("community-import-");
    char *themes = g_build_filename(root, "home/user/themes", NULL);
    char *scripts = g_build_filename(root, "scripts", NULL);
    g_mkdir_with_parents(themes, 0700);
    g_mkdir_with_parents(scripts, 0700);
    char *flake_path = g_build_filename(root, "flake.nix", NULL);
    char *unavail = g_build_filename(themes, "community-unavailable.json", NULL);
    char *catalogue = g_build_filename(themes, "community.json", NULL);
    char *page_path = g_build_filename(root, "page.html", NULL);
    write_file(flake_path, flake, strlen(flake), 0600);
    write_file(unavail, unavailable, strlen(unavailable), 0600);
    write_file(catalogue, "previous", 8, 0600);
    chmod(flake_path, 0640);
    chmod(catalogue, 0600);
    ino_t ino = inode_of(flake_path);
    write_file(page_path, page, strlen(page), 0600);
    char *script = NULL;
    if (old) {
      script = g_build_filename(scripts, "import-omarchy-community.py", NULL);
      size_t n = 0;
      char *body = read_file(legacy, &n);
      if (!body || !write_file(script, body, n, 0644))
        ok = false;
      g_free(body);
    }
    char *argv_new[] = {bin, "--root", root, page_path, (char *)import_source, NULL};
    char *argv_old[] = {(char *)python, script, page_path, (char *)import_source, NULL};
    Proc p = ok ? proc_run(old ? python : bin, old ? argv_old : argv_new, g_get_tmp_dir(),
                           NULL, 10000)
                : (Proc){0};
    if (ok && (!expect_code(&p, status)))
      ok = false;
    if (ok && status) {
      if (!file_eq(flake_path, flake, strlen(flake)) || !file_eq(catalogue, "previous", 8))
        ok = false;
    } else if (ok) {
      if (inode_of(flake_path) != ino || !mode_is(flake_path, 0640) || !mode_is(catalogue, 0600))
        ok = false;
      size_t fn = 0, cn = 0;
      char *flake_text = read_file(flake_path, &fn);
      char *cat_text = read_file(catalogue, &cn);
      yyjson_doc *cat = cat_text ? yyjson_read(cat_text, cn, 0) : NULL;
      if (!old) {
        result.flake = flake_text;
        result.catalogue = cat;
        result.stdout = g_strdup(p.out ? p.out : "");
        g_free(cat_text);
      } else {
        yyjson_doc *expected = result.catalogue;
        if (!result.flake || !flake_text || strcmp(result.flake, flake_text) || !cat ||
            !expected || !json_equal(yyjson_doc_get_root(cat), yyjson_doc_get_root(expected)) ||
            !result.stdout || !p.out || strcmp(result.stdout, p.out))
          ok = false;
        g_free(flake_text);
        g_free(cat_text);
        yyjson_doc_free(cat);
      }
      if (verify)
        verify(root);
    }
    proc_free(&p);
    g_free(script);
    g_free(page_path);
    g_free(catalogue);
    g_free(unavail);
    g_free(flake_path);
    g_free(scripts);
    g_free(themes);
  }
  result.ok = ok;
  g_free(bin);
  g_free(owned_flake);
  if (!ok) {
    g_free(result.flake);
    g_free(result.stdout);
    yyjson_doc_free(result.catalogue);
    result.flake = result.stdout = NULL;
    result.catalogue = NULL;
  }
  return result;
}
static void test_import_valid(void) {
  test_name = "valid figures";
  char *zebra = figure("zebra", "Zèbre &amp; 雪", "https://github.com/vendor/zebra/");
  char *blue = figure(NULL, NULL, NULL);
  char *page = g_strconcat(zebra, blue, NULL);
  ImportResult r = import_check(page, 0, "{\"zebra\":\"private or deleted\"}", NULL, NULL);
  CHECK(r.ok);
  char *expect = g_strdup_printf(
      "before\n%s\n    omarchy-theme-blue = {\n      url = "
      "\"git+https://github.com/owner/blue?shallow=1\";\n      flake = false;\n    };\n%s\n"
      "after\n",
      import_start, import_end);
  CHECK(r.flake && !strcmp(r.flake, expect));
  yyjson_val *themes = yyjson_obj_get(yyjson_doc_get_root(r.catalogue), "themes");
  CHECK(yyjson_obj_size(themes) == 2);
  size_t idx, max;
  yyjson_val *key, *val;
  const char *order[] = {"blue", "zebra"};
  size_t n = 0;
  yyjson_obj_foreach(themes, idx, max, key, val) {
    CHECK(n < 2 && val_streq(key, order[n]));
    n++;
  }
  CHECK(val_streq(yyjson_obj_get(yyjson_obj_get(themes, "zebra"), "label"), "Zèbre & 雪"));
  CHECK(val_streq(yyjson_obj_get(yyjson_doc_get_root(r.catalogue), "source"), import_source));
  CHECK(r.stdout && strstr(r.stdout, "Read 2 community themes"));
  CHECK(strstr(r.stdout, "Unavailable: zebra: private or deleted"));
  g_free(expect);
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
  g_free(page);
  g_free(zebra);
  g_free(blue);
}
static void verify_nobreak(const char *root) {
  char *path = g_build_filename(root, "home/user/themes/community.json", NULL);
  yyjson_doc *doc = NULL;
  size_t n = 0;
  char *text = read_file(path, &n);
  if (text)
    doc = yyjson_read(text, n, 0);
  yyjson_val *label = doc ? yyjson_obj_get(
                               yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "themes"),
                                              "blue"),
                               "label")
                          : NULL;
  if (!val_streq(label, "Before Blue \u2060 & bold after"))
    fail(__LINE__, "caption label");
  yyjson_doc_free(doc);
  g_free(text);
  g_free(path);
}
static void test_import_tokens(void) {
  test_name = "HTML token parsing";
  const char *page =
      "<!doctype html><html><!-- ignored ' quote --><body><FIGURE><IMG SRC='discard.png' "
      "src='/images/blue.png'><FIGCAPTION> Before <A href='https://bad.test/no' "
      "HREF='https://github.com/owner/blue'>Blue &NoBreak; &amp; <b>bold</b></A> after "
      "</FIGCAPTION></FIGURE></body></html>";
  ImportResult r = import_check(page, 0, NULL, NULL, verify_nobreak);
  CHECK(r.ok);
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
}
static void verify_last(const char *root) {
  char *path = g_build_filename(root, "home/user/themes/community.json", NULL);
  size_t n = 0;
  char *text = read_file(path, &n);
  yyjson_doc *doc = text ? yyjson_read(text, n, 0) : NULL;
  yyjson_val *entry = doc ? yyjson_obj_get(
                               yyjson_obj_get(yyjson_doc_get_root(doc), "themes"), "last")
                          : NULL;
  if (!val_streq(yyjson_obj_get(entry, "url"), "https://github.com/owner/last") ||
      !val_streq(yyjson_obj_get(entry, "label"), "FirstLast"))
    fail(__LINE__, "last caption");
  yyjson_doc_free(doc);
  g_free(text);
  g_free(path);
}
static void test_import_last(void) {
  test_name = "last image and caption";
  const char *page =
      "<figure><a href='https://bad.test/ignore'>Outside</a><img src='first.png'><img "
      "src='last.webp'><figcaption><a href='https://github.com/owner/first'>First</a><a "
      "href='https://github.com/owner/last///'>Last</a></figcaption></figure>";
  ImportResult r = import_check(page, 0, NULL, NULL, verify_last);
  CHECK(r.ok);
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
}
static void verify_only_blue(const char *root) {
  char *path = g_build_filename(root, "home/user/themes/community.json", NULL);
  size_t n = 0;
  char *text = read_file(path, &n);
  yyjson_doc *doc = text ? yyjson_read(text, n, 0) : NULL;
  yyjson_val *themes = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "themes") : NULL;
  size_t count = 0;
  bool blue = false;
  if (themes) {
    size_t idx, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(themes, idx, max, key, val) {
      count++;
      if (val_streq(key, "blue"))
        blue = true;
    }
  }
  if (count != 1 || !blue)
    fail(__LINE__, "only blue");
  yyjson_doc_free(doc);
  g_free(text);
  g_free(path);
}
static void test_import_empty(void) {
  test_name = "empty pages";
  char *fig = figure(NULL, NULL, NULL);
  char *pages[5];
  pages[0] = g_strdup("");
  pages[1] = g_strdup("<html><p>none</p></html>");
  pages[2] = g_strdup(
      "<figure><img src=\"blue.png\"><figcaption><a href=\"https://github.com/owner/blue\">Blue</a></figcaption>");
  pages[3] = g_strdup("<figure/>");
  pages[4] = g_strconcat("<figure/>", fig, NULL);
  for (int i = 0; i < 5; i++) {
    ImportResult r = import_check(pages[i], 1, NULL, NULL, NULL);
    CHECK(r.ok);
    g_free(pages[i]);
  }
  char *partial = g_strconcat(fig, "<figure><img src=\"unclosed.png\">", NULL);
  ImportResult r = import_check(partial, 0, NULL, NULL, verify_only_blue);
  CHECK(r.ok);
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
  g_free(partial);
  g_free(fig);
}
static void test_import_invalid(void) {
  test_name = "invalid IDs";
  const char *slugs[] = {"blue_theme", "Blue", "blue--theme", "-blue", "blue-", NULL, "blue.dot"};
  char *long_slug = repeat_char('a', 54);
  slugs[5] = long_slug;
  for (size_t i = 0; i < 7; i++) {
    char *page = figure(slugs[i], NULL, NULL);
    ImportResult r = import_check(page, 1, NULL, NULL, NULL);
    CHECK(r.ok);
    g_free(page);
  }
  g_free(long_slug);
  char *escape = figure("../escape", NULL, NULL);
  ImportResult r = import_check(escape, 0, NULL, NULL, NULL);
  CHECK(r.ok);
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
  g_free(escape);
  char *one = figure(NULL, NULL, NULL);
  char *two = g_strconcat(one, one, NULL);
  r = import_check(two, 1, NULL, NULL, NULL);
  CHECK(r.ok);
  g_free(two);
  const char *urls[] = {
      "http://github.com/owner/repo", "https://evil.test/owner/repo",
      "https://user@github.com/owner/repo", "https://github.com/owner/repo?x=1",
      "https://github.com/owner/repo/tree/main", "https://github.com/owner/repo#fragment"};
  for (size_t i = 0; i < 6; i++) {
    char *page = figure("blue", "Blue", urls[i]);
    r = import_check(page, 1, NULL, NULL, NULL);
    CHECK(r.ok);
    g_free(page);
  }
  g_free(one);
}
static void verify_label(const char *root) {
  char *path = g_build_filename(root, "home/user/themes/community.json", NULL);
  size_t n = 0;
  char *text = read_file(path, &n);
  yyjson_doc *doc = text ? yyjson_read(text, n, 0) : NULL;
  yyjson_val *label =
      doc ? yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "themes"),
                                          "blue"),
                           "label")
          : NULL;
  if (!val_streq(label, "Blue & White"))
    fail(__LINE__, "trimmed label");
  yyjson_doc_free(doc);
  g_free(text);
  g_free(path);
}
static void test_import_labels(void) {
  test_name = "labels";
  char *page = figure("blue", "\xC2\x85\u2003Blue &amp; White\u3000", NULL);
  ImportResult r = import_check(page, 0, NULL, NULL, verify_label);
  CHECK(r.ok);
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
  g_free(page);
  const char *labels[] = {"", " \t\n ", "bad&#10;label", "bad&#9;label"};
  for (size_t i = 0; i < 4; i++) {
    page = figure("blue", labels[i], NULL);
    r = import_check(page, 1, NULL, NULL, NULL);
    CHECK(r.ok);
    g_free(page);
  }
}
static void test_import_missing_data(void) {
  test_name = "missing required figure data";
  char *fig = figure(NULL, NULL, NULL);
  char *dup = g_strdup(fig);
  char *href = strstr(dup, "href=\"https://github.com/owner/blue\"");
  CHECK(href);
  char *rewritten = g_strdup_printf(
      "%.*s%s%s", (int)(href - dup), dup,
      "href=\"https://github.com/owner/blue\" href=\"javascript:bad\"",
      href + strlen("href=\"https://github.com/owner/blue\""));
  const char *bodies[] = {
      "<figure><figcaption><a href=\"https://github.com/owner/blue\">Blue</a></figcaption></figure>",
      "<figure><img src=\"blue.png\"></figure>",
      "<figure><img src><figcaption><a href=\"https://github.com/owner/blue\">Blue</a></figcaption></figure>",
      rewritten};
  for (size_t i = 0; i < 4; i++) {
    ImportResult r = import_check(bodies[i], 1, NULL, NULL, NULL);
    CHECK(r.ok);
  }
  g_free(rewritten);
  g_free(dup);
  g_free(fig);
}
static void test_import_markers(void) {
  test_name = "generated markers";
  char *page = figure(NULL, NULL, NULL);
  char *flakes[] = {
      g_strdup("none"),
      g_strdup_printf("%s\nmissing", import_start),
      g_strdup_printf("%s\n%s\n%s", import_start, import_start, import_end),
      g_strdup_printf("%s\n%s\n%s", import_start, import_end, import_end),
      g_strdup_printf("%s\n%s", import_end, import_start)};
  for (size_t i = 0; i < 5; i++) {
    ImportResult r = import_check(page, 1, "{}", flakes[i], NULL);
    CHECK(r.ok);
    g_free(flakes[i]);
  }
  g_free(page);
}
static void test_import_unavailable(void) {
  test_name = "all-unavailable catalogues";
  char *page = figure(NULL, NULL, NULL);
  ImportResult r = import_check(page, 0, "{\"blue\":\"unavailable\"}", NULL, NULL);
  CHECK(r.ok);
  char *expect = g_strdup_printf("before\n%s\n%s\nafter\n", import_start, import_end);
  CHECK(r.flake && !strcmp(r.flake, expect));
  CHECK(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(r.catalogue), "themes"), "blue"));
  g_free(expect);
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
  g_free(page);
}
static void test_import_page(void) {
  test_name = "pinned website HTML";
  const char *page_path = getenv("COMMUNITY_IMPORT_PAGE");
  if (!page_path || !*page_path) {
    fprintf(stderr, "SKIP pinned page\n");
    return;
  }
  char *root = repo_root();
  CHECK(root);
  char *unavail_path =
      g_build_filename(root, "home/user/themes/community-unavailable.json", NULL);
  size_t un = 0, pn = 0;
  char *unavailable = read_file(unavail_path, &un);
  char *page = read_file(page_path, &pn);
  CHECK(unavailable && page);
  ImportResult r = import_check(page, 0, unavailable, NULL, NULL);
  CHECK(r.ok);
  CHECK(r.catalogue &&
        yyjson_obj_size(yyjson_obj_get(yyjson_doc_get_root(r.catalogue), "themes")) > 0);
  fprintf(stderr, "Compared %zu pinned community themes\n",
          yyjson_obj_size(yyjson_obj_get(yyjson_doc_get_root(r.catalogue), "themes")));
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
  g_free(page);
  g_free(unavailable);
  g_free(unavail_path);
  g_free(root);
}
static void verify_suffix(const char *root) {
  char *path = g_build_filename(root, "flake.nix", NULL);
  size_t n = 0;
  char *text = read_file(path, &n);
  const char *suffix = "\nsuffix\n";
  size_t sn = strlen(suffix);
  struct stat st;
  if (!text || n < sn || memcmp(text + n - sn, suffix, sn) || stat(path, &st) != 0 ||
      !S_ISREG(st.st_mode))
    fail(__LINE__, "suffix bytes");
  g_free(text);
  g_free(path);
}
static void test_import_modes(void) {
  test_name = "direct writes retain modes";
  char *page = figure(NULL, NULL, NULL);
  char *flake = g_strdup_printf("prefix\r\n%s\r\nold\r\n%s\r\nsuffix\r\n", import_start, import_end);
  ImportResult r = import_check(page, 0, NULL, flake, verify_suffix);
  CHECK(r.ok);
  g_free(r.flake);
  g_free(r.stdout);
  yyjson_doc_free(r.catalogue);
  g_free(flake);
  g_free(page);
}

static bool mem_has(const char *text, size_t n, const char *needle) {
  size_t len = needle ? strlen(needle) : 0;
  if (!text || !needle || len > n)
    return false;
  for (size_t i = 0; i + len <= n; i++) {
    if (!memcmp(text + i, needle, len))
      return true;
  }
  return false;
}
#include "extra.inc"
int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "--fixture-storage"))
    return fixture_storage(argc, argv);
  void (*tests[])(void) = {
      test_lock_accept, test_lock_allow, test_lock_identity, test_lock_attrs,
      test_lock_sources, test_lock_malformed, test_lock_cycles, test_lock_dangling,
      test_lock_types, test_lock_repo, test_import_valid, test_import_tokens,
      test_import_last, test_import_empty, test_import_invalid, test_import_labels,
      test_import_missing_data, test_import_markers, test_import_unavailable,
      test_import_page, test_import_modes, test_audit_valid, test_audit_schema,
      test_audit_process, test_audit_duplicate, test_audit_paths, test_audit_collision,
      test_audit_frontmatter, test_audit_findings, test_audit_manual, test_audit_long,
      test_audit_overlap, test_audit_freshness, test_audit_symlink, test_audit_cli,
      test_storage_scope, test_storage_defaults, test_storage_nh, test_storage_dry,
      test_storage_skip, test_storage_separator, test_storage_inventory,
      test_storage_shell, test_storage_accounting};
  for (size_t i = 0; i < G_N_ELEMENTS(tests); i++) {
    int before = failures;
    tests[i]();
    fprintf(stderr, "%s %s\n", failures == before ? "PASS" : "FAIL", test_name);
  }
  fprintf(stderr, "%d failures\n", failures);
  return failures ? 1 : 0;
}
