#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
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
#include <yyjson.h>

static int failures;
static char *fake_bin;

static void fail_msg(const char *file, int line, const char *fmt, ...) {
  fprintf(stderr, "FAIL %s:%d: ", file, line);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  failures++;
}
#define FAIL(...) fail_msg(__FILE__, __LINE__, __VA_ARGS__)
#define EXPECT(cond, ...)                                                      \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(__VA_ARGS__);                                                       \
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
  char *out = xmalloc(na + slash + nb + 1);
  memcpy(out, a, na);
  if (slash)
    out[na++] = '/';
  memcpy(out + na, b, nb + 1);
  return out;
}

static void write_all_fd(int fd, const void *data, size_t n) {
  const char *p = data;
  size_t off = 0;
  while (off < n) {
    ssize_t wrote = write(fd, p + off, n - off);
    if (wrote < 0) {
      if (errno == EINTR)
        continue;
      perror("write");
      exit(1);
    }
    off += (size_t)wrote;
  }
}

static void write_file(const char *path, const void *data, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror(path);
    exit(1);
  }
  write_all_fd(fd, data, n);
  close(fd);
}

static void write_str(const char *path, const char *text) {
  write_file(path, text, strlen(text));
}

static char *read_file(const char *path, size_t *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL;
  size_t cap = 4096, n = 0;
  char *buf = xmalloc(cap);
  for (;;) {
    if (n == cap) {
      cap *= 2;
      char *next = realloc(buf, cap);
      if (!next) {
        perror("realloc");
        exit(1);
      }
      buf = next;
    }
    ssize_t got = read(fd, buf + n, cap - n);
    if (got < 0) {
      if (errno == EINTR)
        continue;
      perror("read");
      exit(1);
    }
    if (got == 0)
      break;
    n += (size_t)got;
  }
  close(fd);
  *len = n;
  return buf;
}

static bool exists_stat(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static bool contains_mem(const char *hay, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (!m || n < m)
    return false;
  for (size_t i = 0; i + m <= n; i++)
    if (!memcmp(hay + i, needle, m))
      return true;
  return false;
}

static void rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st) < 0)
    return;
  if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
    DIR *dir = opendir(path);
    if (dir) {
      struct dirent *ent;
      while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
          continue;
        char *child = join_path(path, ent->d_name);
        rm_rf(child);
        free(child);
      }
      closedir(dir);
    }
    rmdir(path);
  } else
    unlink(path);
}

static char *sibling(const char *name) {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0)
    return NULL;
  buf[n] = 0;
  char *slash = strrchr(buf, '/');
  if (!slash)
    return NULL;
  *slash = 0;
  return join_path(buf, name);
}

typedef struct {
  char *root, *state, *config, *bin, *log, *catalogue, *greeter, *proc,
      *wallpaper;
} Fix;

static void fix_free(Fix *f) {
  if (!f)
    return;
  if (f->root)
    rm_rf(f->root);
  free(f->root);
  free(f->state);
  free(f->config);
  free(f->bin);
  free(f->log);
  free(f->catalogue);
  free(f->greeter);
  free(f->proc);
  free(f->wallpaper);
  free(f);
}

static const char *palette_ids[] = {"tokyo-night", "rose-pine", "osaka-jade"};
static const char *palette_labels[] = {"Tokyo Night", "Rosé Pine",
                                       "Osaka Jade"};
static const char *file_names[] = {
    "host-palettes.json", "herdr.toml",     "btop.theme",
    "ghostty-dark",       "ghostty-light",  "host-dark.json",
    "host-light.json",    "delta",          "bat-dark",
    "bat-light"};
static const char *session_names[] = {"hyprland.conf", "gtk.css", "fuzzel.ini",
                                      "walker.css"};
static const char *commands[] = {"fuzzel",    "gdbus",    "herdr",  "notify-send",
                                 "nix",       "gsettings", "hyprctl", "systemctl",
                                 "makoctl",   "walker"};

static char *session_body(const char *id, const char *mode, const char *name) {
  if (!strcmp(name, "hyprland.conf")) {
    char *text = xmalloc(160);
    snprintf(text, 160,
             "$theme_gtk = custom-%s\n$theme_font = Test Font\n$theme_mono_font "
             "= Test Mono\n$theme_font_size = 15\n$theme_active_border = "
             "rgb(abcdef)\n$theme_rounding = 9\n",
             mode);
    return text;
  }
  if (!strcmp(name, "gtk.css"))
    return xstrdup("/* Shared GTK and Brave colours and fonts */\n");
  if (!strcmp(name, "fuzzel.ini"))
    return xstrdup("[main]\nfont=Test Font:size=14\n[border]\nwidth=3\nradius=7\n");
  size_t n = strlen(id) + 1 + strlen(mode);
  char *text = xmalloc(n + 1);
  memcpy(text, id, strlen(id));
  text[strlen(id)] = ':';
  memcpy(text + strlen(id) + 1, mode, strlen(mode) + 1);
  return text;
}

static void add_colours(yyjson_mut_doc *doc, yyjson_mut_val *parent,
                        const char *mode) {
  yyjson_mut_val *o = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_val(doc, parent, mode, o);
  if (!strcmp(mode, "dark")) {
    yyjson_mut_obj_add_strcpy(doc, o, "base00", "#24273a");
    yyjson_mut_obj_add_strcpy(doc, o, "base02", "#494d64");
    yyjson_mut_obj_add_strcpy(doc, o, "base05", "#cad3f5");
    yyjson_mut_obj_add_strcpy(doc, o, "base07", "#f4dbd6");
    yyjson_mut_obj_add_strcpy(doc, o, "base0D", "#b7bdf8");
  } else {
    yyjson_mut_obj_add_strcpy(doc, o, "base00", "#eeeeee");
    yyjson_mut_obj_add_strcpy(doc, o, "base02", "#bbbbbb");
    yyjson_mut_obj_add_strcpy(doc, o, "base05", "#222222");
    yyjson_mut_obj_add_strcpy(doc, o, "base07", "#111111");
    yyjson_mut_obj_add_strcpy(doc, o, "base0D", "#bbbb00");
  }
}

static Fix *setup(void) {
  char *tmpl = xstrdup("/tmp/theme-menu-fixture-XXXXXX");
  if (!mkdtemp(tmpl)) {
    perror("mkdtemp");
    free(tmpl);
    return NULL;
  }
  Fix *f = calloc(1, sizeof *f);
  if (!f) {
    perror("calloc");
    exit(1);
  }
  f->root = tmpl;
  f->state = join_path(f->root, "state");
  f->config = join_path(f->root, "config");
  f->bin = join_path(f->root, "bin");
  f->log = join_path(f->root, "commands.jsonl");
  f->catalogue = join_path(f->root, "catalogue.json");
  f->greeter = join_path(f->root, "greeter");
  f->proc = join_path(f->root, "proc");
  f->wallpaper = join_path(f->root, "wallpaper");
  for (int i = 0; i < 4; i++) {
    const char *part = (const char *[]){f->bin, f->config, f->proc, f->wallpaper}[i];
    if (mkdir(part, 0755) < 0) {
      perror(part);
      fix_free(f);
      return NULL;
    }
  }
  write_str(f->log, "");
  unsigned char png[] = {0, 1, 2, 255};
  char *png_path = join_path(f->wallpaper, "wallpaper.png");
  write_file(png_path, png, sizeof png);
  free(png_path);
  for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
    char *dest = join_path(f->bin, commands[i]);
    if (symlink(fake_bin, dest) < 0) {
      perror(dest);
      free(dest);
      fix_free(f);
      return NULL;
    }
    free(dest);
  }
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_strcpy(doc, root, "default", "tokyo-night");
  yyjson_mut_val *palettes = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_val(doc, root, "palettes", palettes);
  for (int i = 0; i < 3; i++) {
    char *source = join_path(f->root, palette_ids[i]);
    mkdir(source, 0755);
    yyjson_mut_val *palette = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, palettes, palette_ids[i], palette);
    yyjson_mut_obj_add_strcpy(doc, palette, "label", palette_labels[i]);
    yyjson_mut_val *files = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, palette, "files", files);
    for (size_t n = 0; n < sizeof file_names / sizeof file_names[0]; n++) {
      char *path = join_path(source, file_names[n]);
      size_t a = strlen(palette_ids[i]), b = strlen(file_names[n]);
      char *body = xmalloc(a + b + 3);
      memcpy(body, palette_ids[i], a);
      body[a] = ':';
      memcpy(body + a + 1, file_names[n], b);
      body[a + 1 + b] = '\n';
      body[a + 2 + b] = 0;
      write_str(path, body);
      free(body);
      yyjson_mut_obj_add_strcpy(doc, files, file_names[n], path);
      free(path);
    }
    yyjson_mut_val *session = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, palette, "session", session);
    for (int m = 0; m < 2; m++) {
      const char *mode = m ? "dark" : "light";
      yyjson_mut_val *assets = yyjson_mut_obj(doc);
      yyjson_mut_obj_add_val(doc, session, mode, assets);
      for (size_t n = 0; n < sizeof session_names / sizeof session_names[0]; n++) {
        char *leaf = xmalloc(strlen(mode) + strlen(session_names[n]) + 2);
        snprintf(leaf, strlen(mode) + strlen(session_names[n]) + 2, "%s-%s",
                 mode, session_names[n]);
        char *path = join_path(source, leaf);
        char *body = session_body(palette_ids[i], mode, session_names[n]);
        write_str(path, body);
        free(body);
        yyjson_mut_obj_add_strcpy(doc, assets, session_names[n], path);
        free(path);
        free(leaf);
      }
    }
    yyjson_mut_val *nvim = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, palette, "nvim", nvim);
    add_colours(doc, nvim, "dark");
    add_colours(doc, nvim, "light");
    free(source);
  }
  yyjson_mut_val *appearance = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_val(doc, root, "appearance", appearance);
  yyjson_mut_obj_add_strcpy(doc, appearance, "monoFont", "Fallback Mono");
  yyjson_mut_obj_add_int(doc, appearance, "fontSize", 13);
  yyjson_mut_obj_add_int(doc, appearance, "borderSize", 2);
  yyjson_mut_obj_add_int(doc, appearance, "rounding", 10);
  yyjson_mut_val *launcher = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_val(doc, root, "launcher", launcher);
  yyjson_mut_obj_add_int(doc, launcher, "lines", 4);
  yyjson_mut_obj_add_int(doc, launcher, "width", 45);
  yyjson_mut_obj_add_strcpy(doc, launcher, "anchor", "center");
  yyjson_mut_obj_add_strcpy(doc, launcher, "layer", "overlay");
  yyjson_mut_obj_add_strcpy(doc, launcher, "matchMode", "fzf");
  size_t n = 0;
  char *json = yyjson_mut_write(doc, 0, &n);
  yyjson_mut_doc_free(doc);
  if (!json) {
    fix_free(f);
    return NULL;
  }
  write_file(f->catalogue, json, n);
  free(json);
  return f;
}

