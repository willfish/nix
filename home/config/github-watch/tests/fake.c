#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yyjson.h>

static void write_all(int fd, const void *data, size_t n) {
  const char *p = data;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      _exit(1);
    }
    p += (size_t)w;
    n -= (size_t)w;
  }
}

static char *read_path(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  char *buf = NULL;
  size_t cap = 0, n = 0;
  char tmp[4096];
  size_t got;
  while ((got = fread(tmp, 1, sizeof tmp, f))) {
    if (n + got + 1 < n) {
      free(buf);
      fclose(f);
      return NULL;
    }
    if (n + got + 1 > cap) {
      size_t next = cap ? cap * 2 : 4096;
      while (next < n + got + 1)
        next *= 2;
      char *grown = realloc(buf, next);
      if (!grown) {
        free(buf);
        fclose(f);
        return NULL;
      }
      buf = grown;
      cap = next;
    }
    memcpy(buf + n, tmp, got);
    n += got;
  }
  fclose(f);
  if (!buf)
    buf = calloc(1, 1);
  if (!buf)
    return NULL;
  buf[n] = 0;
  if (len)
    *len = n;
  return buf;
}

static void append_doc(const char *path, yyjson_mut_doc *doc) {
  size_t n = 0;
  char *text = yyjson_mut_write(doc, 0, &n);
  if (!text)
    _exit(1);
  char *line = malloc(n + 2);
  if (!line)
    _exit(1);
  memcpy(line, text, n);
  line[n] = '\n';
  line[n + 1] = 0;
  int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0)
    _exit(1);
  write_all(fd, line, n + 1);
  close(fd);
  free(line);
  free(text);
}

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static void log_call(const char *kind, int argc, char **argv) {
  const char *path = getenv("FAKE_LOG");
  if (!path)
    _exit(1);
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_strcpy(doc, root, "kind", kind);
  yyjson_mut_val *args = yyjson_mut_arr(doc);
  for (int i = 1; i < argc; i++)
    yyjson_mut_arr_add_strcpy(doc, args, argv[i]);
  yyjson_mut_obj_add_val(doc, root, "args", args);
  yyjson_mut_obj_add_int(doc, root, "pid", (int)getpid());
  append_doc(path, doc);
  yyjson_mut_doc_free(doc);
}

static void finish_notify(yyjson_val *db) {
  yyjson_val *action = yyjson_obj_get(db, "action");
  if (action && yyjson_is_str(action) && yyjson_get_len(action))
    write_all(STDOUT_FILENO, yyjson_get_str(action), yyjson_get_len(action));
  write_all(STDERR_FILENO, "fake-private-diagnostic", 23);
  yyjson_val *code = yyjson_obj_get(db, "notifyExit");
  _exit(code && yyjson_is_num(code) ? yyjson_get_int(code) : 0);
}

int main(int argc, char **argv) {
  const char *kind = base_name(argv[0]);
  const char *db_path = getenv("FAKE_DB");
  size_t db_len = 0;
  char *db_text = db_path ? read_path(db_path, &db_len) : NULL;
  yyjson_doc *db_doc = db_text ? yyjson_read(db_text, db_len, 0) : NULL;
  yyjson_val *db = db_doc ? yyjson_doc_get_root(db_doc) : NULL;
  if (!db || !yyjson_is_obj(db))
    _exit(1);
  log_call(kind, argc, argv);
  if (!strcmp(kind, "gh")) {
    if (yyjson_get_bool(yyjson_obj_get(db, "ghError")))
      write_all(STDERR_FILENO, "fake-private-diagnostic", 23);
    yyjson_val *raw = yyjson_obj_get(db, "raw");
    if (raw && yyjson_is_str(raw)) {
      if (yyjson_get_len(raw))
        write_all(STDOUT_FILENO, yyjson_get_str(raw), yyjson_get_len(raw));
    } else {
      yyjson_val *items = yyjson_obj_get(db, "items");
      yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
      yyjson_mut_val *copy =
          items ? yyjson_val_mut_copy(out, items) : yyjson_mut_arr(out);
      yyjson_mut_doc_set_root(out, copy);
      size_t n = 0;
      char *text = yyjson_mut_write(out, 0, &n);
      if (text && n)
        write_all(STDOUT_FILENO, text, n);
      free(text);
      yyjson_mut_doc_free(out);
    }
    yyjson_val *code = yyjson_obj_get(db, "ghExit");
    int status = code && yyjson_is_num(code) ? yyjson_get_int(code) : 0;
    yyjson_doc_free(db_doc);
    free(db_text);
    _exit(status);
  }
  if (!strcmp(kind, "notify-send")) {
    if (yyjson_get_bool(yyjson_obj_get(db, "waitForState"))) {
      const char *state = getenv("FAKE_STATE");
      const char *log = getenv("FAKE_LOG");
      for (int tries = 0; tries <= 50; tries++) {
        usleep(10000);
        if (state && access(state, F_OK) == 0) {
          size_t n = 0;
          char *text = read_path(state, &n);
          yyjson_doc *seen_doc = text ? yyjson_read(text, n, 0) : NULL;
          yyjson_val *seen = seen_doc ? yyjson_obj_get(yyjson_doc_get_root(seen_doc), "seen") : NULL;
          yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
          yyjson_mut_val *root = yyjson_mut_obj(out);
          yyjson_mut_doc_set_root(out, root);
          yyjson_mut_obj_add_strcpy(out, root, "kind", "state-before-action");
          yyjson_mut_obj_add_val(out, root, "seen",
                                 seen ? yyjson_val_mut_copy(out, seen) : yyjson_mut_arr(out));
          if (log)
            append_doc(log, out);
          yyjson_mut_doc_free(out);
          yyjson_doc_free(seen_doc);
          free(text);
          finish_notify(db);
        }
      }
      _exit(9);
    }
    finish_notify(db);
  }
  if (!strcmp(kind, "xdg-open"))
    _exit(0);
  yyjson_doc_free(db_doc);
  free(db_text);
  return 0;
}
