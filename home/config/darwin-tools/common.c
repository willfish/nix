#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "tools.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
Val *field(Val *v, const char *key) {
  Val *k, *x, *found = NULL;
  size_t i, n;
  yyjson_obj_foreach(v, i, n, k, x) if (yyjson_get_len(k) == strlen(key) &&
                                        !memcmp(yyjson_get_str(k), key,
                                                strlen(key))) found = x;
  return found;
}
const char *text(Val *v) {
  return yyjson_is_str(v) && strlen(yyjson_get_str(v)) == yyjson_get_len(v)
             ? yyjson_get_str(v)
             : NULL;
}
char **strings(Val *v) {
  if (!yyjson_is_arr(v))
    return NULL;
  char **out = g_new0(char *, yyjson_arr_size(v) + 1);
  Val *item;
  size_t i, n;
  yyjson_arr_foreach(v, i, n, item) {
    const char *s = text(item);
    if (!s) {
      g_strfreev(out);
      return NULL;
    }
    out[i] = g_strdup(s);
  }
  return out;
}
bool exists(const char *p) {
  struct stat st;
  return p && stat(p, &st) == 0;
}
bool is_file(const char *p) {
  struct stat st;
  return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
bool is_link(const char *p) {
  struct stat st;
  return p && lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}
static char *resolve_inner(const char *path, bool strict, unsigned depth) {
  if (!path)
    return NULL;
  char *real = realpath(path, NULL);
  if (real) {
    char *out = g_strdup(real);
    free(real);
    return out;
  }
  if (strict)
    return NULL;
  char *cur = *path == '/' ? g_strdup("/") : g_get_current_dir();
  char **parts = g_strsplit(path, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    char *next = !strcmp(parts[i], "..")
                     ? g_path_get_dirname(cur)
                     : g_build_filename(cur, parts[i], NULL);
    g_free(cur);
    real = realpath(next, NULL);
    if (real)
      cur = g_strdup(real);
    else if (depth < 40 && is_link(next)) {
      char *link = g_file_read_link(next, NULL),
           *parent = g_path_get_dirname(next),
           *target = link && *link == '/' ? g_strdup(link)
                     : link               ? g_build_filename(parent, link, NULL)
                                          : NULL;
      cur = target ? resolve_inner(target, false, depth + 1) : g_strdup(next);
      g_free(link);
      g_free(parent);
      g_free(target);
    } else
      cur = g_strdup(next);
    free(real);
    g_free(next);
  }
  g_strfreev(parts);
  return cur;
}
char *resolve_path(const char *path, bool strict) {
  return resolve_inner(path, strict, 0);
}
char *linked(const char *path) {
  return is_link(path) ? resolve_path(path, false) : NULL;
}
static bool mkdir_mode(const char *path, mode_t mode) {
  if (mkdir(path, mode) == 0)
    return true;
  struct stat st;
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
    return true;
  if (errno != ENOENT)
    return false;
  char *parent = g_path_get_dirname(path);
  bool ok = strcmp(parent, path) && mkdir_mode(parent, 0777);
  g_free(parent);
  return ok && (mkdir(path, mode) == 0 ||
                (stat(path, &st) == 0 && S_ISDIR(st.st_mode)));
}
bool make_parent(const char *path, mode_t mode) {
  char *p = g_path_get_dirname(path);
  bool ok = mkdir_mode(p, mode);
  g_free(p);
  return ok;
}
yyjson_doc *load(const char *path) {
  char *s = NULL;
  gsize n;
  if (!path || !g_file_get_contents(path, &s, &n, NULL))
    return NULL;
  yyjson_doc *d = yyjson_read(s, n, 0);
  g_free(s);
  return d;
}
char *json(Doc *doc, bool pretty) {
  return yyjson_mut_write(doc,
                          YYJSON_WRITE_ESCAPE_UNICODE |
                              (pretty ? YYJSON_WRITE_PRETTY_TWO_SPACES : 0),
                          NULL);
}
bool atomic_file(const char *path, const void *data, size_t length,
                 const char *prefix) {
  char *parent = g_path_get_dirname(path),
       *temp = g_strconcat(parent, "/", prefix, "XXXXXX", NULL);
  g_free(parent);
  int fd = g_mkstemp_full(temp, O_WRONLY | O_CLOEXEC, 0600);
  bool made = fd >= 0, ok = made;
  for (size_t at = 0; ok && at < length;) {
    ssize_t n = write(fd, (const char *)data + at, length - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      ok = false;
    else
      at += (size_t)n;
  }
  if (fd >= 0 && close(fd) != 0)
    ok = false;
  if (ok && rename(temp, path) != 0)
    ok = false;
  if (made && !ok)
    unlink(temp);
  g_free(temp);
  return ok;
}
static gint key_order(gconstpointer a, gconstpointer b) {
  return strcmp(yyjson_get_str(*(Val *const *)a),
                yyjson_get_str(*(Val *const *)b));
}
Mut *copy_sorted(Doc *doc, Val *v) {
  if (yyjson_is_obj(v)) {
    GPtrArray *keys = g_ptr_array_new();
    Val *k, *item;
    size_t i, n;
    yyjson_obj_foreach(v, i, n, k, item) g_ptr_array_add(keys, k);
    g_ptr_array_sort(keys, key_order);
    Mut *out = yyjson_mut_obj(doc);
    for (i = 0; i < keys->len; i++) {
      k = keys->pdata[i];
      yyjson_mut_obj_add(out, yyjson_val_mut_copy(doc, k),
                         copy_sorted(doc, yyjson_obj_getn(v, yyjson_get_str(k),
                                                          yyjson_get_len(k))));
    }
    g_ptr_array_free(keys, TRUE);
    return out;
  }
  if (yyjson_is_arr(v)) {
    Mut *out = yyjson_mut_arr(doc);
    Val *item;
    size_t i, n;
    yyjson_arr_foreach(v, i, n, item)
        yyjson_mut_arr_add_val(out, copy_sorted(doc, item));
    return out;
  }
  return yyjson_val_mut_copy(doc, v);
}
typedef struct {
  GMainLoop *loop;
  GSubprocess *child;
  GCancellable *cancel;
  GBytes *out;
  bool ok, timed_out;
} Pending;
static void communicated(GObject *object, GAsyncResult *result, gpointer data) {
  Pending *p = data;
  p->ok = g_subprocess_communicate_finish(G_SUBPROCESS(object), result, &p->out,
                                          NULL, NULL);
  g_main_loop_quit(p->loop);
}
static gboolean expired(gpointer data) {
  Pending *p = data;
  p->timed_out = true;
  g_subprocess_force_exit(p->child);
  g_cancellable_cancel(p->cancel);
  return G_SOURCE_REMOVE;
}
Result command(void *context, const char *const *argv, unsigned timeout,
               bool capture) {
  (void)context;
  Result r = {124, true, g_string_new(NULL)};
  GSubprocess *child =
      g_subprocess_newv(argv,
                        G_SUBPROCESS_FLAGS_STDIN_INHERIT |
                            (capture ? G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                           G_SUBPROCESS_FLAGS_STDERR_SILENCE
                                     : G_SUBPROCESS_FLAGS_NONE),
                        NULL);
  if (!child)
    return r;
  if (!capture) {
    bool ok = g_subprocess_wait(child, NULL, NULL);
    r.error = !ok;
    r.code = ok && g_subprocess_get_if_exited(child)
                 ? g_subprocess_get_exit_status(child)
                 : 1;
    g_object_unref(child);
    return r;
  }
  Pending p = {g_main_loop_new(NULL, FALSE),
               child,
               g_cancellable_new(),
               NULL,
               false,
               false};
  guint timer = g_timeout_add(timeout, expired, &p);
  g_subprocess_communicate_async(child, NULL, p.cancel, communicated, &p);
  g_main_loop_run(p.loop);
  if (!p.timed_out)
    g_source_remove(timer);
  else
    g_subprocess_wait(child, NULL, NULL);
  if (p.ok && !p.timed_out) {
    gsize n;
    const char *out = g_bytes_get_data(p.out, &n);
    if (g_utf8_validate(out, (gssize)n, NULL)) {
      for (gsize i = 0; i < n; i++) {
        if (out[i] == '\r') {
          g_string_append_c(r.out, '\n');
          if (i + 1 < n && out[i + 1] == '\n')
            i++;
        } else
          g_string_append_c(r.out, out[i]);
      }
      r.error = false;
      r.code = g_subprocess_get_if_exited(child)
                   ? g_subprocess_get_exit_status(child)
                   : 1;
    }
  }
  if (p.out)
    g_bytes_unref(p.out);
  g_object_unref(p.cancel);
  g_main_loop_unref(p.loop);
  g_object_unref(child);
  return r;
}
void result_free(Result *r) { g_string_free(r->out, TRUE); }
