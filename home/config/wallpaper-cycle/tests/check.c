#define _GNU_SOURCE
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
#include <yyjson.h>

static const char *current_test;
static int failures;
static char *binary, *fake_bin;
static const char *legacy;

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
static void begin(const char *name) {
  current_test = name;
  printf("RUN %s\n", name);
  fflush(stdout);
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
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
typedef struct {
  int status;
  bool error;
  int signal;
} Proc;
static Proc spawn_wait(char *const *argv, const char *const *env_set, int timeout_ms) {
  Proc p = {0};
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
    if (env_set)
      for (size_t i = 0; env_set[i]; i++)
        putenv((char *)env_set[i]);
    execvp(argv[0], argv);
    _exit(127);
  }
  close(outp[1]);
  close(errp[1]);
  int64_t deadline = now_ms() + timeout_ms;
  while (outp[0] >= 0 || errp[0] >= 0) {
    if (now_ms() > deadline) {
      kill(pid, SIGKILL);
      p.error = true;
      break;
    }
    struct pollfd fds[2];
    nfds_t nfd = 0;
    if (outp[0] >= 0)
      fds[nfd++] = (struct pollfd){outp[0], POLLIN, 0};
    if (errp[0] >= 0)
      fds[nfd++] = (struct pollfd){errp[0], POLLIN, 0};
    poll(fds, nfd, 50);
    char tmp[1024];
    for (nfds_t i = 0; i < nfd; i++) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      if (read(fds[i].fd, tmp, sizeof tmp) <= 0) {
        if (fds[i].fd == outp[0])
          outp[0] = -1;
        else
          errp[0] = -1;
        close(fds[i].fd);
      }
    }
  }
  if (outp[0] >= 0)
    close(outp[0]);
  if (errp[0] >= 0)
    close(errp[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status))
    p.status = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) {
    p.signal = WTERMSIG(status);
    p.error = true;
  } else
    p.error = true;
  return p;
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
  out[0] = (char)(0xe0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[2] = (char)(0x80 | (cp & 0x3f));
  return 3;
}
static const uint32_t py_spaces[] = {
    9,     10,    11,    12,    13,    28,    29,    30,    31,    32,
    0x85,  0xa0,  0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
    0x2006, 0x2007, 0x2008, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f,
    0x3000};