static yyjson_mut_doc *load_catalogue(Fix *f) {
  size_t n = 0;
  char *text = read_file(f->catalogue, &n);
  if (!text)
    return NULL;
  yyjson_doc *doc = yyjson_read(text, n, 0);
  free(text);
  if (!doc)
    return NULL;
  yyjson_mut_doc *mut = yyjson_doc_mut_copy(doc, NULL);
  yyjson_doc_free(doc);
  return mut;
}

static void save_catalogue(Fix *f, yyjson_mut_doc *doc) {
  size_t n = 0;
  char *text = yyjson_mut_write(doc, 0, &n);
  if (!text) {
    yyjson_mut_doc_free(doc);
    return;
  }
  write_file(f->catalogue, text, n);
  free(text);
  yyjson_mut_doc_free(doc);
}

static void set_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key,
                    const char *value) {
  yyjson_mut_obj_remove_key(obj, key);
  yyjson_mut_obj_add_strcpy(doc, obj, key, value);
}

typedef struct {
  int status;
  char *out, *err;
  size_t out_len, err_len;
  bool spawn_error, timed_out;
  pid_t pid;
  int out_fd, err_fd;
  bool running;
} Result;

static void result_free(Result *r) {
  free(r->out);
  free(r->err);
  r->out = r->err = NULL;
}

static void append_buf(char **buf, size_t *len, size_t *cap, const char *data,
                       size_t n) {
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (*len + n + 1 > next)
      next *= 2;
    char *grown = realloc(*buf, next);
    if (!grown) {
      perror("realloc");
      exit(1);
    }
    *buf = grown;
    *cap = next;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
  (*buf)[*len] = 0;
}

static void read_ready(int fd, char **buf, size_t *len, size_t *cap, bool *eof) {
  char tmp[4096];
  for (;;) {
    ssize_t got = read(fd, tmp, sizeof tmp);
    if (got < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN)
        return;
      perror("read");
      exit(1);
    }
    if (got == 0) {
      *eof = true;
      return;
    }
    append_buf(buf, len, cap, tmp, (size_t)got);
  }
}

static Result start_proc(char **argv, char **env, const char *input,
                         int timeout_ms) {
  (void)timeout_ms;
  Result r = {.status = -1, .out_fd = -1, .err_fd = -1};
  int in[2], out[2], err[2];
  if (pipe(in) < 0 || pipe(out) < 0 || pipe(err) < 0) {
    r.spawn_error = true;
    return r;
  }
  pid_t pid = fork();
  if (pid < 0) {
    r.spawn_error = true;
    return r;
  }
  if (pid == 0) {
    dup2(in[0], STDIN_FILENO);
    dup2(out[1], STDOUT_FILENO);
    dup2(err[1], STDERR_FILENO);
    close(in[0]);
    close(in[1]);
    close(out[0]);
    close(out[1]);
    close(err[0]);
    close(err[1]);
    execve(argv[0], argv, env);
    _exit(127);
  }
  close(in[0]);
  close(out[1]);
  close(err[1]);
  if (input)
    write_all_fd(in[1], input, strlen(input));
  close(in[1]);
  int flags = fcntl(out[0], F_GETFL);
  fcntl(out[0], F_SETFL, flags | O_NONBLOCK);
  flags = fcntl(err[0], F_GETFL);
  fcntl(err[0], F_SETFL, flags | O_NONBLOCK);
  r.pid = pid;
  r.out_fd = out[0];
  r.err_fd = err[0];
  r.running = true;
  r.out = xmalloc(1);
  r.err = xmalloc(1);
  r.out[0] = r.err[0] = 0;
  return r;
}

static void finish_proc(Result *r, int timeout_ms) {
  if (!r->running)
    return;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  bool out_eof = false, err_eof = false;
  size_t out_cap = 1, err_cap = 1;
  for (;;) {
    struct pollfd pf[2] = {{r->out_fd, POLLIN, 0}, {r->err_fd, POLLIN, 0}};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                        (now.tv_nsec - start.tv_nsec) / 1000000);
    int remain = timeout_ms - elapsed;
    if (remain < 0)
      remain = 0;
    int ready = poll(pf, 2, remain);
    if (ready > 0) {
      if (pf[0].revents)
        read_ready(r->out_fd, &r->out, &r->out_len, &out_cap, &out_eof);
      if (pf[1].revents)
        read_ready(r->err_fd, &r->err, &r->err_len, &err_cap, &err_eof);
    }
    int status = 0;
    pid_t got = waitpid(r->pid, &status, WNOHANG);
    if (got == r->pid) {
      read_ready(r->out_fd, &r->out, &r->out_len, &out_cap, &out_eof);
      read_ready(r->err_fd, &r->err, &r->err_len, &err_cap, &err_eof);
      if (WIFEXITED(status))
        r->status = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
        r->status = -WTERMSIG(status);
      r->running = false;
      break;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                    (now.tv_nsec - start.tv_nsec) / 1000000);
    if (elapsed >= timeout_ms) {
      kill(r->pid, SIGKILL);
      waitpid(r->pid, &status, 0);
      r->timed_out = true;
      r->status = -SIGKILL;
      r->running = false;
      break;
    }
    if (ready < 0 && errno != EINTR) {
      r->spawn_error = true;
      r->running = false;
      break;
    }
  }
  close(r->out_fd);
  close(r->err_fd);
  r->out_fd = r->err_fd = -1;
}

static char **base_env(void) {
  extern char **environ;
  size_t count = 0;
  while (environ[count])
    count++;
  char **env = xmalloc((count + 24) * sizeof *env);
  size_t n = 0;
  for (size_t i = 0; i < count; i++) {
    if (!strncmp(environ[i], "THEME_MENU_PUBLISH=", 19))
      continue;
    env[n++] = xstrdup(environ[i]);
  }
  env[n] = NULL;
  return env;
}

static void env_set(char ***env, const char *key, const char *value) {
  size_t klen = strlen(key);
  size_t count = 0;
  while ((*env)[count])
    count++;
  for (size_t i = 0; i < count; i++) {
    if (!strncmp((*env)[i], key, klen) && (*env)[i][klen] == '=') {
      free((*env)[i]);
      if (!value) {
        memmove(*env + i, *env + i + 1, (count - i) * sizeof **env);
        return;
      }
      size_t n = klen + 1 + strlen(value);
      char *item = xmalloc(n + 1);
      memcpy(item, key, klen);
      item[klen] = '=';
      memcpy(item + klen + 1, value, strlen(value) + 1);
      (*env)[i] = item;
      return;
    }
  }
  if (!value)
    return;
  char **next = realloc(*env, (count + 2) * sizeof *next);
  if (!next) {
    perror("realloc");
    exit(1);
  }
  *env = next;
  size_t n = klen + 1 + strlen(value);
  char *item = xmalloc(n + 1);
  memcpy(item, key, klen);
  item[klen] = '=';
  memcpy(item + klen + 1, value, strlen(value) + 1);
  (*env)[count] = item;
  (*env)[count + 1] = NULL;
}

static char **fixture_env(Fix *f, const char **extra_keys, const char **extra_vals,
                          int nextra) {
  char **env = base_env();
  env_set(&env, "PATH", f->bin);
  env_set(&env, "XDG_CONFIG_HOME", f->config);
  env_set(&env, "XDG_CURRENT_DESKTOP", "");
  env_set(&env, "HYPRLAND_INSTANCE_SIGNATURE", "");
  env_set(&env, "THEME_MENU_PUBLISH", NULL);
  env_set(&env, "TM_LOG", f->log);
  env_set(&env, "TM_STATE", f->state);
  env_set(&env, "TM_WALLPAPER", f->wallpaper);
  env_set(&env, "TM_COMMANDS", "{}");
  for (int i = 0; i < nextra; i++)
    env_set(&env, extra_keys[i], extra_vals[i]);
  return env;
}

static void free_env(char **env) {
  for (size_t i = 0; env[i]; i++)
    free(env[i]);
  free(env);
}

static void free_argv(char **argv) {
  for (int i = 0; argv[i]; i++)
    free(argv[i]);
  free(argv);
}

static char **menu_argv(Fix *f, const char **extra, int nextra, bool fixture,
                        int failure) {
  char **argv = xmalloc((size_t)(nextra + 12) * sizeof *argv);
  int n = 0;
  if (fixture) {
    argv[n++] = xstrdup(getenv("THEME_MENU_FIXTURE"));
    argv[n++] = xstrdup("run");
    argv[n++] = xstrdup(f->greeter);
    argv[n++] = xstrdup(f->proc);
    char num[32];
    snprintf(num, sizeof num, "%d", failure);
    argv[n++] = xstrdup(num);
  } else
    argv[n++] = xstrdup(getenv("THEME_MENU_BIN"));
  argv[n++] = xstrdup("--catalogue");
  argv[n++] = xstrdup(f->catalogue);
  argv[n++] = xstrdup("--state");
  argv[n++] = xstrdup(f->state);
  for (int i = 0; i < nextra; i++)
    argv[n++] = xstrdup(extra[i]);
  argv[n] = NULL;
  return argv;
}

static Result run_menu(Fix *f, const char **extra, int nextra, const char **keys,
                       const char **vals, int nextra_env, bool fixture,
                       int failure) {
  char **argv = menu_argv(f, extra, nextra, fixture, failure);
  char **env = fixture_env(f, keys, vals, nextra_env);
  Result r = start_proc(argv, env, "", 20000);
  finish_proc(&r, 20000);
  free_argv(argv);
  free_env(env);
  return r;
}

