#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char payload[] = "speech model fixture\n";
static volatile sig_atomic_t stop_flag;

static void on_stop(int sig) {
  (void)sig;
  stop_flag = 1;
}
static int write_all_fd(int fd, const void *data, size_t n) {
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
static int ssl_write_all(SSL *s, const void *data, size_t n) {
  const unsigned char *p = data;
  while (n) {
    int chunk = n > 16384 ? 16384 : (int)n;
    int w = SSL_write(s, p, chunk);
    if (w <= 0)
      return -1;
    p += (size_t)w;
    n -= (size_t)w;
  }
  return 0;
}
static int listen_local(int *port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 16)) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof addr;
  if (getsockname(fd, (struct sockaddr *)&addr, &len)) {
    close(fd);
    return -1;
  }
  *port = ntohs(addr.sin_port);
  return fd;
}
static int header_value(const char *req, size_t n, const char *name, char *out,
                        size_t cap) {
  size_t name_len = strlen(name);
  const char *p = req, *end = req + n;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t line = nl ? (size_t)(nl - p) : (size_t)(end - p);
    if (line && p[line - 1] == '\r')
      line--;
    if (!line)
      break;
    if (line > name_len && p[name_len] == ':' &&
        !strncasecmp(p, name, name_len)) {
      const char *v = p + name_len + 1;
      size_t vn = line - name_len - 1;
      while (vn && (*v == ' ' || *v == '\t')) {
        v++;
        vn--;
      }
      if (vn >= cap)
        vn = cap - 1;
      memcpy(out, v, vn);
      out[vn] = 0;
      return 1;
    }
    if (!nl)
      break;
    p = nl + 1;
  }
  return 0;
}
static void log_request(FILE *log, const char *req, size_t n) {
  char path[256] = "/";
  if (n >= 4 && !memcmp(req, "GET ", 4)) {
    const char *sp = memchr(req + 4, ' ', n - 4);
    size_t pn = sp ? (size_t)(sp - (req + 4)) : 0;
    if (pn && pn < sizeof path) {
      memcpy(path, req + 4, pn);
      path[pn] = 0;
    }
  }
  char ua[128] = "", conn[64] = "", enc[64] = "", accept[64] = "";
  int has_accept = header_value(req, n, "Accept", accept, sizeof accept);
  header_value(req, n, "User-Agent", ua, sizeof ua);
  header_value(req, n, "Connection", conn, sizeof conn);
  header_value(req, n, "Accept-Encoding", enc, sizeof enc);
  fprintf(log,
          "{\"url\":\"%s\",\"user-agent\":\"%s\",\"connection\":\"%s\","
          "\"accept-encoding\":\"%s\",\"accept\":",
          path, ua, conn, enc);
  if (!has_accept)
    fputs("null}\n", log);
  else
    fprintf(log, "\"%s\"}\n", accept);
  fflush(log);
}
static int read_headers(int fd, SSL *ssl_io, char *buf, size_t cap, size_t *n) {
  *n = 0;
  while (*n + 1 < cap) {
    ssize_t got;
    if (ssl_io)
      got = SSL_read(ssl_io, buf + *n, (int)(cap - 1 - *n));
    else
      got = read(fd, buf + *n, cap - 1 - *n);
    if (got == 0)
      return -1;
    if (got < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    *n += (size_t)got;
    buf[*n] = 0;
    if (strstr(buf, "\r\n\r\n"))
      return 0;
  }
  return -1;
}
static int send_bytes(int fd, SSL *ssl_io, const void *data, size_t n) {
  return ssl_io ? ssl_write_all(ssl_io, data, n) : write_all_fd(fd, data, n);
}
static void wait_close(int fd) {
  char sink[256];
  while (!stop_flag) {
    ssize_t n = read(fd, sink, sizeof sink);
    if (n == 0 || (n < 0 && errno != EINTR))
      return;
  }
}
static void respond(int fd, SSL *ssl_io, const char *mode, int status,
                    const char *path, int *seen) {
  char head[256];
  int attempt = ++*seen;
  if (!strcmp(mode, "reset")) {
    struct linger lin = {.l_onoff = 1, .l_linger = 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof lin);
    return;
  }
  if (!strcmp(mode, "retry") && attempt < 3) {
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n"
                         "Connection: close\r\n\r\n");
    if (wrote > 0)
      send_bytes(fd, ssl_io, head, (size_t)wrote);
    return;
  }
  if (!strcmp(mode, "503")) {
    const char *body = "unavailable";
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 503 Service Unavailable\r\nContent-Length: "
                         "%zu\r\nConnection: close\r\n\r\n",
                         strlen(body));
    if (wrote > 0 && !send_bytes(fd, ssl_io, head, (size_t)wrote))
      send_bytes(fd, ssl_io, body, strlen(body));
    return;
  }
  if (!strcmp(mode, "stall503") || !strcmp(mode, "status")) {
    int code = !strcmp(mode, "status") ? status : 503;
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 %d status\r\nContent-Length: 10000\r\n"
                         "Connection: close\r\n\r\n",
                         code);
    if (wrote > 0)
      send_bytes(fd, ssl_io, head, (size_t)wrote);
    wait_close(fd);
    return;
  }
  if (!strcmp(mode, "stall200")) {
    const char *raw = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
    send_bytes(fd, ssl_io, raw, strlen(raw));
    wait_close(fd);
    return;
  }
  if (!strcmp(mode, "chunk")) {
    char hex[32];
    int hexn = snprintf(hex, sizeof hex, "%zx", strlen(payload));
    char *raw = NULL;
    size_t need = strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                         "Connection: close\r\n\r\n") +
                  (size_t)hexn + 2 + strlen(payload) + 2 + 1;
    raw = malloc(need);
    if (!raw)
      return;
    int wrote = snprintf(raw, need,
                         "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                         "Connection: close\r\n\r\n%s\r\n%s\r\n",
                         hex, payload);
    if (wrote > 0)
      send_bytes(fd, ssl_io, raw, (size_t)wrote);
    free(raw);
    return;
  }
  if (!strcmp(mode, "redirect") && path && !strcmp(path, "/model")) {
    const char *body = "irrelevant oversized redirect body";
    int wrote =
        snprintf(head, sizeof head,
                 "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: "
                 "%zu\r\nConnection: close\r\n\r\n",
                 strlen(body));
    if (wrote > 0 && !send_bytes(fd, ssl_io, head, (size_t)wrote))
      send_bytes(fd, ssl_io, body, strlen(body));
    return;
  }
  if (!strcmp(mode, "short") || !strcmp(mode, "shortcl")) {
    const char *body = payload;
    size_t blen = 2;
    size_t declared = !strcmp(mode, "shortcl") ? strlen(payload) : blen;
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         declared);
    if (wrote > 0 && !send_bytes(fd, ssl_io, head, (size_t)wrote))
      send_bytes(fd, ssl_io, body, blen);
    return;
  }
  if (!strcmp(mode, "over")) {
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         strlen(payload) + 100);
    if (wrote > 0 && !send_bytes(fd, ssl_io, head, (size_t)wrote))
      send_bytes(fd, ssl_io, payload, strlen(payload));
    return;
  }
  if (!strcmp(mode, "drip")) {
    int wrote = snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         strlen(payload));
    if (wrote > 0)
      send_bytes(fd, ssl_io, head, (size_t)wrote);
    for (size_t i = 0; i < strlen(payload) && !stop_flag; i++) {
      struct timespec pause = {.tv_nsec = 60 * 1000 * 1000};
      nanosleep(&pause, NULL);
      if (send_bytes(fd, ssl_io, payload + i, 1))
        break;
    }
    return;
  }
  int wrote = snprintf(head, sizeof head,
                       "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                       "Connection: close\r\n\r\n",
                       strlen(payload));
  if (wrote > 0 && !send_bytes(fd, ssl_io, head, (size_t)wrote))
    send_bytes(fd, ssl_io, payload, strlen(payload));
}
static int serve(int fd, SSL_CTX *ctx, const char *mode, int status, FILE *log) {
  int seen = 0;
  while (!stop_flag) {
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    int cfd = accept(fd, (struct sockaddr *)&addr, &len);
    if (cfd < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    SSL *io = NULL;
    if (ctx) {
      io = SSL_new(ctx);
      if (!io || SSL_set_fd(io, cfd) != 1 || SSL_accept(io) != 1) {
        if (io)
          SSL_free(io);
        close(cfd);
        continue;
      }
    }
    char req[8192];
    size_t n = 0;
    if (!read_headers(cfd, io, req, sizeof req, &n)) {
      log_request(log, req, n);
      char path[256] = "/";
      if (n >= 4 && !memcmp(req, "GET ", 4)) {
        const char *sp = memchr(req + 4, ' ', n - 4);
        size_t pn = sp ? (size_t)(sp - (req + 4)) : 0;
        if (pn && pn < sizeof path) {
          memcpy(path, req + 4, pn);
          path[pn] = 0;
        }
      }
      respond(cfd, io, mode, status, path, &seen);
    }
    if (io) {
      SSL_shutdown(io);
      SSL_free(io);
    }
    close(cfd);
  }
  return 0;
}
int main(int argc, char **argv) {
  if (argc < 4)
    return 2;
  const char *port_file = argv[1], *log_path = argv[2], *mode = argv[3];
  int status = argc > 4 ? atoi(argv[4]) : 0;
  const char *cert = argc > 5 ? argv[5] : NULL;
  const char *key = argc > 6 ? argv[6] : NULL;
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa = {.sa_handler = on_stop};
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  int port = 0;
  int fd = listen_local(&port);
  if (fd < 0)
    return 1;
  SSL_CTX *ctx = NULL;
  if (cert) {
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx || SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
      if (ctx)
        SSL_CTX_free(ctx);
      close(fd);
      return 1;
    }
  }
  FILE *log = fopen(log_path, "w");
  if (!log) {
    close(fd);
    return 1;
  }
  char tmp[64];
  int wrote = snprintf(tmp, sizeof tmp, "%s.tmp", port_file);
  FILE *pf = wrote > 0 ? fopen(tmp, "w") : NULL;
  if (!pf || fprintf(pf, "%d\n", port) < 0 || fclose(pf) ||
      rename(tmp, port_file)) {
    close(fd);
    fclose(log);
    return 1;
  }
  serve(fd, ctx, mode, status, log);
  fclose(log);
  close(fd);
  if (ctx)
    SSL_CTX_free(ctx);
  return 0;
}