typedef struct {
  char *root, *state, *theme, *bin, *catalogue, *config, *log, *link, *current,
      *live, *race, *fail, *path_env;
} Fix;
static void write_text(const char *path, const char *text) {
  if (!g_file_set_contents(path, text, -1, NULL)) {
    perror(path);
    exit(1);
  }
}
static char *read_text(const char *path) {
  char *text = NULL;
  g_file_get_contents(path, &text, NULL, NULL);
  return text;
}
static Fix *fixture(const char *names_json, const char *preferred) {
  Fix *f = g_new0(Fix, 1);
  f->root = g_dir_make_tmp("wallpaper-cycle-XXXXXX", NULL);
  if (!f->root) {
    perror("tmpdir");
    exit(1);
  }
  f->state = g_build_filename(f->root, "state", NULL);
  f->theme = g_build_filename(f->root, "theme", NULL);
  f->bin = g_build_filename(f->root, "bin", NULL);
  char *backgrounds = g_build_filename(f->theme, "backgrounds", NULL);
  g_mkdir_with_parents(f->state, 0755);
  g_mkdir_with_parents(f->bin, 0755);
  g_mkdir_with_parents(backgrounds, 0755);
  const char *files[] = {"a.png", "b.png", "c.png", "雪.png", "other.png"};
  for (size_t i = 0; i < 5; i++) {
    char *path = g_build_filename(backgrounds, files[i], NULL);
    write_text(path, files[i]);
    g_free(path);
  }
  g_free(backgrounds);
  char *theme_json = g_build_filename(f->theme, "theme.json", NULL);
  char *pref = g_strdup_printf("\"%s\"", preferred ? preferred : "a.png");
  char *body = g_strdup_printf(
      "{\"backgrounds\":%s,\"preferred\":%s}",
      names_json ? names_json : "[\"a.png\",\"b.png\",\"c.png\"]", pref);
  write_text(theme_json, body);
  g_free(body);
  g_free(pref);
  g_free(theme_json);
  f->catalogue = g_build_filename(f->root, "catalogue.json", NULL);
  write_text(f->catalogue,
             "{\"default\":\"default-palette\",\"palettes\":{\"default-palette\":{\"session\":{\"dark\":{\"wallpaper.png\":\"nix-theme:rose\"},\"light\":{\"wallpaper.png\":\"nix-theme:light-rose\"}}}}}");
  f->log = g_build_filename(f->root, "log", NULL);
  f->config = g_build_filename(f->root, "config.json", NULL);
  f->link = g_build_filename(f->state, "wallpaper-source", NULL);
  f->current = g_build_filename(f->state, "wallpaper-current", NULL);
  f->live = g_build_filename(f->state, "active", "wallpaper-live.png", NULL);
  f->race = g_strdup("");
  f->fail = g_strdup("");
  const char *names[] = {"nix", "magick"};
  for (size_t i = 0; i < 2; i++) {
    char *path = g_build_filename(f->bin, names[i], NULL);
    if (symlink(fake_bin, path) < 0) {
      perror(path);
      exit(1);
    }
    g_free(path);
  }
  const char *old = g_getenv("PATH");
  f->path_env = g_strdup_printf("PATH=%s%s%s", f->bin, old ? ":" : "", old ? old : "");
  return f;
}
static void fix_free(Fix *f) {
  char *rm[] = {"rm", "-rf", "--", f->root, NULL};
  spawn_wait(rm, NULL, 5000);
  g_free(f->root);
  g_free(f->state);
  g_free(f->theme);
  g_free(f->bin);
  g_free(f->catalogue);
  g_free(f->config);
  g_free(f->log);
  g_free(f->link);
  g_free(f->current);
  g_free(f->live);
  g_free(f->race);
  g_free(f->fail);
  g_free(f->path_env);
  g_free(f);
}
static void prepare(Fix *f) {
  if (!g_file_test(f->link, G_FILE_TEST_EXISTS) && symlink(f->theme, f->link) < 0)
    FAIL("prepare link: %s", g_strerror(errno));
}
static void write_db(Fix *f) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_strcpy(doc, root, "state", f->state);
  yyjson_mut_obj_add_strcpy(doc, root, "theme", f->theme);
  yyjson_mut_obj_add_strcpy(doc, root, "log", f->log);
  yyjson_mut_obj_add_strcpy(doc, root, "race", f->race);
  yyjson_mut_obj_add_strcpy(doc, root, "fail", f->fail);
  char *json = yyjson_mut_write(doc, 0, NULL);
  yyjson_mut_doc_free(doc);
  if (!json) {
    FAIL("db allocation failed");
    exit(1);
  }
  write_text(f->config, json);
  free(json);
}
static int run_dir(Fix *f, const char *direction) {
  write_db(f);
  char *magick = g_build_filename(f->bin, "magick", NULL);
  GPtrArray *args = g_ptr_array_new();
  if (legacy) {
    g_ptr_array_add(args, (gpointer) "python3");
    g_ptr_array_add(args, (gpointer)legacy);
  } else
    g_ptr_array_add(args, binary);
  const char *fixed[] = {"--state", f->state, "--catalogue", f->catalogue, "--flake",
                         "/flake with spaces;no-shell", "--magick", magick,
                         "--direction", direction};
  for (size_t i = 0; i < G_N_ELEMENTS(fixed); i++)
    g_ptr_array_add(args, (gpointer)fixed[i]);
  g_ptr_array_add(args, NULL);
  char *db = g_strdup_printf("CYCLE_FAKE_DB=%s", f->config);
  const char *env[] = {f->path_env, db, NULL};
  Proc p = spawn_wait((char *const *)args->pdata, env, 5000);
  EXPECT(!p.error && p.signal == 0);
  g_free(db);
  g_free(magick);
  g_ptr_array_free(args, true);
  return p.status;
}
static yyjson_val *call_at(yyjson_doc *doc, size_t index) {
  yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
  return root && yyjson_is_arr(root) ? yyjson_arr_get(root, index) : NULL;
}
static yyjson_doc *calls(Fix *f) {
  char *text = read_text(f->log);
  if (!text)
    return yyjson_read("[]", 2, 0);
  GString *arr = g_string_new("[");
  char **lines = g_strsplit(g_strchomp(text), "\n", -1);
  bool first = true;
  for (size_t i = 0; lines[i]; i++) {
    if (!*lines[i])
      continue;
    if (!first)
      g_string_append_c(arr, ',');
    first = false;
    g_string_append(arr, lines[i]);
  }
  g_string_append_c(arr, ']');
  yyjson_doc *doc = yyjson_read(arr->str, arr->len, 0);
  EXPECT(doc);
  g_string_free(arr, true);
  g_strfreev(lines);
  g_free(text);
  return doc;
}
static bool same_call(yyjson_val *call, const char **parts) {
  size_t n = 0;
  while (parts[n])
    n++;
  if (!call || yyjson_arr_size(call) != n)
    return false;
  for (size_t i = 0; i < n; i++) {
    yyjson_val *item = yyjson_arr_get(call, i);
    if (!item || !yyjson_is_str(item) || strcmp(yyjson_get_str(item), parts[i]))
      return false;
  }
  return true;
}
static void expect_calls(Fix *f, const char ***expected, size_t n) {
  yyjson_doc *doc = calls(f);
  yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(root && yyjson_arr_size(root) == n);
  for (size_t i = 0; i < n; i++)
    EXPECT(same_call(call_at(doc, i), expected[i]));
  yyjson_doc_free(doc);
}
static void expect_no_calls(Fix *f) { expect_calls(f, NULL, 0); }
static bool only_magick(Fix *f) {
  yyjson_doc *doc = calls(f);
  yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
  bool ok = root != NULL;
  size_t i, n;
  yyjson_val *item;
  yyjson_arr_foreach(root, i, n, item) {
    yyjson_val *name = yyjson_arr_get(item, 0);
    if (!name || strcmp(yyjson_get_str(name), "magick"))
      ok = false;
  }
  yyjson_doc_free(doc);
  return ok;
}
static void old_live(Fix *f) {
  char *dir = g_path_get_dirname(f->live);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  write_text(f->live, "old image");
}
static void set_source(Fix *f, const char *json_value) {
  char *text = g_strdup_printf(
      "{\"default\":\"default-palette\",\"palettes\":{\"default-palette\":{\"session\":{\"dark\":{\"wallpaper.png\":%s},\"light\":{\"wallpaper.png\":\"nix-theme:light-rose\"}}}}}",
      json_value);
  write_text(f->catalogue, text);
  g_free(text);
}
static char *wrap_space(uint32_t cp, const char *inner) {
  char u[4];
  size_t n = utf8(cp, u);
  GString *s = g_string_new(NULL);
  g_string_append_len(s, u, (gssize)n);
  g_string_append(s, inner);
  g_string_append_len(s, u, (gssize)n);
  return g_string_free(s, false);
}
static char *link_target(const char *path) {
  char buf[4096];
  ssize_t n = readlink(path, buf, sizeof buf - 1);
  if (n < 0)
    return NULL;
  buf[n] = 0;
  return g_strdup(buf);
}
int main(void) {
  binary = sibling("WALLPAPER_CYCLE_BIN", "wallpaper-cycle");
  fake_bin = sibling(NULL, "wallpaper-fake");
  legacy = getenv("WALLPAPER_CYCLE_LEGACY");
  if (!binary || !fake_bin) {
    fprintf(stderr, "Set WALLPAPER_CYCLE_BIN to the candidate executable\n");
    return 1;
  }
  begin("lazily builds exactly the selected theme, converts its next image and writes private current state");
  {
    Fix *f = fixture(NULL, NULL);
    EXPECT(run_dir(f, "next") == 0);
    char *image = g_build_filename(f->theme, "backgrounds", "b.png", NULL);
    char *tmp = g_build_filename(f->state, "active", ".wallpaper-live.png.tmp", NULL);
    char *png = g_strdup_printf("PNG:%s", tmp);
    const char *nix[] = {"nix", "build", "--out-link", f->link,
                         "/flake with spaces;no-shell#theme-rose", NULL};
    const char *mag[] = {"magick", image, png, NULL};
    const char **expected[] = {nix, mag};
    expect_calls(f, expected, 2);
    char *current = read_text(f->current);
    char *live = read_text(f->live);
    struct stat st;
    EXPECT(current && !strcmp(current, "b.png\n"));
    EXPECT(live && !strcmp(live, "png:b.png"));
    EXPECT(stat(f->current, &st) == 0 && (st.st_mode & 0777) == 0600);
    EXPECT(!g_file_test(tmp, G_FILE_TEST_EXISTS));
    g_free(current);
    g_free(live);
    g_free(png);
    g_free(tmp);
    g_free(image);
    fix_free(f);
  }
  begin("next/previous use current order and wrap without rebuilding the existing package");
  {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    write_text(f->current, "c.png\n");
    EXPECT(run_dir(f, "next") == 0);
    char *current = read_text(f->current);
    EXPECT(current && !strcmp(current, "a.png\n"));
    g_free(current);
    EXPECT(run_dir(f, "previous") == 0);
    current = read_text(f->current);
    EXPECT(current && !strcmp(current, "c.png\n"));
    EXPECT(only_magick(f));
    g_free(current);
    fix_free(f);
  }
  begin("missing or stale current uses preferred; absent preferred starts from first entry");
  const char *prefs[] = {"b.png", "b.png", "absent"};
  const char *currents[] = {"", "deleted.png", ""};
  const char *dirs[] = {"next", "previous", "next"};
  const char *chosen[] = {"c.png", "a.png", "b.png"};
  for (size_t i = 0; i < 3; i++) {
    Fix *f = fixture(NULL, prefs[i]);
    prepare(f);
    write_text(f->current, currents[i]);
    EXPECT(run_dir(f, dirs[i]) == 0);
    char *current = read_text(f->current);
    char *expect = g_strdup_printf("%s\n", chosen[i]);
    EXPECT(current && !strcmp(current, expect));
    g_free(expect);
    g_free(current);
    fix_free(f);
  }
  begin("duplicate entries retain the first current/preferred index and original ordering");
  {
    Fix *f = fixture("[\"a.png\",\"b.png\",\"a.png\",\"c.png\"]", NULL);
    prepare(f);
    EXPECT(run_dir(f, "next") == 0);
    char *current = read_text(f->current);
    EXPECT(current && !strcmp(current, "b.png\n"));
    g_free(current);
    fix_free(f);
  }
  begin("empty, single or path-traversal-only lists do not rotate or create live files");
  const char *lists[] = {"[]", "[\"a.png\"]",
                         "[\"../outside.png\",\".\",\"..\",1,null,true,\"a.png\"]"};
  for (size_t i = 0; i < 3; i++) {
    Fix *f = fixture(lists[i], NULL);
    prepare(f);
    EXPECT(run_dir(f, "next") == 3);
    expect_no_calls(f);
    EXPECT(!g_file_test(f->live, G_FILE_TEST_EXISTS));
    fix_free(f);
  }
  begin("unsafe entries are filtered and valid Unicode basenames rotate");
  {
    Fix *f = fixture("[\"../a\",{},null,\"a.png\",\"雪.png\",\"b.png\"]", NULL);
    prepare(f);
    EXPECT(run_dir(f, "next") == 0);
    char *current = read_text(f->current);
    EXPECT(current && !strcmp(current, "雪.png\n"));
    g_free(current);
    fix_free(f);
  }
  begin("empty/default selection, Unicode whitespace and invalid mode select the configured default dark theme");
  char *selections[3];
  selections[0] = g_strdup("");
  selections[1] = g_strdup("default\n");
  {
    char u[8];
    size_t n = utf8(0x2007, u);
    GString *s = g_string_new_len(u, (gssize)n);
    g_string_append(s, "default");
    n = utf8(0x1c, u);
    g_string_append_len(s, u, (gssize)n);
    n = utf8(0x85, u);
    g_string_append_len(s, u, (gssize)n);
    selections[2] = g_string_free(s, false);
  }
  for (size_t i = 0; i < 3; i++) {
    Fix *f = fixture(NULL, NULL);
    char *sel = g_build_filename(f->state, "selection", NULL);
    write_text(sel, selections[i]);
    char *mode = g_build_filename(f->state, "mode", NULL);
    write_text(mode, "invalid");
    EXPECT(run_dir(f, "next") == 0);
    yyjson_doc *doc = calls(f);
    yyjson_val *target = yyjson_arr_get(call_at(doc, 0), 4);
    EXPECT(target && !strcmp(yyjson_get_str(target),
                             "/flake with spaces;no-shell#theme-rose"));
    yyjson_doc_free(doc);
    g_free(mode);
    g_free(sel);
    fix_free(f);
    g_free(selections[i]);
  }
  begin("every Python whitespace character is stripped from selection, mode and current basename");
  for (size_t i = 0; i < G_N_ELEMENTS(py_spaces); i++) {
    Fix *f = fixture(NULL, NULL);
    char *sel = g_build_filename(f->state, "selection", NULL);
    char *mode = g_build_filename(f->state, "mode", NULL);
    char *wrapped_sel = wrap_space(py_spaces[i], "default-palette");
    char *wrapped_mode = wrap_space(py_spaces[i], "light");
    char *wrapped_cur = wrap_space(py_spaces[i], "b.png");
    write_text(sel, wrapped_sel);
    write_text(mode, wrapped_mode);
    write_text(f->current, wrapped_cur);
    EXPECT(run_dir(f, "next") == 0);
    yyjson_doc *doc = calls(f);
    yyjson_val *target = yyjson_arr_get(call_at(doc, 0), 4);
    EXPECT(target && !strcmp(yyjson_get_str(target),
                             "/flake with spaces;no-shell#theme-light-rose"));
    char *current = read_text(f->current);
    EXPECT(current && !strcmp(current, "c.png\n"));
    yyjson_doc_free(doc);
    g_free(current);
    g_free(wrapped_cur);
    g_free(wrapped_mode);
    g_free(wrapped_sel);
    g_free(mode);
    g_free(sel);
    fix_free(f);
  }
  begin("explicit palette and light mode resolve independently");
  {
    Fix *f = fixture(NULL, NULL);
    char *sel = g_build_filename(f->state, "selection", NULL);
    char *mode = g_build_filename(f->state, "mode", NULL);
    write_text(sel, "default-palette\n");
    write_text(mode, "light\n");
    EXPECT(run_dir(f, "next") == 0);
    yyjson_doc *doc = calls(f);
    yyjson_val *target = yyjson_arr_get(call_at(doc, 0), 4);
    EXPECT(target && !strcmp(yyjson_get_str(target),
                             "/flake with spaces;no-shell#theme-light-rose"));
    yyjson_doc_free(doc);
    g_free(mode);
    g_free(sel);
    fix_free(f);
  }
  begin("overrides, missing palettes and unsafe theme references discard the rotation root and live/current files");
  const char *sources[] = {"\"/hand-set.png\"", "null", "\"nix-theme:\"",
                           "\"nix-theme:a/b\"", "\"nix-theme:a..b\"",
                           "\"nix-theme:a b\""};
  for (size_t i = 0; i < 6; i++) {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    old_live(f);
    write_text(f->current, "b.png");
    set_source(f, sources[i]);
    EXPECT(run_dir(f, "next") == 3);
    expect_no_calls(f);
    EXPECT(!g_file_test(f->link, G_FILE_TEST_EXISTS));
    EXPECT(!g_file_test(f->current, G_FILE_TEST_EXISTS));
    EXPECT(!g_file_test(f->live, G_FILE_TEST_EXISTS));
    fix_free(f);
  }
  {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    char *sel = g_build_filename(f->state, "selection", NULL);
    write_text(sel, "not-a-palette");
    EXPECT(run_dir(f, "next") == 3);
    EXPECT(!g_file_test(f->link, G_FILE_TEST_EXISTS));
    g_free(sel);
    fix_free(f);
  }
  begin("missing/malformed catalogue leaves existing state intact; invalid object types fail");
  const char *bad_cats[] = {"bad json", "null"};
  for (size_t i = 0; i < 2; i++) {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    old_live(f);
    write_text(f->catalogue, bad_cats[i]);
    EXPECT(run_dir(f, "next") == 3);
    char *live = read_text(f->live);
    char *target = link_target(f->link);
    EXPECT(live && !strcmp(live, "old image"));
    EXPECT(target && !strcmp(target, f->theme));
    g_free(target);
    g_free(live);
    fix_free(f);
  }
  {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    unlink(f->catalogue);
    EXPECT(run_dir(f, "next") == 3);
    char *target = link_target(f->link);
    EXPECT(target && !strcmp(target, f->theme));
    g_free(target);
    write_text(f->catalogue, "[]");
    EXPECT(run_dir(f, "next") == 1);
    fix_free(f);
  }
  begin("malformed nested catalogue objects fail without deleting rotation state");
  const char *nested[] = {
      "{\"default\":\"x\",\"palettes\":[\"invalid\"]}",
      "{\"default\":\"x\",\"palettes\":{\"x\":1}}",
      "{\"default\":\"x\",\"palettes\":{\"x\":{\"session\":\"invalid\"}}}",
      "{\"default\":\"x\",\"palettes\":{\"x\":{\"session\":{\"dark\":[\"invalid\"]}}}}"};
  for (size_t i = 0; i < 4; i++) {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    old_live(f);
    write_text(f->catalogue, nested[i]);
    EXPECT(run_dir(f, "next") == 1);
    char *live = read_text(f->live);
    char *target = link_target(f->link);
    EXPECT(live && !strcmp(live, "old image"));
    EXPECT(target && !strcmp(target, f->theme));
    expect_no_calls(f);
    g_free(target);
    g_free(live);
    fix_free(f);
  }
  begin("empty nested catalogue values behave as missing theme references");
  const char *palettes[] = {"null", "false", "0", "\"\"", "[]"};
  for (size_t i = 0; i < 5; i++) {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    old_live(f);
    char *text = g_strdup_printf("{\"default\":\"x\",\"palettes\":%s}", palettes[i]);
    write_text(f->catalogue, text);
    EXPECT(run_dir(f, "next") == 3);
    EXPECT(!g_file_test(f->live, G_FILE_TEST_EXISTS));
    EXPECT(!g_file_test(f->link, G_FILE_TEST_EXISTS));
    g_free(text);
    fix_free(f);
  }
  begin("missing or malformed theme metadata never converts an image");
  for (int missing = 1; missing >= 0; missing--) {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    char *path = g_build_filename(f->theme, "theme.json", NULL);
    if (missing)
      unlink(path);
    else
      write_text(path, "{broken");
    EXPECT(run_dir(f, "next") == 3);
    expect_no_calls(f);
    g_free(path);
    fix_free(f);
  }
  begin("missing files and symlinks escaping the background directory do not rotate");
  for (int escape = 0; escape < 2; escape++) {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    char *image = g_build_filename(f->theme, "backgrounds", "b.png", NULL);
    unlink(image);
    if (escape) {
      char *outside = g_build_filename(f->root, "outside.png", NULL);
      write_text(outside, "outside");
      EXPECT(symlink(outside, image) == 0);
      g_free(outside);
    }
    EXPECT(run_dir(f, "next") == 3);
    expect_no_calls(f);
    g_free(image);
    fix_free(f);
  }
  begin("a symlink remaining inside the canonical background directory is accepted");
  {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    char *image = g_build_filename(f->theme, "backgrounds", "b.png", NULL);
    unlink(image);
    EXPECT(symlink("c.png", image) == 0);
    EXPECT(run_dir(f, "next") == 0);
    yyjson_doc *doc = calls(f);
    char *resolved = g_build_filename(f->theme, "backgrounds", "c.png", NULL);
    yyjson_val *arg = yyjson_arr_get(call_at(doc, 0), 1);
    EXPECT(arg && !strcmp(yyjson_get_str(arg), resolved));
    char *current = read_text(f->current);
    EXPECT(current && !strcmp(current, "b.png\n"));
    yyjson_doc_free(doc);
    g_free(current);
    g_free(resolved);
    g_free(image);
    fix_free(f);
  }
  begin("broken package links are rebuilt; a directory package works without a link");
  {
    Fix *f = fixture(NULL, NULL);
    char *missing = g_build_filename(f->root, "missing", NULL);
    EXPECT(symlink(missing, f->link) == 0);
    EXPECT(run_dir(f, "next") == 0);
    yyjson_doc *doc = calls(f);
    yyjson_val *name = yyjson_arr_get(call_at(doc, 0), 0);
    EXPECT(name && !strcmp(yyjson_get_str(name), "nix"));
    yyjson_doc_free(doc);
    g_free(missing);
    fix_free(f);
  }
  {
    Fix *f = fixture(NULL, NULL);
    char *rm[] = {"rm", "-rf", "--", f->theme, NULL};
    spawn_wait(rm, NULL, 5000);
    char *backgrounds = g_build_filename(f->link, "backgrounds", NULL);
    g_mkdir_with_parents(backgrounds, 0755);
    char *a = g_build_filename(backgrounds, "a.png", NULL);
    char *b = g_build_filename(backgrounds, "b.png", NULL);
    write_text(a, "a");
    write_text(b, "b");
    char *meta = g_build_filename(f->link, "theme.json", NULL);
    write_text(meta, "{\"backgrounds\":[\"a.png\",\"b.png\"],\"preferred\":\"a.png\"}");
    EXPECT(run_dir(f, "next") == 0);
    yyjson_doc *doc = calls(f);
    yyjson_val *name = yyjson_arr_get(call_at(doc, 0), 0);
    EXPECT(name && !strcmp(yyjson_get_str(name), "magick"));
    yyjson_doc_free(doc);
    g_free(meta);
    g_free(b);
    g_free(a);
    g_free(backgrounds);
    fix_free(f);
  }
  begin("a concurrent root or current-file change abandons the converted temporary image");
  const char *races[] = {"link", "current"};
  for (size_t i = 0; i < 2; i++) {
    Fix *f = fixture(NULL, NULL);
    prepare(f);
    old_live(f);
    g_free(f->race);
    f->race = g_strdup(races[i]);
    EXPECT(run_dir(f, "next") == 3);
    char *live = read_text(f->live);
    char *tmp = g_build_filename(f->state, "active", ".wallpaper-live.png.tmp", NULL);
    EXPECT(live && !strcmp(live, "old image"));
    EXPECT(!g_file_test(tmp, G_FILE_TEST_EXISTS));
    if (!strcmp(races[i], "current")) {
      char *current = read_text(f->current);
      EXPECT(current && !strcmp(current, "other.png\n"));
      g_free(current);
    }
    g_free(tmp);
    g_free(live);
    fix_free(f);
  }
  begin("build/conversion failure propagates without replacing existing live/current state");
  const char *fails[] = {"nix", "magick"};
  for (size_t i = 0; i < 2; i++) {
    Fix *f = fixture(NULL, NULL);
    old_live(f);
    write_text(f->current, "a.png\n");
    g_free(f->fail);
    f->fail = g_strdup(fails[i]);
    EXPECT(run_dir(f, "next") == 1);
    char *live = read_text(f->live);
    char *current = read_text(f->current);
    EXPECT(live && !strcmp(live, "old image"));
    EXPECT(current && !strcmp(current, "a.png\n"));
    g_free(current);
    g_free(live);
    fix_free(f);
  }
  begin("required arguments and invalid directions retain usage exit status");
  {
    Fix *f = fixture(NULL, NULL);
    EXPECT(run_dir(f, "other") == 2);
    char *argv[] = {legacy ? "python3" : binary, legacy ? (char *)legacy : NULL, NULL};
    if (!legacy)
      argv[1] = NULL;
    Proc p = spawn_wait(argv, NULL, 5000);
    EXPECT(!p.error && p.status == 2);
    fix_free(f);
  }
  g_free(binary);
  g_free(fake_bin);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("ok");
  return 0;
}