static Result run(Fix *f, const char **extra, int nextra, const char **keys,
                  const char **vals, int nenv) {
  return run_menu(f, extra, nextra, keys, vals, nenv, false, -1);
}

static Result run_fixture(Fix *f, const char **extra, int nextra,
                          const char **keys, const char **vals, int nenv,
                          int failure) {
  return run_menu(f, extra, nextra, keys, vals, nenv, true, failure);
}

typedef struct {
  char *rel;
  char *data;
  size_t len;
} SnapItem;

typedef struct {
  SnapItem *items;
  size_t len;
} Snap;

static void snap_free(Snap *s) {
  for (size_t i = 0; i < s->len; i++) {
    free(s->items[i].rel);
    free(s->items[i].data);
  }
  free(s->items);
  s->items = NULL;
  s->len = 0;
}

static void snap_add(Snap *s, const char *rel, const char *data, size_t n) {
  s->items = realloc(s->items, (s->len + 1) * sizeof *s->items);
  if (!s->items) {
    perror("realloc");
    exit(1);
  }
  s->items[s->len].rel = xstrdup(rel);
  s->items[s->len].data = xmalloc(n + 1);
  memcpy(s->items[s->len].data, data, n);
  s->items[s->len].data[n] = 0;
  s->items[s->len].len = n;
  s->len++;
}

static void snap_walk(const char *root, const char *rel, Snap *s) {
  char *dir = rel && *rel ? join_path(root, rel) : xstrdup(root);
  DIR *handle = opendir(dir);
  free(dir);
  if (!handle)
    return;
  struct dirent *ent;
  while ((ent = readdir(handle))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    char *child = rel && *rel ? join_path(rel, ent->d_name) : xstrdup(ent->d_name);
    if (!strcmp(child, "lock")) {
      free(child);
      continue;
    }
    char *full = join_path(root, child);
    struct stat st;
    if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
      snap_walk(root, child, s);
    else if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
      size_t n = 0;
      char *data = read_file(full, &n);
      if (data)
        snap_add(s, child, data, n);
      free(data);
    }
    free(full);
    free(child);
  }
  closedir(handle);
}

static Snap snapshot(Fix *f) {
  Snap s = {0};
  if (exists_stat(f->state))
    snap_walk(f->state, "", &s);
  return s;
}

static const SnapItem *snap_get(const Snap *s, const char *rel) {
  for (size_t i = 0; i < s->len; i++)
    if (!strcmp(s->items[i].rel, rel))
      return &s->items[i];
  return NULL;
}

static bool snap_equal(const Snap *a, const Snap *b) {
  if (a->len != b->len)
    return false;
  for (size_t i = 0; i < a->len; i++) {
    const SnapItem *other = snap_get(b, a->items[i].rel);
    if (!other || other->len != a->items[i].len ||
        memcmp(other->data, a->items[i].data, other->len))
      return false;
  }
  return true;
}

static int changed_count(const Snap *before, const Snap *after) {
  int n = 0;
  for (size_t i = 0; i < after->len; i++) {
    const SnapItem *old = snap_get(before, after->items[i].rel);
    if (!old || old->len != after->items[i].len ||
        memcmp(old->data, after->items[i].data, old->len))
      n++;
  }
  return n;
}

typedef struct {
  char *name;
  char **args;
  int argc;
  char *input, *settings;
} Log;

static void logs_free(Log *logs, int n) {
  for (int i = 0; i < n; i++) {
    free(logs[i].name);
    free(logs[i].input);
    free(logs[i].settings);
    for (int a = 0; a < logs[i].argc; a++)
      free(logs[i].args[a]);
    free(logs[i].args);
  }
  free(logs);
}

static Log *read_logs(Fix *f, int *count) {
  *count = 0;
  size_t n = 0;
  char *text = read_file(f->log, &n);
  if (!text)
    return NULL;
  Log *logs = NULL;
  size_t at = 0;
  while (at < n) {
    size_t end = at;
    while (end < n && text[end] != '\n')
      end++;
    if (end > at) {
      yyjson_doc *doc = yyjson_read(text + at, end - at, 0);
      if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        logs = realloc(logs, (size_t)(*count + 1) * sizeof *logs);
        if (!logs) {
          perror("realloc");
          exit(1);
        }
        Log *item = &logs[*count];
        memset(item, 0, sizeof *item);
        yyjson_val *name = yyjson_obj_get(root, "name");
        item->name = xstrdup(name && yyjson_is_str(name) ? yyjson_get_str(name) : "");
        yyjson_val *args = yyjson_obj_get(root, "args");
        if (args && yyjson_is_arr(args)) {
          item->argc = (int)yyjson_arr_size(args);
          item->args = xmalloc((size_t)item->argc * sizeof *item->args);
          for (int i = 0; i < item->argc; i++) {
            yyjson_val *arg = yyjson_arr_get(args, (size_t)i);
            item->args[i] =
                xstrdup(arg && yyjson_is_str(arg) ? yyjson_get_str(arg) : "");
          }
        }
        yyjson_val *input = yyjson_obj_get(root, "input");
        if (input && yyjson_is_str(input))
          item->input = xstrdup(yyjson_get_str(input));
        yyjson_val *settings = yyjson_obj_get(root, "settings");
        if (settings && yyjson_is_str(settings))
          item->settings = xstrdup(yyjson_get_str(settings));
        (*count)++;
        yyjson_doc_free(doc);
      }
    }
    at = end + 1;
  }
  free(text);
  return logs;
}

static Log *find_log(Log *logs, int n, const char *name) {
  for (int i = 0; i < n; i++)
    if (!strcmp(logs[i].name, name))
      return &logs[i];
  return NULL;
}

static bool args_equal(const Log *log, const char **expect, int n) {
  if (!log || log->argc != n)
    return false;
  for (int i = 0; i < n; i++)
    if (strcmp(log->args[i], expect[i]))
      return false;
  return true;
}

static bool arg_has(const Log *log, const char *value) {
  if (!log)
    return false;
  for (int i = 0; i < log->argc; i++)
    if (!strcmp(log->args[i], value))
      return true;
  return false;
}

static bool file_eq_path(const char *left, const char *right) {
  size_t a = 0, b = 0;
  char *da = read_file(left, &a), *db = read_file(right, &b);
  bool ok = da && db && a == b && !memcmp(da, db, a);
  free(da);
  free(db);
  return ok;
}

static bool file_eq_mem(const char *path, const void *data, size_t n) {
  size_t got = 0;
  char *buf = read_file(path, &got);
  bool ok = buf && got == n && !memcmp(buf, data, n);
  free(buf);
  return ok;
}

static char *file_text(const char *path) {
  size_t n = 0;
  char *buf = read_file(path, &n);
  if (!buf)
    return NULL;
  char *text = xmalloc(n + 1);
  memcpy(text, buf, n);
  text[n] = 0;
  free(buf);
  return text;
}

static void clear_log(Fix *f) { write_str(f->log, ""); }

static bool no_dot_active(Fix *f) {
  char *active = join_path(f->state, "active");
  DIR *dir = opendir(active);
  free(active);
  if (!dir)
    return false;
  struct dirent *ent;
  bool ok = true;
  while ((ent = readdir(dir))) {
    if (ent->d_name[0] == '.' && strcmp(ent->d_name, ".") &&
        strcmp(ent->d_name, ".."))
      ok = false;
  }
  closedir(dir);
  return ok;
}

static char *find_python(void) {
  const char *given = getenv("THEME_MENU_PYTHON");
  if (given && *given)
    return xstrdup(given);
  const char *path = getenv("PATH");
  if (!path)
    return NULL;
  char *copy = xstrdup(path);
  char *save = NULL;
  for (char *part = strtok_r(copy, ":", &save); part;
       part = strtok_r(NULL, ":", &save)) {
    char *cand = join_path(part, "python3");
    if (access(cand, X_OK) == 0) {
      free(copy);
      return cand;
    }
    free(cand);
  }
  free(copy);
  return NULL;
}

