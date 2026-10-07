#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    perror("malloc");
    exit(1);
  }
  return p;
}

static char *read_fd(int fd, size_t *len) {
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
  *len = n;
  return buf;
}

static char *read_path(const char *path, size_t *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL;
  char *data = read_fd(fd, len);
  close(fd);
  return data;
}

static void write_all(int fd, const char *data, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t wrote = write(fd, data + off, n - off);
    if (wrote < 0) {
      if (errno == EINTR)
        continue;
      perror("write");
      exit(1);
    }
    off += (size_t)wrote;
  }
}

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static void append_log(const char *path, const char *json) {
  int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0) {
    perror(path);
    exit(1);
  }
  size_t n = strlen(json);
  char *line = xmalloc(n + 1);
  memcpy(line, json, n);
  line[n] = '\n';
  write_all(fd, line, n + 1);
  free(line);
  close(fd);
}

static yyjson_val *lookup(yyjson_val *table, const char *name, int argc,
                          char **argv) {
  size_t n = strlen(name) + 1;
  for (int i = 1; i < argc; i++)
    n += strlen(argv[i]) + 1;
  char *key = xmalloc(n + 1);
  size_t at = 0;
  memcpy(key + at, name, strlen(name));
  at += strlen(name);
  key[at++] = ' ';
  for (int i = 1; i < argc; i++) {
    if (i > 1)
      key[at++] = ' ';
    size_t len = strlen(argv[i]);
    memcpy(key + at, argv[i], len);
    at += len;
  }
  key[at] = 0;
  yyjson_val *found = yyjson_obj_get(table, key);
  if (!found)
    found = yyjson_obj_get(table, name);
  free(key);
  return found;
}

int main(int argc, char **argv) {
  const char *name = base_name(argv[0] ? argv[0] : "");
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc)
    return 1;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_strcpy(doc, root, "name", name);
  yyjson_mut_val *args = yyjson_mut_arr(doc);
  yyjson_mut_obj_add_val(doc, root, "args", args);
  for (int i = 1; i < argc; i++)
    yyjson_mut_arr_add_strcpy(doc, args, argv[i]);
  if (!strcmp(name, "fuzzel")) {
    size_t n = 0;
    char *input = read_fd(STDIN_FILENO, &n);
    yyjson_mut_obj_add_strncpy(doc, root, "input", input, n);
    free(input);
    const char *config = NULL;
    for (int i = 1; i + 1 < argc; i++)
      if (!strcmp(argv[i], "--config"))
        config = argv[i + 1];
    if (!config)
      return 1;
    size_t sn = 0;
    char *settings = read_path(config, &sn);
    if (!settings)
      return 1;
    yyjson_mut_obj_add_strncpy(doc, root, "settings", settings, sn);
    free(settings);
  }
  char *line = yyjson_mut_write(doc, 0, NULL);
  yyjson_mut_doc_free(doc);
  if (!line)
    return 1;
  const char *log = getenv("TM_LOG");
  if (log)
    append_log(log, line);
  free(line);

  const char *raw = getenv("TM_COMMANDS");
  if (!raw)
    raw = "{}";
  yyjson_doc *table_doc = yyjson_read(raw, strlen(raw), 0);
  if (!table_doc)
    return 1;
  yyjson_val *table = yyjson_doc_get_root(table_doc);
  yyjson_val *response = yyjson_is_obj(table) ? lookup(table, name, argc, argv)
                                              : NULL;
  yyjson_val *mode = response ? yyjson_obj_get(response, "writeMode") : NULL;
  if (mode && yyjson_is_str(mode)) {
    const char *state = getenv("TM_STATE");
    if (!state)
      return 1;
    size_t sl = strlen(state), ml = yyjson_get_len(mode);
    char *path = xmalloc(sl + 6);
    memcpy(path, state, sl);
    memcpy(path + sl, "/mode", 6);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    free(path);
    if (fd < 0)
      return 1;
    write_all(fd, yyjson_get_str(mode), ml);
    write_all(fd, "\n", 1);
    close(fd);
  }
  if (!strcmp(name, "nix") && getenv("TM_WALLPAPER")) {
    for (int i = 1; i + 1 < argc; i++) {
      if (!strcmp(argv[i], "--out-link")) {
        unlink(argv[i + 1]);
        if (symlink(getenv("TM_WALLPAPER"), argv[i + 1]) < 0)
          return 1;
      }
    }
  }
  long delay = 0;
  yyjson_val *delay_val = response ? yyjson_obj_get(response, "delay") : NULL;
  if (delay_val && yyjson_is_num(delay_val))
    delay = (long)yyjson_get_num(delay_val);
  if (delay > 0) {
    struct timespec ts = {.tv_sec = delay / 1000,
                          .tv_nsec = (delay % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
      ;
  }
  yyjson_val *err = response ? yyjson_obj_get(response, "err") : NULL;
  if (err && yyjson_is_str(err) && yyjson_get_len(err))
    write_all(STDERR_FILENO, yyjson_get_str(err), yyjson_get_len(err));
  yyjson_val *out = response ? yyjson_obj_get(response, "out") : NULL;
  if (out && yyjson_is_str(out) && yyjson_get_len(out))
    write_all(STDOUT_FILENO, yyjson_get_str(out), yyjson_get_len(out));
  else if (!response && !strcmp(name, "gdbus"))
    write_all(STDOUT_FILENO, "(false,)", 8);
  int code = 0;
  yyjson_val *code_val = response ? yyjson_obj_get(response, "code") : NULL;
  if (code_val && yyjson_is_num(code_val))
    code = (int)yyjson_get_num(code_val);
  yyjson_doc_free(table_doc);
  return code;
}
