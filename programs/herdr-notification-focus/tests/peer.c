#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

static volatile sig_atomic_t stop_flag;
static void on_stop(int sig) {
  (void)sig;
  stop_flag = 1;
}
static int write_all(int fd, const void *data, size_t n) {
  const unsigned char *p = data;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return -1;
    p += (size_t)w;
    n -= (size_t)w;
  }
  return 0;
}
static void pause_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}
static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END)) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return NULL;
  }
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[n] = 0;
  fclose(f);
  *len = (size_t)n;
  return buf;
}
static int log_call(const char *path, const char *sock, yyjson_val *req) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_strcpy(doc, root, "path", sock);
  yyjson_val *id = yyjson_obj_get(req, "id");
  yyjson_val *method = yyjson_obj_get(req, "method");
  yyjson_val *params = yyjson_obj_get(req, "params");
  if (yyjson_is_str(id))
    yyjson_mut_obj_add_strncpy(doc, root, "id", yyjson_get_str(id),
                               yyjson_get_len(id));
  if (yyjson_is_str(method))
    yyjson_mut_obj_add_strncpy(doc, root, "method", yyjson_get_str(method),
                               yyjson_get_len(method));
  if (params)
    yyjson_mut_obj_add_val(doc, root, "params", yyjson_val_mut_copy(doc, params));
  size_t len = 0;
  char *text = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_free(doc);
  if (!text)
    return -1;
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    free(text);
    return -1;
  }
  int rc = write_all(fd, text, len) || write_all(fd, "\n", 1);
  free(text);
  close(fd);
  return rc;
}
static char *response_for(yyjson_val *req, const char *snapshot, size_t snap_len,
                          size_t *out_len) {
  yyjson_val *method = yyjson_obj_get(req, "method");
  const char *name = yyjson_is_str(method) ? yyjson_get_str(method) : "";
  if (strcmp(name, "session.snapshot")) {
    const char *ok = "{\"result\":{\"type\":\"ok\"}}";
    *out_len = strlen(ok);
    return strdup(ok);
  }
  const char *prefix = "{\"result\":{\"snapshot\":";
  const char *suffix = "}}";
  size_t n = strlen(prefix) + snap_len + strlen(suffix);
  char *out = malloc(n + 1);
  if (!out)
    return NULL;
  memcpy(out, prefix, strlen(prefix));
  memcpy(out + strlen(prefix), snapshot, snap_len);
  memcpy(out + strlen(prefix) + snap_len, suffix, strlen(suffix) + 1);
  *out_len = n;
  return out;
}
static void serve_one(int cfd, const char *mode, const char *sock,
                      const char *calls, const char *snapshot, size_t snap_len) {
  char buf[65536];
  size_t n = 0;
  while (n + 1 < sizeof buf) {
    ssize_t got = read(cfd, buf + n, sizeof buf - 1 - n);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return;
    n += (size_t)got;
    buf[n] = 0;
    if (memchr(buf, '\n', n))
      break;
  }
  char *nl = memchr(buf, '\n', n);
  if (!nl)
    return;
  yyjson_doc *doc = yyjson_read(buf, (size_t)(nl - buf), 0);
  if (!doc)
    return;
  log_call(calls, sock, yyjson_doc_get_root(doc));
  if (!strcmp(mode, "stall")) {
    while (!stop_flag) {
      struct pollfd p = {cfd, POLLIN, 0};
      if (poll(&p, 1, 200) > 0)
        break;
    }
    yyjson_doc_free(doc);
    return;
  }
  if (!strcmp(mode, "empty")) {
    yyjson_doc_free(doc);
    return;
  }
  if (!strcmp(mode, "bad")) {
    write_all(cfd, "malformed\n", 10);
    yyjson_doc_free(doc);
    return;
  }
  size_t len = 0;
  char *body = response_for(yyjson_doc_get_root(doc), snapshot, snap_len, &len);
  yyjson_doc_free(doc);
  if (!body)
    return;
  if (!strcmp(mode, "chunks") || !strcmp(mode, "progress")) {
    size_t step = !strcmp(mode, "progress") ? 50 : 10000;
    int delay = !strcmp(mode, "progress") ? 600 : 15;
    size_t at = 0;
    for (;;) {
      pause_ms(delay);
      if (stop_flag || at >= len)
        break;
      size_t nwrite = len - at > step ? step : len - at;
      if (write_all(cfd, body + at, nwrite))
        break;
      at += nwrite;
    }
    if (!stop_flag)
      write_all(cfd, "\nignored", 8);
  } else if (!strcmp(mode, "eof"))
    write_all(cfd, body, len);
  else if (!strcmp(mode, "bom")) {
    unsigned char bom[] = {0xef, 0xbb, 0xbf};
    write_all(cfd, bom, sizeof bom);
    write_all(cfd, body, len);
    write_all(cfd, "\nignored", 8);
  } else {
    write_all(cfd, body, len);
    write_all(cfd, "\nignored", 8);
  }
  free(body);
}
int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  const char *sock = argv[1], *mode = argv[2], *snap_path = argv[3],
             *calls = argv[4];
  size_t snap_len = 0;
  char *snapshot = read_file(snap_path, &snap_len);
  if (!snapshot)
    snapshot = strdup("null");
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa = {.sa_handler = on_stop};
  sigaction(SIGTERM, &sa, NULL);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return 1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (strlen(sock) >= sizeof addr.sun_path)
    return 1;
  memcpy(addr.sun_path, sock, strlen(sock) + 1);
  unlink(sock);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 8))
    return 1;
  while (!stop_flag) {
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 200) <= 0)
      continue;
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0)
      continue;
    serve_one(cfd, mode, sock, calls, snapshot, snap_len);
    close(cfd);
  }
  close(fd);
  unlink(sock);
  free(snapshot);
  return 0;
}