static size_t utf8_encode(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static void test_parity(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *steps[] = {"default", "rose-pine", "--reapply", "default"};
  for (int i = 0; i < 4; i++) {
    const char *extra[] = {"--no-reload", steps[i]};
    Result r = run(f, extra, 2, NULL, NULL, 0);
    EXPECT(r.status == 0, "parity %s status %d %s", steps[i], r.status, r.err);
    char *sel = join_path(f->state, "selection");
    EXPECT(exists_stat(sel) == (strcmp(steps[i], "default") != 0),
           "selection existence for %s", steps[i]);
    if (!strcmp(steps[i], "--reapply")) {
      char *text = file_text(sel);
      EXPECT(text && !strcmp(text, "rose-pine\n"), "reapply selection");
      free(text);
    }
    free(sel);
    result_free(&r);
  }
  const char *legacy = getenv("THEME_MENU_LEGACY");
  if (legacy) {
    char *python = find_python();
    EXPECT(python, "THEME_MENU_PYTHON");
    Fix *old = setup();
    if (python && old) {
      const char *ids[] = {"rose-pine", "light", "--reapply", "default", "dark"};
      for (int i = 0; i < 5; i++) {
        const char *extra[] = {"--no-reload", ids[i]};
        Result c = run(f, extra, 2, NULL, NULL, 0);
        EXPECT(c.status == 0, "legacy driver %s", ids[i]);
        result_free(&c);
        char **argv = xmalloc(8 * sizeof *argv);
        int n = 0;
        argv[n++] = python;
        argv[n++] = xstrdup(legacy);
        argv[n++] = xstrdup("--catalogue");
        argv[n++] = xstrdup(old->catalogue);
        argv[n++] = xstrdup("--state");
        argv[n++] = xstrdup(old->state);
        argv[n++] = xstrdup("--no-reload");
        argv[n++] = xstrdup(ids[i]);
        argv[n] = NULL;
        char **env = fixture_env(old, NULL, NULL, 0);
        Result p = start_proc(argv, env, "", 20000);
        finish_proc(&p, 20000);
        EXPECT(p.status == 0, "legacy %s %s", ids[i], p.err);
        Snap a = snapshot(f), b = snapshot(old);
        EXPECT(snap_equal(&a, &b), "legacy content %s", ids[i]);
        snap_free(&a);
        snap_free(&b);
        result_free(&p);
        free_env(env);
        for (int k = 1; argv[k]; k++)
          free(argv[k]);
        free(argv);
      }
    }
    free(python);
    fix_free(old);
  }
  fix_free(f);
}

static void test_bundles(void) {
  Fix *f = setup();
  if (!f)
    return;
  mkdir(f->state, 0755);
  char *mode = join_path(f->state, "mode");
  write_str(mode, "light\n");
  const char *extra[] = {"--no-reload", "rose-pine"};
  Result r = run(f, extra, 2, NULL, NULL, 0);
  EXPECT(r.status == 0, "bundle apply %s", r.err);
  result_free(&r);
  yyjson_doc *doc = NULL;
  size_t n = 0;
  char *json = read_file(f->catalogue, &n);
  if (json)
    doc = yyjson_read(json, n, 0);
  free(json);
  EXPECT(doc, "catalogue");
  if (doc) {
    yyjson_val *rose = yyjson_obj_get(
        yyjson_obj_get(yyjson_doc_get_root(doc), "palettes"), "rose-pine");
    yyjson_val *files = yyjson_obj_get(rose, "files");
    yyjson_val *light =
        yyjson_obj_get(yyjson_obj_get(rose, "session"), "light");
    yyjson_val *objects[] = {files, light};
    for (int o = 0; o < 2; o++) {
      yyjson_val *key, *val;
      size_t i, count;
      yyjson_obj_foreach(objects[o], i, count, key, val) {
        char *active = join_path(f->state, "active");
        char *dest = join_path(active, yyjson_get_str(key));
        EXPECT(file_eq_path(dest, yyjson_get_str(val)), "bundle %s",
               yyjson_get_str(key));
        free(dest);
        free(active);
      }
    }
    yyjson_doc_free(doc);
  }
  char *delta = join_path(f->state, "active/delta");
  struct stat before, after;
  EXPECT(stat(delta, &before) == 0, "delta stat");
  const char *re[] = {"--no-reload", "--reapply"};
  r = run(f, re, 2, NULL, NULL, 0);
  EXPECT(r.status == 0, "reapply inode");
  result_free(&r);
  EXPECT(stat(delta, &after) == 0 && before.st_ino == after.st_ino, "inode");
  free(delta);
  yyjson_mut_doc *mut = load_catalogue(f);
  yyjson_mut_val *palette = yyjson_mut_obj_get(
      yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "palettes"), "rose-pine");
  yyjson_mut_obj_add_strcpy(mut, palette, "nativeMode", "dark");
  save_catalogue(f, mut);
  r = run(f, extra, 2, NULL, NULL, 0);
  EXPECT(r.status == 0, "native apply");
  result_free(&r);
  char *text = file_text(mode);
  EXPECT(text && !strcmp(text, "dark\n"), "native mode");
  free(text);
  const char *light_args[] = {"--no-reload", "light"};
  r = run(f, light_args, 2, NULL, NULL, 0);
  EXPECT(r.status == 1, "dark-only status");
  EXPECT(contains_mem(r.err, r.err_len, "dark-only"), "dark-only stderr");
  result_free(&r);
  free(mode);
  fix_free(f);
}

static void test_mode_actions(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *apply[] = {"--no-reload", "rose-pine"};
  Result r = run(f, apply, 2, NULL, NULL, 0);
  result_free(&r);
  char *delta = join_path(f->state, "active/delta");
  write_str(delta, "manual override");
  clear_log(f);
  const char *light[] = {"light"};
  r = run(f, light, 1, NULL, NULL, 0);
  EXPECT(r.status == 0, "mode %s", r.err);
  result_free(&r);
  char *sel = join_path(f->state, "selection");
  char *text = file_text(sel);
  EXPECT(text && !strcmp(text, "rose-pine\n"), "selection preserved");
  free(text);
  free(sel);
  EXPECT(file_eq_mem(delta, "manual override", 15), "override preserved");
  char *mode = join_path(f->state, "mode");
  text = file_text(mode);
  EXPECT(text && !strcmp(text, "light\n"), "mode light");
  free(text);
  free(mode);
  int n = 0;
  Log *logs = read_logs(f, &n);
  EXPECT(n == 1 && args_equal(&logs[0], (const char *[]){"Appearance mode", "Light mode"}, 2) &&
             !strcmp(logs[0].name, "notify-send"),
         "mode notify log");
  logs_free(logs, n);
  free(delta);
  fix_free(f);
}

static void test_unknown(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *ids[] = {"../outside", "unknown"};
  for (int i = 0; i < 2; i++) {
    const char *extra[] = {"--no-reload", ids[i]};
    Result r = run(f, extra, 2, NULL, NULL, 0);
    EXPECT(r.status == 1, "unknown %s", ids[i]);
    char *active = join_path(f->state, "active");
    EXPECT(!exists_stat(active), "no active %s", ids[i]);
    free(active);
    result_free(&r);
  }
  char *delta = join_path(f->root, "rose-pine/delta");
  unlink(delta);
  free(delta);
  const char *extra[] = {"--no-reload", "rose-pine"};
  Result r = run(f, extra, 2, NULL, NULL, 0);
  EXPECT(r.status == 1, "missing source");
  char *active = join_path(f->state, "active");
  EXPECT(!exists_stat(active), "no active after missing source");
  free(active);
  result_free(&r);
  write_str(f->catalogue, "[]");
  const char *def[] = {"--no-reload", "default"};
  r = run(f, def, 2, NULL, NULL, 0);
  EXPECT(r.status == 1, "invalid catalogue");
  result_free(&r);
  fix_free(f);
}

static void test_rollback(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *def[] = {"--no-reload", "default"};
  Result r = run(f, def, 2, NULL, NULL, 0);
  result_free(&r);
  Snap original = snapshot(f);
  const char *rose[] = {"--no-reload", "rose-pine"};
  r = run(f, rose, 2, NULL, NULL, 0);
  result_free(&r);
  Snap changed = snapshot(f);
  int count = changed_count(&original, &changed);
  snap_free(&changed);
  r = run(f, def, 2, NULL, NULL, 0);
  result_free(&r);
  int steps[] = {0, 4, count - 2, count - 1};
  for (int i = 0; i < 4; i++) {
    r = run_fixture(f, rose, 2, NULL, NULL, 0, steps[i]);
    EXPECT(r.status == 1, "rollback %d %s", steps[i], r.err);
    Snap now = snapshot(f);
    EXPECT(snap_equal(&now, &original), "rollback content %d", steps[i]);
    EXPECT(no_dot_active(f), "rollback temps %d", steps[i]);
    snap_free(&now);
    result_free(&r);
  }
  r = run(f, rose, 2, NULL, NULL, 0);
  result_free(&r);
  Snap light = snapshot(f);
  const char *mode[] = {"--no-reload", "light"};
  r = run_fixture(f, mode, 2, NULL, NULL, 0, 1);
  EXPECT(r.status == 1, "mode rollback");
  Snap now = snapshot(f);
  EXPECT(snap_equal(&now, &light), "mode rollback content");
  snap_free(&now);
  snap_free(&light);
  snap_free(&original);
  result_free(&r);
  fix_free(f);
}

static void test_symlinks(void) {
  Fix *f = setup();
  if (!f)
    return;
  char *active = join_path(f->state, "active");
  mkdir(f->state, 0755);
  mkdir(active, 0755);
  char *target = join_path(f->root, "target");
  write_str(target, "target");
  char *delta = join_path(active, "delta");
  symlink(target, delta);
  char *missing = join_path(f->root, "missing");
  char *missing2 = join_path(f->root, "missing2");
  char *sel = join_path(f->state, "selection");
  char *mode = join_path(f->state, "mode");
  symlink(missing, sel);
  symlink(missing2, mode);
  const char *extra[] = {"--no-reload", "--reapply"};
  Result r = run(f, extra, 2, NULL, NULL, 0);
  EXPECT(r.status == 0, "symlink reapply %s", r.err);
  struct stat st;
  EXPECT(stat(delta, &st) == 0 && S_ISREG(st.st_mode), "delta replaced");
  char *text = file_text(target);
  EXPECT(text && !strcmp(text, "target"), "target untouched");
  free(text);
  EXPECT(!exists_stat(sel), "broken selection cleared");
  text = file_text(mode);
  EXPECT(text && !strcmp(text, "dark\n"), "mode fallback");
  free(text);
  result_free(&r);
  free(active);
  free(target);
  free(delta);
  free(missing);
  free(missing2);
  free(sel);
  free(mode);
  fix_free(f);
}

static void test_solarized(void) {
  Fix *f = setup();
  if (!f)
    return;
  mkdir(f->state, 0755);
  char *sel = join_path(f->state, "selection");
  char raw[] = {(char)0xC2, (char)0x85, 's', 'o', 'l', 'a', 'r', 'i',
                'z', 'e', 'd', 0x1c, '\n'};
  write_file(sel, raw, sizeof raw);
  char *mode = join_path(f->state, "mode");
  write_str(mode, "invalid\n");
  const char *extra[] = {"--no-reload", "--reapply"};
  Result r = run(f, extra, 2, NULL, NULL, 0);
  EXPECT(r.status == 0, "solarized %s", r.err);
  char *text = file_text(sel);
  EXPECT(text && !strcmp(text, "osaka-jade\n"), "migrated");
  free(text);
  text = file_text(mode);
  EXPECT(text && !strcmp(text, "dark\n"), "invalid mode");
  free(text);
  result_free(&r);
  free(sel);
  free(mode);
  fix_free(f);
}

