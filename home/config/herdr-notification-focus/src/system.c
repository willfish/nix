#define _DEFAULT_SOURCE
#include "focus.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static int path_compare(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
GPtrArray *socket_paths(void) {
  GPtrArray *candidates = g_ptr_array_new_with_free_func(g_free),
            *out = g_ptr_array_new_with_free_func(g_free);
  char *home = home_path();
  const char *xdg = g_getenv("XDG_CONFIG_HOME"),
             *override = g_getenv("HERDR_SOCKET_PATH");
  char *config = xdg && *xdg ? normal_path(xdg)
                             : g_build_filename(home, ".config", NULL),
       *root = g_build_filename(config, "herdr", NULL),
       *sessions = g_build_filename(root, "sessions", NULL);
  if (override && *override)
    g_ptr_array_add(candidates, normal_path(override));
  g_ptr_array_add(candidates, g_build_filename(root, "herdr.sock", NULL));
  GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
  GDir *dir = g_dir_open(sessions, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name(dir)))
    g_ptr_array_add(found,
                    g_build_filename(sessions, name, "herdr.sock", NULL));
  if (dir)
    g_dir_close(dir);
  g_ptr_array_sort(found, path_compare);
  for (size_t i = 0; i < found->len; i++)
    g_ptr_array_add(candidates, g_strdup(found->pdata[i]));
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (size_t i = 0; i < candidates->len; i++) {
    char *p = normal_path(candidates->pdata[i]);
    if (!g_hash_table_contains(seen, p) && g_file_test(p, G_FILE_TEST_EXISTS)) {
      g_ptr_array_add(out, p);
      g_hash_table_add(seen, p);
    } else
      g_free(p);
  }
  g_hash_table_destroy(seen);
  g_ptr_array_free(candidates, true);
  g_ptr_array_free(found, true);
  g_free(home);
  g_free(config);
  g_free(root);
  g_free(sessions);
  return out;
}
static bool wait_fd(int fd, short events, gint64 deadline) {
  for (;;) {
    gint64 left = deadline - g_get_monotonic_time();
    if (left <= 0)
      return false;
    struct pollfd p = {fd, events, 0};
    int result = poll(&p, 1, (int)MIN((left + 999) / 1000, G_MAXINT));
    if (result < 0 && errno == EINTR)
      continue;
    return result > 0 && (p.revents & (events | POLLHUP | POLLERR));
  }
}
yyjson_doc *herdr_call(const char *path, const char *method, Val *params) {
  int fd = -1;
  char *payload = NULL;
  GByteArray *data = g_byte_array_new();
  yyjson_doc *out = NULL;
  Doc *doc = empty_object();
  Mut *root = yyjson_mut_doc_get_root(doc);
  yyjson_mut_obj_add_str(doc, root, "id", "omapager-herdr-focus");
  yyjson_mut_obj_add_strcpy(doc, root, "method", method);
  member(doc, root, "params", params);
  if (!params) {
    yyjson_mut_obj_remove_key(root, "params");
    yyjson_mut_obj_add_obj(doc, root, "params");
  }
  char *s = json(doc);
  yyjson_mut_doc_free(doc);
  if (!s)
    goto done;
  payload = g_strconcat(s, "\n", NULL);
  g_free(s);
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof address.sun_path)
    goto done;
  memcpy(address.sun_path, path, strlen(path) + 1);
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    goto done;
  gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
  if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0) {
    if (errno != EINPROGRESS || !wait_fd(fd, POLLOUT, deadline))
      goto done;
    int error;
    socklen_t size = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
      goto done;
  }
  size_t sent = 0, length = strlen(payload);
  deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
  while (sent < length) {
    ssize_t n = send(fd, payload + sent, length - sent, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      if (!wait_fd(fd, POLLOUT, deadline))
        goto done;
      continue;
    }
    if (n <= 0)
      goto done;
    sent += (size_t)n;
  }
  unsigned char buffer[65536];
  size_t newline = 0;
  bool complete = false;
  while (!complete) {
    deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    if (!wait_fd(fd, POLLIN, deadline))
      goto done;
    ssize_t n = recv(fd, buffer, sizeof buffer, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (n < 0)
      goto done;
    if (!n)
      break;
    unsigned char *line = memchr(buffer, '\n', (size_t)n);
    if (line) {
      newline = data->len + (size_t)(line - buffer);
      complete = true;
    }
    g_byte_array_append(data, buffer, (guint)n);
  }
  if (!data->len)
    goto empty;
  size_t n = complete ? newline : data->len;
  unsigned char *bytes = data->data;
  if (n >= 3 && !memcmp(bytes, "\xef\xbb\xbf", 3)) {
    bytes += 3;
    n -= 3;
  }
  out = yyjson_read((const char *)bytes, n,
                    YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_BIGNUM_AS_RAW);
  if (!out)
    goto done;
  goto empty;
done:
  focus_error = true;
empty:
  if (fd >= 0)
    close(fd);
  g_free(payload);
  g_byte_array_unref(data);
  return out;
}
Val *snapshot_from(yyjson_doc *response) {
  Val *root = response ? yyjson_doc_get_root(response) : NULL;
  if (truth(root) && !yyjson_is_obj(root)) {
    focus_error = true;
    return NULL;
  }
  Val *result = field(root, "result");
  if (truth(result) && !yyjson_is_obj(result)) {
    focus_error = true;
    return NULL;
  }
  Val *snapshot = field(result, "snapshot");
  return yyjson_is_obj(snapshot) ? snapshot : NULL;
}
char *parent_pid(const char *pid, const char *proc) {
  if (!strcmp(pid, "0"))
    return g_strdup("0");
  char *path = g_build_filename(proc, pid, "stat", NULL), *text = NULL;
  bool read = g_file_get_contents(path, &text, NULL, NULL);
  g_free(path);
  if (!read)
    return g_strdup("0");
  char *last = strrchr(text, ')'), *out = NULL;
  if (last && strlen(last) >= 2) {
    char **parts = g_strsplit_set(last + 2, " \t\n\r\v\f", -1);
    size_t count = 0;
    for (size_t i = 0; parts[i]; i++)
      if (*parts[i]) {
        if (++count == 2) {
          bool saved = focus_error;
          out = integer_text(parts[i]);
          focus_error = saved;
          break;
        }
      }
    g_strfreev(parts);
  }
  g_free(text);
  return out ? out : g_strdup("0");
}
GPtrArray *ancestor_pids(const char *start, const char *proc) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  char *pid = integer_text(start);
  while (pid && strcmp(pid, "0") && !g_hash_table_contains(seen, pid) &&
         out->len < 32) {
    g_ptr_array_add(out, pid);
    g_hash_table_add(seen, pid);
    pid = parent_pid(pid, proc);
  }
  g_free(pid);
  g_hash_table_destroy(seen);
  return out;
}
static char *session_name(const char *path) {
  char *normal = normal_path(path);
  char **parts = g_strsplit(normal, "/", -1);
  char *session = NULL;
  size_t count = g_strv_length(parts);
  for (size_t i = 0; i < count; i++)
    if (!strcmp(parts[i], "sessions")) {
      if (i + 1 < count - 1)
        session = g_strdup(parts[i + 1]);
      break;
    }
  g_free(normal);
  g_strfreev(parts);
  return session;
}
GPtrArray *attached_pids(const char *path, const char *proc) {
  char *session = session_name(path);
  GPtrArray *matched = g_ptr_array_new_with_free_func(g_free),
            *fallback = g_ptr_array_new_with_free_func(g_free);
  GDir *dir = g_dir_open(proc, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name(dir))) {
    bool numeric = *name;
    for (const char *p = name; *p; p++)
      if (!g_ascii_isdigit(*p))
        numeric = false;
    if (!numeric)
      continue;
    char *file = g_build_filename(proc, name, "cmdline", NULL), *raw = NULL;
    gsize length;
    bool read = g_file_get_contents(file, &raw, &length, NULL);
    g_free(file);
    if (!read)
      continue;
    for (size_t i = 0; i < length; i++)
      if (!raw[i])
        raw[i] = ' ';
    GString *text = g_string_new(NULL);
    for (const char *p = raw, *end = raw + length; p < end;) {
      gunichar c = g_utf8_get_char_validated(p, (gssize)(end - p));
      if (c == (gunichar)-1 || c == (gunichar)-2) {
        p++;
        continue;
      }
      g_string_append_unichar(text, c);
      p = g_utf8_next_char(p);
    }
    g_free(raw);
    if (strstr(text->str, "herdr") && strstr(text->str, "session attach")) {
      g_ptr_array_add(fallback, integer_text(name));
      char *needle =
          g_strconcat("session attach ", session ? session : "default", NULL);
      if (strstr(text->str, needle))
        g_ptr_array_add(matched, integer_text(name));
      g_free(needle);
    }
    g_string_free(text, true);
  }
  if (dir)
    g_dir_close(dir);
  g_free(session);
  GPtrArray *out = matched->len ? matched : fallback;
  g_ptr_array_free(matched->len ? fallback : matched, true);
  return out;
}
typedef struct {
  GSubprocess *child;
  GCancellable *cancel;
  GMainLoop *loop;
  GBytes *output, *error;
  bool ok, timed_out;
  guint timer;
} Command;
static gboolean expired(void *data) {
  Command *c = data;
  c->timer = 0;
  c->timed_out = true;
  g_subprocess_force_exit(c->child);
  g_cancellable_cancel(c->cancel);
  return G_SOURCE_REMOVE;
}
static void completed(GObject *source, GAsyncResult *result, void *data) {
  Command *c = data;
  GError *error = NULL;
  c->ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result,
                                          &c->output, &c->error, &error);
  GBytes *buffers[] = {c->output, c->error};
  for (size_t i = 0; i < 2; i++)
    if (buffers[i]) {
      gsize n;
      const char *s = g_bytes_get_data(buffers[i], &n);
      if (!utf8_bytes(s, n)) {
        focus_error = true;
        c->ok = false;
      }
    }
  g_clear_error(&error);
  if (c->timer)
    g_source_remove(c->timer);
  g_subprocess_wait(c->child, NULL, NULL);
  g_main_loop_quit(c->loop);
}
static bool command(const char *const *argv, bool capture, GBytes **output,
                    int *status) {
  GError *error = NULL;
  GSubprocess *child = g_subprocess_newv(
      argv,
      capture ? G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE
              : G_SUBPROCESS_FLAGS_NONE,
      &error);
  g_clear_error(&error);
  if (!child)
    return false;
  Command c = {.child = child,
               .cancel = g_cancellable_new(),
               .loop = g_main_loop_new(NULL, false)};
  c.timer = g_timeout_add(2000, expired, &c);
  g_subprocess_communicate_async(child, NULL, c.cancel, completed, &c);
  g_main_loop_run(c.loop);
  if (status)
    *status = g_subprocess_get_if_exited(child)
                  ? g_subprocess_get_exit_status(child)
                  : -1;
  if (output)
    *output = g_steal_pointer(&c.output);
  if (c.output)
    g_bytes_unref(c.output);
  if (c.error)
    g_bytes_unref(c.error);
  g_main_loop_unref(c.loop);
  g_object_unref(c.cancel);
  g_object_unref(child);
  return c.ok && !c.timed_out;
}
yyjson_doc *hypr_clients(void) {
  const char *argv[] = {"hyprctl", "clients", "-j", NULL};
  GBytes *output = NULL;
  int status;
  yyjson_doc *doc = NULL;
  if (command(argv, true, &output, &status) && status == 0 && output) {
    gsize n;
    const char *s = g_bytes_get_data(output, &n);
    doc = yyjson_read(
        s, n, YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_BIGNUM_AS_RAW);
    if (doc && !yyjson_is_arr(yyjson_doc_get_root(doc))) {
      yyjson_doc_free(doc);
      doc = NULL;
    }
  }
  if (output)
    g_bytes_unref(output);
  return doc;
}
bool focus_address(Val *address) {
  char *s = string(address);
  if (focus_error) {
    g_free(s);
    return false;
  }
  char *argument = g_strconcat("address:", s, NULL);
  g_free(s);
  const char *argv[] = {"hyprctl", "dispatch", "focuswindow", argument, NULL};
  bool ok = command(argv, false, NULL, NULL);
  g_free(argument);
  if (!ok)
    focus_error = true;
  return ok;
}