static void test_popup(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *rose[] = {"--no-reload", "rose-pine"};
  Result r = run(f, rose, 2, NULL, NULL, 0);
  result_free(&r);
  clear_log(f);
  const char *keys[] = {"TM_COMMANDS"};
  const char *vals[] = {"{\"fuzzel\":{\"out\":\"1\\n\"}}"};
  const char *popup[] = {"--no-reload"};
  r = run(f, popup, 1, keys, vals, 1);
  EXPECT(r.status == 0, "popup %s", r.err);
  result_free(&r);
  int n = 0;
  Log *logs = read_logs(f, &n);
  Log *menu = find_log(logs, n, "fuzzel");
  EXPECT(menu && menu->input &&
             !strcmp(menu->input,
                     "  Switch to light mode\n  Host default (Tokyo Night)\n  "
                     "Osaka Jade\n* Rosé Pine\n  Tokyo Night\n"),
         "popup input");
  EXPECT(menu && menu->argc && !strcmp(menu->args[menu->argc - 1], "3"),
         "popup index");
  EXPECT(menu && menu->settings && strstr(menu->settings, "font=Test Font:size=14") &&
             strstr(menu->settings, "width=3\nradius=7"),
         "popup settings");
  char *sel = join_path(f->state, "selection");
  EXPECT(!exists_stat(sel), "default selected");
  free(sel);
  const char *legacy = getenv("THEME_MENU_LEGACY");
  if (legacy && menu && menu->settings) {
    char *python = find_python();
    EXPECT(python, "python for styling parity");
    Fix *old = setup();
    if (python && old) {
      char **argv = xmalloc(10 * sizeof *argv);
      int k = 0;
      argv[k++] = python;
      argv[k++] = xstrdup(legacy);
      argv[k++] = xstrdup("--catalogue");
      argv[k++] = xstrdup(old->catalogue);
      argv[k++] = xstrdup("--state");
      argv[k++] = xstrdup(old->state);
      argv[k++] = xstrdup("--no-reload");
      argv[k++] = xstrdup("rose-pine");
      argv[k] = NULL;
      char **env = fixture_env(old, NULL, NULL, 0);
      Result p = start_proc(argv, env, "", 20000);
      finish_proc(&p, 20000);
      result_free(&p);
      free_env(env);
      for (int i = 1; argv[i]; i++)
        free(argv[i]);
      free(argv);
      clear_log(old);
      argv = xmalloc(10 * sizeof *argv);
      k = 0;
      argv[k++] = python;
      argv[k++] = xstrdup(legacy);
      argv[k++] = xstrdup("--catalogue");
      argv[k++] = xstrdup(old->catalogue);
      argv[k++] = xstrdup("--state");
      argv[k++] = xstrdup(old->state);
      argv[k++] = xstrdup("--no-reload");
      argv[k] = NULL;
      const char *ek[] = {"TM_COMMANDS"};
      const char *ev[] = {"{\"fuzzel\":{\"out\":\"1\\n\"}}"};
      env = fixture_env(old, ek, ev, 1);
      p = start_proc(argv, env, "", 20000);
      finish_proc(&p, 20000);
      EXPECT(p.status == 0, "legacy popup %s", p.err);
      int on = 0;
      Log *ol = read_logs(old, &on);
      Log *om = find_log(ol, on, "fuzzel");
      EXPECT(om && om->settings && !strcmp(om->settings, menu->settings),
             "legacy settings");
      logs_free(ol, on);
      result_free(&p);
      free_env(env);
      for (int i = 1; argv[i]; i++)
        free(argv[i]);
      free(argv);
    }
    free(python);
    fix_free(old);
  }
  logs_free(logs, n);
  fix_free(f);
}

static void test_cancel(void) {
  struct {
    const char *json;
    int code;
    bool err;
  } cases[] = {
      {"{\"fuzzel\":{\"code\":1,\"out\":\"\"}}", 1, false},
      {"{\"fuzzel\":{\"code\":2,\"out\":\"\"}}", 2, false},
      {"{\"fuzzel\":{\"code\":0,\"out\":\"999\"}}", 0, false},
      {"{\"fuzzel\":{\"code\":0,\"out\":\"-1\"}}", 0, false},
      {"{\"fuzzel\":{\"code\":0,\"out\":\"bad\"}}", 0, false},
      {"{\"fuzzel\":{\"code\":0,\"out\":\"0\\u0000bad\"}}", 0, false},
      {"{\"fuzzel\":{\"code\":2,\"err\":\"invalid option\"}}", 2, true},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    Fix *f = setup();
    if (!f)
      return;
    const char *keys[] = {"TM_COMMANDS"};
    const char *vals[] = {cases[i].json};
    const char *extra[] = {"--no-reload"};
    Result r = run(f, extra, 1, keys, vals, 1);
    int want = cases[i].code == 1 || (cases[i].code == 2 && !cases[i].err) ? 0 : 1;
    EXPECT(r.status == want, "cancel %zu status %d %s", i, r.status, r.err);
    char *active = join_path(f->state, "active");
    EXPECT(!exists_stat(active), "cancel %zu no active", i);
    free(active);
    result_free(&r);
    fix_free(f);
  }
}

static void test_unicode_index(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *def[] = {"--no-reload", "default"};
  Result r = run(f, def, 2, NULL, NULL, 0);
  result_free(&r);
  const char *keys[] = {"TM_COMMANDS"};
  const char *vals[] = {"{\"fuzzel\":{\"out\":\"٠\",\"writeMode\":\"light\"}}"};
  const char *extra[] = {"--no-reload"};
  r = run(f, extra, 1, keys, vals, 1);
  EXPECT(r.status == 0, "unicode index %s", r.err);
  char *mode = join_path(f->state, "mode");
  char *text = file_text(mode);
  EXPECT(text && !strcmp(text, "light\n"), "unicode mode");
  free(text);
  free(mode);
  result_free(&r);
  fix_free(f);
}

static void test_native_popup(void) {
  Fix *f = setup();
  if (!f)
    return;
  yyjson_mut_doc *mut = load_catalogue(f);
  yyjson_mut_val *palette = yyjson_mut_obj_get(
      yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "palettes"), "rose-pine");
  yyjson_mut_obj_add_strcpy(mut, palette, "nativeMode", "light");
  save_catalogue(f, mut);
  const char *rose[] = {"--no-reload", "rose-pine"};
  Result r = run(f, rose, 2, NULL, NULL, 0);
  result_free(&r);
  clear_log(f);
  const char *keys[] = {"TM_COMMANDS"};
  const char *vals[] = {"{\"fuzzel\":{\"out\":\"0\"}}"};
  const char *extra[] = {"--no-reload"};
  r = run(f, extra, 1, keys, vals, 1);
  EXPECT(r.status == 0, "native popup %s", r.err);
  int n = 0;
  Log *logs = read_logs(f, &n);
  Log *menu = find_log(logs, n, "fuzzel");
  EXPECT(menu && menu->input && !strncmp(menu->input, "  Host default", 14),
         "native menu start");
  char *sel = join_path(f->state, "selection");
  EXPECT(!exists_stat(sel), "native default");
  free(sel);
  logs_free(logs, n);
  result_free(&r);
  fix_free(f);
}

static void test_lock(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *keys[] = {"TM_COMMANDS"};
  const char *vals[] = {"{\"fuzzel\":{\"code\":2,\"delay\":800}}"};
  const char *extra[] = {"--no-reload"};
  char **argv = menu_argv(f, extra, 1, false, -1);
  char **env = fixture_env(f, keys, vals, 1);
  Result first = start_proc(argv, env, "", 20000);
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  int n = 0;
  while (!n) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - start.tv_sec) * 1000 +
            (now.tv_nsec - start.tv_nsec) / 1000000 >
        3000)
      break;
    Log *logs = read_logs(f, &n);
    logs_free(logs, n);
    if (!n) {
      struct timespec pause = {.tv_nsec = 10 * 1000000L};
      nanosleep(&pause, NULL);
    }
  }
  EXPECT(n > 0, "lock saw popup");
  const char *second_args[] = {"--no-reload", "rose-pine"};
  Result second = run(f, second_args, 2, NULL, NULL, 0);
  EXPECT(second.status == 1, "lock status");
  EXPECT(contains_mem(second.err, second.err_len, "Another theme menu is open"),
         "lock message");
  finish_proc(&first, 20000);
  EXPECT(first.status == 0, "first popup %s", first.err);
  result_free(&second);
  result_free(&first);
  free_argv(argv);
  free_env(env);
  fix_free(f);
}

static void test_unpublished_wallpaper(void) {
  Fix *f = setup();
  if (!f)
    return;
  yyjson_mut_doc *mut = load_catalogue(f);
  yyjson_mut_val *dark = yyjson_mut_obj_get(
      yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_obj_get(
                             yyjson_mut_doc_get_root(mut), "palettes"),
                         "tokyo-night"),
                         "session"),
      "dark");
  set_str(mut, dark, "wallpaper.png", "nix-theme:tokyo-night");
  save_catalogue(f, mut);
  const char *extra[] = {"--no-reload", "default"};
  Result r = run(f, extra, 2, NULL, NULL, 0);
  EXPECT(r.status == 0, "unpublished %s", r.err);
  char *wall = join_path(f->state, "active/wallpaper.png");
  EXPECT(!exists_stat(wall), "no wallpaper copy");
  free(wall);
  int n = 0;
  Log *logs = read_logs(f, &n);
  EXPECT(n == 0, "no commands");
  logs_free(logs, n);
  result_free(&r);
  fix_free(f);
}

static void test_published_wallpaper(void) {
  Fix *f = setup();
  if (!f)
    return;
  yyjson_mut_doc *mut = load_catalogue(f);
  yyjson_mut_val *palettes =
      yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "palettes");
  size_t i, count;
  yyjson_mut_val *key, *val;
  yyjson_mut_obj_foreach(palettes, i, count, key, val) {
    yyjson_mut_val *dark =
        yyjson_mut_obj_get(yyjson_mut_obj_get(val, "session"), "dark");
    set_str(mut, dark, "wallpaper.png", "nix-theme:wallpaper-id");
  }
  save_catalogue(f, mut);
  char *nix = xmalloc(strlen(f->wallpaper) + 32);
  snprintf(nix, strlen(f->wallpaper) + 32, "{\"nix\":{\"out\":\"%s\\n\"}}",
           f->wallpaper);
  const char *keys[] = {"THEME_MENU_PUBLISH", "THEME_WALLPAPER_FLAKE",
                        "TM_COMMANDS"};
  const char *vals[] = {"1", "/flake with spaces", nix};
  const char *extra[] = {"default"};
  Result r = run_fixture(f, extra, 1, keys, vals, 3, -1);
  EXPECT(r.status == 0, "publish wallpaper %s", r.err);
  unsigned char png[] = {0, 1, 2, 255};
  char *copied = join_path(f->state, "active/wallpaper.png");
  EXPECT(file_eq_mem(copied, png, sizeof png), "wallpaper bytes");
  free(copied);
  char *link = join_path(f->state, "wallpaper-source");
  char target[4096];
  ssize_t tn = readlink(link, target, sizeof target);
  EXPECT(tn == (ssize_t)strlen(f->wallpaper) &&
             !memcmp(target, f->wallpaper, (size_t)tn),
         "wallpaper root");
  free(link);
  int n = 0;
  Log *logs = read_logs(f, &n);
  int nix_n = 0;
  Log *calls[8] = {0};
  for (int k = 0; k < n && nix_n < 8; k++)
    if (!strcmp(logs[k].name, "nix"))
      calls[nix_n++] = &logs[k];
  const char *first[] = {"build", "--no-link", "--print-out-paths",
                         "/flake with spaces#theme-wallpaper-id"};
  EXPECT(nix_n >= 2 && args_equal(calls[0], first, 4), "nix build args");
  EXPECT(nix_n >= 2 && arg_has(calls[1], "--out-link"), "nix out-link");
  logs_free(logs, n);
  result_free(&r);
  char *current = join_path(f->state, "wallpaper-current");
  char *live = join_path(f->state, "active/wallpaper-live.png");
  write_str(current, "rotated.png");
  write_str(live, "rotated");
  const char *re[] = {"--reapply"};
  r = run_fixture(f, re, 1, keys, vals, 3, -1);
  EXPECT(r.status == 0, "reapply keeps rotation");
  char *text = file_text(current);
  EXPECT(text && !strcmp(text, "rotated.png"), "rotation kept");
  free(text);
  result_free(&r);
  mut = load_catalogue(f);
  yyjson_mut_val *dark = yyjson_mut_obj_get(
      yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_obj_get(
                             yyjson_mut_doc_get_root(mut), "palettes"),
                         "tokyo-night"),
                         "session"),
      "dark");
  set_str(mut, dark, "wallpaper.png", "nix-theme:other");
  save_catalogue(f, mut);
  r = run_fixture(f, re, 1, keys, vals, 3, -1);
  EXPECT(r.status == 0, "theme change reset");
  EXPECT(!exists_stat(current), "rotation cleared");
  result_free(&r);
  free(current);
  free(live);
  free(nix);
  fix_free(f);
}

static void test_wallpaper_override(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *def[] = {"--no-reload", "default"};
  Result r = run(f, def, 2, NULL, NULL, 0);
  result_free(&r);
  Snap before = snapshot(f);
  const char *bad[] = {"nix-theme:../escape", "nix-theme:bad id", "nix-theme:"};
  for (int i = 0; i < 3; i++) {
    yyjson_mut_doc *mut = load_catalogue(f);
    yyjson_mut_val *dark = yyjson_mut_obj_get(
        yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_obj_get(
                               yyjson_mut_doc_get_root(mut), "palettes"),
                           "rose-pine"),
                           "session"),
        "dark");
    set_str(mut, dark, "wallpaper.png", bad[i]);
    save_catalogue(f, mut);
    const char *keys[] = {"THEME_MENU_PUBLISH"};
    const char *vals[] = {"1"};
    const char *extra[] = {"rose-pine"};
    r = run_fixture(f, extra, 1, keys, vals, 1, -1);
    EXPECT(r.status == 1, "bad wallpaper %s", bad[i]);
    Snap now = snapshot(f);
    EXPECT(snap_equal(&now, &before), "untouched %s", bad[i]);
    snap_free(&now);
    result_free(&r);
  }
  yyjson_mut_doc *mut = load_catalogue(f);
  yyjson_mut_val *dark = yyjson_mut_obj_get(
      yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_obj_get(
                             yyjson_mut_doc_get_root(mut), "palettes"),
                         "rose-pine"),
                         "session"),
      "dark");
  set_str(mut, dark, "wallpaper.png", "nix-theme:good");
  save_catalogue(f, mut);
  const char *keys[] = {"THEME_MENU_PUBLISH", "TM_COMMANDS"};
  const char *vals[] = {"1", "{\"nix\":{\"code\":1}}"};
  const char *extra[] = {"rose-pine"};
  r = run_fixture(f, extra, 1, keys, vals, 2, -1);
  EXPECT(r.status == 1, "failed build");
  Snap now = snapshot(f);
  EXPECT(snap_equal(&now, &before), "failed build untouched");
  snap_free(&now);
  result_free(&r);
  const char *names[] = {"wallpaper-current", "wallpaper-theme"};
  for (int i = 0; i < 2; i++) {
    char *path = join_path(f->state, names[i]);
    write_str(path, "old");
    free(path);
  }
  char *live = join_path(f->state, "active/wallpaper-live.png");
  write_str(live, "old");
  free(live);
  char *link = join_path(f->state, "wallpaper-source");
  symlink(f->wallpaper, link);
  free(link);
  char *png = join_path(f->wallpaper, "wallpaper.png");
  mut = load_catalogue(f);
  dark = yyjson_mut_obj_get(
      yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_obj_get(
                             yyjson_mut_doc_get_root(mut), "palettes"),
                         "rose-pine"),
                         "session"),
      "dark");
  set_str(mut, dark, "wallpaper.png", png);
  save_catalogue(f, mut);
  const char *pk[] = {"THEME_MENU_PUBLISH"};
  const char *pv[] = {"1"};
  r = run_fixture(f, extra, 1, pk, pv, 1, -1);
  EXPECT(r.status == 0, "override apply %s", r.err);
  const char *gone[] = {"wallpaper-current", "wallpaper-theme",
                        "wallpaper-source", "active/wallpaper-live.png"};
  for (int i = 0; i < 4; i++) {
    char *path = join_path(f->state, gone[i]);
    EXPECT(!exists_stat(path), "cleared %s", gone[i]);
    free(path);
  }
  result_free(&r);
  free(png);
  snap_free(&before);
  fix_free(f);
}

static void test_greeter(void) {
  const char *kinds[] = {"regular", "missing", "symlink", "directory", "long"};
  for (int i = 0; i < 5; i++) {
    Fix *f = setup();
    if (!f)
      return;
    if (!strcmp(kinds[i], "regular"))
      write_str(f->greeter, "old id much longer");
    if (!strcmp(kinds[i], "symlink")) {
      char *target = join_path(f->root, "target");
      write_str(target, "untouched");
      symlink(target, f->greeter);
      free(target);
    }
    if (!strcmp(kinds[i], "directory"))
      mkdir(f->greeter, 0755);
    if (!strcmp(kinds[i], "long")) {
      char id[65];
      memset(id, 'a', 64);
      id[64] = 0;
      yyjson_mut_doc *mut = load_catalogue(f);
      yyjson_mut_val *root = yyjson_mut_doc_get_root(mut);
      yyjson_mut_val *palettes = yyjson_mut_obj_get(root, "palettes");
      yyjson_mut_val *copy = yyjson_mut_val_mut_copy(
          mut, yyjson_mut_obj_get(palettes, "rose-pine"));
      yyjson_mut_obj_add_val(mut, palettes, id, copy);
      set_str(mut, root, "default", id);
      save_catalogue(f, mut);
      write_str(f->greeter, "untouched");
    }
    const char *keys[] = {"THEME_MENU_PUBLISH"};
    const char *vals[] = {"1"};
    const char *extra[] = {"default"};
    Result r = run_fixture(f, extra, 1, keys, vals, 1, -1);
    EXPECT(r.status == 0, "greeter %s %s", kinds[i], r.err);
    if (!strcmp(kinds[i], "regular")) {
      char *text = file_text(f->greeter);
      EXPECT(text && !strcmp(text, "tokyo-night\n"), "greeter id");
      free(text);
    }
    if (!strcmp(kinds[i], "missing"))
      EXPECT(!exists_stat(f->greeter), "greeter not created");
    if (!strcmp(kinds[i], "symlink")) {
      char *target = join_path(f->root, "target");
      char *text = file_text(target);
      EXPECT(text && !strcmp(text, "untouched"), "greeter symlink target");
      free(text);
      free(target);
    }
    if (!strcmp(kinds[i], "long")) {
      char *text = file_text(f->greeter);
      EXPECT(text && !strcmp(text, "untouched"), "long greeter");
      free(text);
    }
    result_free(&r);
    fix_free(f);
  }
}

static int count_name(Log *logs, int n, const char *name) {
  int c = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(logs[i].name, name))
      c++;
  return c;
}

static void test_hyprland(void) {
  Fix *f = setup();
  if (!f)
    return;
  char *g3 = join_path(f->config, "gtk-3.0");
  char *g4 = join_path(f->config, "gtk-4.0");
  mkdir(g3, 0755);
  mkdir(g4, 0755);
  char *custom = join_path(f->root, "custom");
  write_str(custom, "custom");
  char *css3 = join_path(g3, "gtk.css");
  char *css4 = join_path(g4, "gtk.css");
  symlink(custom, css3);
  write_str(css4, "custom");
  const char *keys[] = {"THEME_MENU_PUBLISH", "XDG_CURRENT_DESKTOP"};
  const char *vals[] = {"1", "GNOME: HYPRLAND ; other"};
  const char *extra[] = {"default"};
  Result r = run_fixture(f, extra, 1, keys, vals, 2, -1);
  EXPECT(r.status == 0, "hyprland %s", r.err);
  EXPECT(contains_mem(r.err, r.err_len, "Preserved custom gtk-4.0"),
         "gtk warning");
  char *text = file_text(custom);
  EXPECT(text && !strcmp(text, "custom"), "custom preserved");
  free(text);
  int n = 0;
  Log *logs = read_logs(f, &n);
  const char *last[] = {"prefer-dark", "custom-dark", "Test Font 15",
                        "Test Mono 15"};
  int seen = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(logs[i].name, "gsettings")) {
      EXPECT(seen < 4 && logs[i].argc &&
                 !strcmp(logs[i].args[logs[i].argc - 1], last[seen]),
             "gsettings %d", seen);
      seen++;
    }
  EXPECT(seen == 4, "gsettings count");
  const char *hypr[][3] = {
      {"keyword", "general:col.active_border", "rgb(abcdef)"},
      {"keyword", "decoration:rounding", "9"}};
  int h = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(logs[i].name, "hyprctl")) {
      EXPECT(h < 2 && args_equal(&logs[i], hypr[h], 3), "hyprctl %d", h);
      h++;
    }
  EXPECT(h == 2, "hyprctl count");
  const char *sys[][4] = {
      {"--user", "kill", "--signal=SIGUSR2", "waybar.service"},
      {"--user", "try-restart", "walker.service", NULL},
      {"--user", "reset-failed", "hypr-wallpaper.service", NULL},
      {"--user", "restart", "hypr-wallpaper.service", NULL}};
  int slen[] = {4, 3, 3, 3};
  int s = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(logs[i].name, "systemctl")) {
      EXPECT(s < 4 && args_equal(&logs[i], sys[s], slen[s]), "systemctl %d", s);
      s++;
    }
  EXPECT(s == 4, "systemctl count");
  EXPECT(count_name(logs, n, "pkill") == 0, "no pkill");
  logs_free(logs, n);
  result_free(&r);
  free(g3);
  free(g4);
  free(custom);
  free(css3);
  free(css4);
  fix_free(f);
}

static void test_desktop_failures(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *table =
      "{\"gsettings\":{\"code\":1,\"err\":\"schema missing\"},"
      "\"hyprctl\":{\"code\":1,\"err\":\"no socket\"},"
      "\"systemctl --user kill --signal=SIGUSR2 waybar.service\":"
      "{\"code\":1,\"err\":\"not active\"},\"makoctl\":{\"code\":1},"
      "\"herdr\":{\"code\":1}}";
  const char *keys[] = {"THEME_MENU_PUBLISH", "HYPRLAND_INSTANCE_SIGNATURE",
                        "TM_COMMANDS"};
  const char *vals[] = {"1", "instance", table};
  const char *extra[] = {"default"};
  Result r = run_fixture(f, extra, 1, keys, vals, 3, -1);
  EXPECT(r.status == 0, "desktop failures %s", r.err);
  const char *messages[] = {"schema missing", "no socket", "not active",
                            "Herdr was not reloaded"};
  for (int i = 0; i < 4; i++)
    EXPECT(contains_mem(r.err, r.err_len, messages[i]), "missing %s",
           messages[i]);
  EXPECT(!contains_mem(r.err, r.err_len, "Notifications will use"),
         "mako suppressed");
  int n = 0;
  Log *logs = read_logs(f, &n);
  EXPECT(count_name(logs, n, "gdbus") == 1, "one gdbus");
  logs_free(logs, n);
  result_free(&r);
  const char *again =
      "{\"gsettings\":{\"code\":1,\"err\":\"schema missing\"},"
      "\"hyprctl\":{\"code\":1,\"err\":\"no socket\"},"
      "\"systemctl --user kill --signal=SIGUSR2 waybar.service\":"
      "{\"code\":1,\"err\":\"not active\"},\"makoctl\":{\"code\":1},"
      "\"herdr\":{\"code\":1},\"gdbus\":{\"out\":\"(true,)\"}}";
  const char *k2[] = {"TM_COMMANDS"};
  const char *v2[] = {again};
  const char *re[] = {"--reapply"};
  r = run_fixture(f, re, 1, k2, v2, 1, -1);
  EXPECT(r.status == 0, "ghostty reload");
  logs = read_logs(f, &n);
  bool found = false;
  for (int i = 0; i < n; i++)
    if (arg_has(&logs[i], "org.gtk.Actions.Activate"))
      found = true;
  EXPECT(found, "ghostty activate");
  logs_free(logs, n);
  result_free(&r);
  fix_free(f);
}

static void test_gtk_read_error(void) {
  Fix *f = setup();
  if (!f)
    return;
  char *dir = join_path(f->config, "gtk-3.0/gtk.css");
  char *parent = join_path(f->config, "gtk-3.0");
  mkdir(parent, 0755);
  mkdir(dir, 0755);
  const char *keys[] = {"THEME_MENU_PUBLISH", "HYPRLAND_INSTANCE_SIGNATURE"};
  const char *vals[] = {"1", "test"};
  const char *extra[] = {"rose-pine"};
  Result r = run_fixture(f, extra, 1, keys, vals, 2, -1);
  EXPECT(r.status == 1, "gtk read error");
  char *sel = join_path(f->state, "selection");
  char *text = file_text(sel);
  EXPECT(text && !strcmp(text, "rose-pine\n"), "palette retained");
  free(text);
  free(sel);
  int n = 0;
  Log *logs = read_logs(f, &n);
  EXPECT(count_name(logs, n, "hyprctl") == 0 &&
             count_name(logs, n, "systemctl") == 0,
         "later reloads stopped");
  logs_free(logs, n);
  result_free(&r);
  free(dir);
  free(parent);
  fix_free(f);
}

static void test_equal_labels(void) {
  Fix *f = setup();
  if (!f)
    return;
  yyjson_mut_doc *mut = load_catalogue(f);
  yyjson_mut_val *palettes =
      yyjson_mut_obj_get(yyjson_mut_doc_get_root(mut), "palettes");
  size_t i, count;
  yyjson_mut_val *key, *val;
  yyjson_mut_obj_foreach(palettes, i, count, key, val)
      set_str(mut, val, "label", "Same");
  save_catalogue(f, mut);
  const char *keys[] = {"TM_COMMANDS"};
  const char *vals[] = {"{\"fuzzel\":{\"out\":\"2\"}}"};
  const char *extra[] = {"--no-reload"};
  Result r = run(f, extra, 1, keys, vals, 1);
  EXPECT(r.status == 0, "equal labels %s", r.err);
  char *sel = join_path(f->state, "selection");
  char *text = file_text(sel);
  EXPECT(text && !strcmp(text, "tokyo-night\n"), "catalogue order");
  free(text);
  free(sel);
  int n = 0;
  Log *logs = read_logs(f, &n);
  Log *menu = find_log(logs, n, "fuzzel");
  EXPECT(menu && menu->input &&
             !strcmp(menu->input,
                     "  Switch to light mode\n* Host default (Same)\n  Same\n  "
                     "Same\n  Same\n"),
         "equal input");
  logs_free(logs, n);
  result_free(&r);
  char *g3 = join_path(f->config, "gtk-3.0");
  mkdir(g3, 0755);
  char *css = join_path(g3, "gtk.css");
  char marker[] = {'\0', 'S', 'h', 'a', 'r', 'e', 'd', ' ', 'G', 'T', 'K', ' ',
                   'a', 'n', 'd', ' ', 'B', 'r', 'a', 'v', 'e', ' ', 'c', 'o',
                   'l', 'o', 'u', 'r', 's', ' ', 'a', 'n', 'd', ' ', 'f', 'o',
                   'n', 't', 's'};
  write_file(css, marker, sizeof marker);
  const char *pk[] = {"THEME_MENU_PUBLISH", "HYPRLAND_INSTANCE_SIGNATURE"};
  const char *pv[] = {"1", "test"};
  const char *def[] = {"default"};
  Result pub = run_fixture(f, def, 1, pk, pv, 2, -1);
  EXPECT(pub.status == 0, "binary gtk %s", pub.err);
  text = file_text(css);
  EXPECT(text && !strcmp(text, "/* Shared GTK and Brave colours and fonts */\n"),
         "gtk owned");
  free(text);
  result_free(&pub);
  free(g3);
  free(css);
  fix_free(f);
}

static void test_btop(void) {
  Fix *f = setup();
  if (!f)
    return;
  char *marker = join_path(f->root, "signal");
  int ready[2];
  if (pipe(ready) < 0) {
    fix_free(f);
    return;
  }
  pid_t child = fork();
  if (child == 0) {
    close(ready[0]);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGTERM);
    sigprocmask(SIG_BLOCK, &set, NULL);
    write_all_fd(ready[1], "ready", 5);
    close(ready[1]);
    for (;;) {
      int sig = 0;
      sigwait(&set, &sig);
      if (sig == SIGUSR2)
        write_file(marker, "reloaded", 8);
      else
        _exit(0);
    }
  }
  close(ready[1]);
  char buf[8];
  read(ready[0], buf, 5);
  close(ready[0]);
  char pid[32];
  snprintf(pid, sizeof pid, "%d", (int)child);
  char *proc = join_path(f->proc, pid);
  mkdir(proc, 0755);
  char *comm = join_path(proc, "comm");
  write_str(comm, "btop\n");
  const char *keys[] = {"THEME_MENU_PUBLISH"};
  const char *vals[] = {"1"};
  const char *extra[] = {"default"};
  Result r = run_fixture(f, extra, 1, keys, vals, 1, -1);
  EXPECT(r.status == 0, "btop %s", r.err);
  for (int i = 0; i < 200 && !exists_stat(marker); i++) {
    struct timespec pause = {.tv_nsec = 10 * 1000000L};
    nanosleep(&pause, NULL);
  }
  char *text = file_text(marker);
  EXPECT(text && !strcmp(text, "reloaded"), "btop signal");
  free(text);
  unlink(marker);
  write_str(comm, "not-btop\n");
  Result again = run_fixture(f, extra, 1, keys, vals, 1, -1);
  EXPECT(again.status == 0, "non-btop");
  EXPECT(!exists_stat(marker), "no signal");
  kill(child, SIGTERM);
  waitpid(child, NULL, 0);
  result_free(&r);
  result_free(&again);
  free(marker);
  free(proc);
  free(comm);
  fix_free(f);
}

static void test_deadline(void) {
  Fix *f = setup();
  if (!f)
    return;
  const char *keys[] = {"TM_COMMANDS"};
  const char *vals[] = {"{\"gdbus\":{\"delay\":6000}}"};
  const char *extra[] = {"default"};
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  Result r = run_fixture(f, extra, 1, keys, vals, 1, -1);
  clock_gettime(CLOCK_MONOTONIC, &end);
  long elapsed = (end.tv_sec - start.tv_sec) * 1000 +
                 (end.tv_nsec - start.tv_nsec) / 1000000;
  EXPECT(r.status == 0, "deadline status %s", r.err);
  EXPECT(elapsed >= 4900 && elapsed < 7500, "deadline %ld", elapsed);
  EXPECT(contains_mem(r.err, r.err_len, "Reload Ghostty configuration manually"),
         "ghostty warning");
  result_free(&r);
  fix_free(f);
}

static void test_whitespace(void) {
  const uint32_t spaces[] = {
      9,     10,    11,    12,    13,    28,    29,    30,    31,    32,
      0x85,  0xa0,  0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
      0x2006, 0x2007, 0x2008, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f,
      0x3000};
  for (size_t i = 0; i < sizeof spaces / sizeof spaces[0]; i++) {
    Fix *f = setup();
    if (!f)
      return;
    mkdir(f->state, 0755);
    char enc[4];
    size_t en = utf8_encode(spaces[i], enc);
    char *body = xmalloc(en * 2 + 9);
    memcpy(body, enc, en);
    memcpy(body + en, "rose-pine", 9);
    memcpy(body + en + 9, enc, en);
    char *sel = join_path(f->state, "selection");
    write_file(sel, body, en * 2 + 9);
    const char *extra[] = {"--no-reload", "--reapply"};
    Result r = run(f, extra, 2, NULL, NULL, 0);
    EXPECT(r.status == 0, "space U+%X %s", spaces[i], r.err);
    char *text = file_text(sel);
    EXPECT(text && !strcmp(text, "rose-pine\n"), "trimmed U+%X", spaces[i]);
    free(text);
    result_free(&r);
    char out[64];
    memcpy(out, "{\"fuzzel\":{\"out\":\"", 19);
    size_t at = 19;
    for (size_t k = 0; k < en; k++) {
      snprintf(out + at, sizeof out - at, "\\u%04x", (unsigned)enc[k]);
      at += 6;
    }
    /* JSON must contain the code point, not its UTF-8 bytes escaped. */
    char json[96];
    snprintf(json, sizeof json, "{\"fuzzel\":{\"out\":\"");
    size_t j = strlen(json);
    if (spaces[i] < 0x20 || spaces[i] == 0x7f) {
      snprintf(json + j, sizeof json - j, "\\u%04x", spaces[i]);
      j = strlen(json);
    } else {
      memcpy(json + j, enc, en);
      j += en;
    }
    memcpy(json + j, "1", 1);
    j++;
    if (spaces[i] < 0x20 || spaces[i] == 0x7f)
      snprintf(json + j, sizeof json - j, "\\u%04x\"}}", spaces[i]);
    else {
      memcpy(json + j, enc, en);
      j += en;
      memcpy(json + j, "\"}}", 4);
    }
    (void)out;
    const char *keys[] = {"TM_COMMANDS"};
    const char *vals[] = {json};
    const char *popup[] = {"--no-reload"};
    r = run(f, popup, 1, keys, vals, 1);
    EXPECT(r.status == 0, "picker space U+%X %s", spaces[i], r.err);
    EXPECT(!exists_stat(sel), "picker reset U+%X", spaces[i]);
    result_free(&r);
    free(body);
    free(sel);
    fix_free(f);
  }
}

static void test_variables(void) {
  const char *input =
      "  $theme_font = Test Font\xC2\x85$theme_x = one\n$theme_x=two\xE2\x80"
      "\xA8ignored=three\n\x1c $theme_last = spaced \x1c";
  char **argv = xmalloc(3 * sizeof *argv);
  argv[0] = xstrdup(getenv("THEME_MENU_FIXTURE"));
  argv[1] = xstrdup("variables");
  argv[2] = NULL;
  extern char **environ;
  Result r = start_proc(argv, environ, input, 20000);
  finish_proc(&r, 20000);
  EXPECT(r.status == 0, "variables status");
  yyjson_doc *doc = yyjson_read(r.out, r.out_len, 0);
  EXPECT(doc, "variables json");
  if (doc) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *font = yyjson_obj_get(root, "theme_font");
    yyjson_val *x = yyjson_obj_get(root, "theme_x");
    yyjson_val *last = yyjson_obj_get(root, "theme_last");
    EXPECT(font && !strcmp(yyjson_get_str(font), "Test Font"), "theme_font");
    EXPECT(x && !strcmp(yyjson_get_str(x), "two"), "theme_x");
    EXPECT(last && !strcmp(yyjson_get_str(last), "spaced"), "theme_last");
    EXPECT(yyjson_obj_size(root) == 3, "variable count");
    yyjson_doc_free(doc);
  }
  result_free(&r);
  free_argv(argv);
  const char *rows[][5] = {
      {"000000", "eeeeee", "ffffff", "bbbbbb", "ffffff"},
      {"777777", "000000", "888888", "ffffff", "ffffff"},
      {"777777", "000000", "777777", "eeeeee", "000000"}};
  for (int i = 0; i < 3; i++) {
    argv = xmalloc(7 * sizeof *argv);
    argv[0] = xstrdup(getenv("THEME_MENU_FIXTURE"));
    argv[1] = xstrdup("selection-text");
    for (int k = 0; k < 4; k++)
      argv[2 + k] = xstrdup(rows[i][k]);
    argv[6] = NULL;
    r = start_proc(argv, environ, "", 20000);
    finish_proc(&r, 20000);
    while (r.out_len && (r.out[r.out_len - 1] == '\n' || r.out[r.out_len - 1] == '\r'))
      r.out[--r.out_len] = 0;
    EXPECT(r.out && !strcmp(r.out, rows[i][4]), "contrast %d", i);
    result_free(&r);
    free_argv(argv);
  }
}

static void test_catalogue(void) {
  const char *path = getenv("THEME_MENU_CATALOGUE");
  if (!path) {
    fprintf(stderr, "SKIP catalogue bundle check\n");
    return;
  }
  Fix *f = setup();
  if (!f)
    return;
  free(f->catalogue);
  f->catalogue = xstrdup(path);
  size_t n = 0;
  char *text = read_file(path, &n);
  yyjson_doc *doc = text ? yyjson_read(text, n, 0) : NULL;
  free(text);
  EXPECT(doc, "host catalogue");
  if (!doc) {
    fix_free(f);
    return;
  }
  yyjson_val *palettes =
      yyjson_obj_get(yyjson_doc_get_root(doc), "palettes");
  int count = 0;
  yyjson_val *key, *palette;
  size_t i, total;
  yyjson_obj_foreach(palettes, i, total, key, palette) {
    const char *id = yyjson_get_str(key);
    const char *extra[] = {"--no-reload", id};
    Result r = run(f, extra, 2, NULL, NULL, 0);
    EXPECT(r.status == 0, "%s: %s", id, r.err);
    result_free(&r);
    yyjson_val *native = yyjson_obj_get(palette, "nativeMode");
    const char *mode =
        native && yyjson_is_str(native) && yyjson_get_len(native) ? yyjson_get_str(native)
                                                                  : "dark";
    yyjson_val *groups[] = {
        yyjson_obj_get(palette, "files"),
        yyjson_obj_get(yyjson_obj_get(palette, "session"), mode)};
    for (int g = 0; g < 2; g++) {
      yyjson_val *name, *source;
      size_t j, gn;
      yyjson_obj_foreach(groups[g], j, gn, name, source) {
        const char *src = yyjson_get_str(source);
        if (src && strncmp(src, "nix-theme:", 10)) {
          char *active = join_path(f->state, "active");
          char *dest = join_path(active, yyjson_get_str(name));
          EXPECT(file_eq_path(dest, src), "%s:%s", id, yyjson_get_str(name));
          free(dest);
          free(active);
        }
      }
    }
    for (int m = 0; m < 2; m++) {
      char *mode_path = join_path(f->state, "mode");
      write_str(mode_path, m ? "light\n" : "dark\n");
      free(mode_path);
      clear_log(f);
      const char *keys[] = {"TM_COMMANDS"};
      const char *vals[] = {"{\"fuzzel\":{\"code\":2}}"};
      const char *popup[] = {"--no-reload"};
      Result popup_result = run(f, popup, 1, keys, vals, 1);
      EXPECT(popup_result.status == 0, "%s picker %s", id, popup_result.err);
      result_free(&popup_result);
      int ln = 0;
      Log *logs = read_logs(f, &ln);
      Log *menu = find_log(logs, ln, "fuzzel");
      char *ini = join_path(f->root, "picker.ini");
      if (menu && menu->settings)
        write_str(ini, menu->settings);
      const char *fuzzel = getenv("THEME_MENU_FUZZEL");
      if (fuzzel) {
        char **argv = xmalloc(5 * sizeof *argv);
        argv[0] = xstrdup(fuzzel);
        argv[1] = xstrdup("--config");
        argv[2] = xstrdup(ini);
        argv[3] = xstrdup("--check-config");
        argv[4] = NULL;
        extern char **environ;
        Result check = start_proc(argv, environ, "", 20000);
        finish_proc(&check, 20000);
        EXPECT(check.status == 0, "%s/%s: %s", id, m ? "light" : "dark",
               check.err);
        result_free(&check);
        free_argv(argv);
      }
      free(ini);
      logs_free(logs, ln);
      count++;
    }
  }
  printf("Validated %zu bundles and %d picker configurations\n",
         yyjson_obj_size(palettes), count);
  yyjson_doc_free(doc);
  fix_free(f);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  if (!getenv("THEME_MENU_BIN") || !getenv("THEME_MENU_FIXTURE")) {
    fprintf(stderr, "Set THEME_MENU_BIN and THEME_MENU_FIXTURE\n");
    return 1;
  }
  fake_bin = sibling("theme-menu-fake");
  if (!fake_bin || access(fake_bin, X_OK) != 0) {
    fprintf(stderr, "theme-menu-fake is missing\n");
    return 1;
  }
  test_parity();
  test_bundles();
  test_mode_actions();
  test_unknown();
  test_rollback();
  test_symlinks();
  test_solarized();
  test_popup();
  test_cancel();
  test_unicode_index();
  test_native_popup();
  test_lock();
  test_unpublished_wallpaper();
  test_published_wallpaper();
  test_wallpaper_override();
  test_greeter();
  test_hyprland();
  test_desktop_failures();
  test_gtk_read_error();
  test_equal_labels();
  test_btop();
  test_deadline();
  test_whitespace();
  test_variables();
  test_catalogue();
  free(fake_bin);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
