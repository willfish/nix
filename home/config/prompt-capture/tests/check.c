#include "run.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef SCRIPT
#define SCRIPT "home/user/prompt-capture.sh"
#endif
enum { M_UTF8, M_ECHO, M_PRIVATE, M_ANTHROPIC, M_REVERSE, M_SSE, M_WS, M_NONE = -1 };
static int server_mode, release_r = -1;
static char *note_dir, *bash_bin, *flock_bin, *ready_bin, *adapter;
static void expect(int cond, const char *what) {
  if (!cond)
    check_fail("%s", what);
}
static char *which(const char *name) {
  if (strchr(name, '/'))
    return access(name, X_OK) == 0 ? strdup(name) : NULL;
  const char *path = getenv("PATH");
  if (!path)
    return NULL;
  char *copy = strdup(path);
  for (char *save = NULL, *dir = strtok_r(copy, ":", &save); dir;
       dir = strtok_r(NULL, ":", &save)) {
    char *full = check_join(dir, name);
    if (full && access(full, X_OK) == 0) {
      free(copy);
      return full;
    }
    free(full);
  }
  free(copy);
  return NULL;
}
static int write_all(int fd, const void *data, size_t len) {
  const unsigned char *p = data;
  size_t at = 0;
  while (at < len) {
    ssize_t n = write(fd, p + at, len - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    at += (size_t)n;
  }
  return 0;
}
static int read_full(int fd, void *data, size_t len) {
  unsigned char *p = data;
  size_t at = 0;
  while (at < len) {
    ssize_t n = read(fd, p + at, len - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    at += (size_t)n;
  }
  return 0;
}
static int timed_read(int fd, void *data, size_t cap, size_t *got, int ms) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int rc = poll(&pfd, 1, ms);
  if (rc <= 0)
    return -1;
  ssize_t n = read(fd, data, cap);
  if (n < 0 && errno == EINTR)
    return timed_read(fd, data, cap, got, ms);
  if (n <= 0)
    return n < 0 ? -1 : 0;
  *got = (size_t)n;
  return 1;
}
static void sha1(const unsigned char *data, size_t len, unsigned char out[20]) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  uint64_t bits = (uint64_t)len * 8;
  size_t padded = ((len + 9 + 63) / 64) * 64;
  unsigned char *buf = calloc(1, padded ? padded : 1);
  memcpy(buf, data, len);
  buf[len] = 0x80;
  for (int i = 0; i < 8; i++)
    buf[padded - 1 - i] = (unsigned char)((bits >> (8 * i)) & 255);
  for (size_t chunk = 0; chunk < padded; chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)buf[chunk + (size_t)i * 4] << 24) |
             ((uint32_t)buf[chunk + (size_t)i * 4 + 1] << 16) |
             ((uint32_t)buf[chunk + (size_t)i * 4 + 2] << 8) | buf[chunk + (size_t)i * 4 + 3];
    for (int i = 16; i < 80; i++) {
      uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
      w[i] = (x << 1) | (x >> 31);
    }
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
      e = d;
      d = c;
      c = (b << 30) | (b >> 2);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }
  free(buf);
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; i++) {
    out[i * 4] = (unsigned char)(hs[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(hs[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(hs[i] >> 8);
    out[i * 4 + 3] = (unsigned char)hs[i];
  }
}
static char *b64(const unsigned char *data, size_t len) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char *out = malloc(((len + 2) / 3) * 4 + 1);
  size_t at = 0;
  for (size_t i = 0; i < len; i += 3) {
    unsigned n = (unsigned)data[i] << 16;
    int have = 1;
    if (i + 1 < len) {
      n |= (unsigned)data[i + 1] << 8;
      have = 2;
    }
    if (i + 2 < len) {
      n |= data[i + 2];
      have = 3;
    }
    out[at++] = tbl[(n >> 18) & 63];
    out[at++] = tbl[(n >> 12) & 63];
    out[at++] = have > 1 ? tbl[(n >> 6) & 63] : '=';
    out[at++] = have > 2 ? tbl[n & 63] : '=';
  }
  out[at] = 0;
  return out;
}
static int listen_port(int *port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 16)) {
    if (fd >= 0)
      close(fd);
    return -1;
  }
  socklen_t len = sizeof addr;
  getsockname(fd, (struct sockaddr *)&addr, &len);
  *port = ntohs(addr.sin_port);
  return fd;
}
static const char *header_line(const char *headers, const char *name) {
  size_t n = strlen(name);
  for (const char *p = headers; *p;) {
    if (!strncasecmp(p, name, n) && p[n] == ':') {
      p += n + 1;
      while (*p == ' ')
        p++;
      return p;
    }
    const char *nl = strchr(p, '\n');
    if (!nl)
      break;
    p = nl + 1;
  }
  return NULL;
}
static void copy_token(const char *value, char *out, size_t cap) {
  size_t n = 0;
  if (!value) {
    out[0] = 0;
    return;
  }
  while (*value && *value != '\r' && *value != '\n' && n + 1 < cap)
    out[n++] = *value++;
  out[n] = 0;
}
static int read_http_head(int fd, char **head, unsigned char **rest, size_t *rest_len) {
  unsigned char *buf = malloc(65536);
  size_t n = 0;
  while (n + 1 < 65536) {
    size_t got = 0;
    if (timed_read(fd, buf + n, 65536 - 1 - n, &got, 15000) <= 0)
      break;
    n += got;
    buf[n] = 0;
    unsigned char *split = memmem(buf, n, "\r\n\r\n", 4);
    if (!split)
      continue;
    size_t hlen = (size_t)(split - buf) + 4;
    *head = malloc(hlen + 1);
    memcpy(*head, buf, hlen);
    (*head)[hlen] = 0;
    *rest_len = n - hlen;
    *rest = malloc(*rest_len + 1);
    memcpy(*rest, buf + hlen, *rest_len);
    free(buf);
    return 0;
  }
  free(buf);
  return -1;
}
static void handle_client(int fd) {
  int nodelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
  char *head = NULL;
  unsigned char *rest = NULL;
  size_t rest_len = 0;
  if (read_http_head(fd, &head, &rest, &rest_len)) {
    close(fd);
    return;
  }
  char path[512] = "/";
  char *start = strchr(head, ' ');
  if (start) {
    start++;
    char *end = strchr(start, ' ');
    if (end && (size_t)(end - start) < sizeof path) {
      memcpy(path, start, (size_t)(end - start));
      path[end - start] = 0;
    }
  }
  const char *length = header_line(head, "content-length");
  long body_len = length ? strtol(length, NULL, 10) : 0;
  unsigned char *body = NULL;
  if (body_len > 0 && body_len < 8 * 1024 * 1024) {
    body = malloc((size_t)body_len);
    size_t have = rest_len < (size_t)body_len ? rest_len : (size_t)body_len;
    if (have)
      memcpy(body, rest, have);
    if (have < (size_t)body_len && read_full(fd, body + have, (size_t)body_len - have))
      body_len = (long)have;
  }
  if (server_mode == M_REVERSE && note_dir) {
    char *path_file = check_join(note_dir, "upstream-path");
    char *body_file = check_join(note_dir, "upstream-body");
    check_write(path_file, path, strlen(path));
    check_write(body_file, body ? body : (const unsigned char *)"", body ? (size_t)body_len : 0);
    free(path_file);
    free(body_file);
  }
  if (server_mode == M_WS) {
    char key[128];
    copy_token(header_line(head, "sec-websocket-key"), key, sizeof key);
    char magic[192];
    snprintf(magic, sizeof magic, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char dig[20];
    sha1((const unsigned char *)magic, strlen(magic), dig);
    char *accept = b64(dig, 20);
    char resp[320];
    int wrote = snprintf(resp, sizeof resp,
                         "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: %s\r\n\r\n",
                         accept);
    write_all(fd, resp, (size_t)wrote);
    free(accept);
    unsigned char tmp[128];
    size_t got = 0;
    timed_read(fd, tmp, sizeof tmp, &got, 10000);
    unsigned char frames[] = {0x81, 5, 'w', 'o', 'r', 'l', 'd', 0x82, 2, 0x00, 0xff};
    write_all(fd, frames, sizeof frames);
  } else if (server_mode == M_ECHO) {
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
    write_all(fd, resp, strlen(resp));
  } else if (server_mode == M_PRIVATE) {
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                       "data: {\"private\":\"response\"}\n\n";
    write_all(fd, resp, strlen(resp));
  } else if (server_mode == M_ANTHROPIC) {
    const char *events =
        "event: message_start\r\ndata: {\"message\":{\"usage\":{\"input_tokens\":123,\"output_tokens\":0}}}\r\n\r\n"
        "data: {\"usage\":{\"output_tokens\":7}}\n\n";
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
    write_all(fd, resp, strlen(resp));
    write_all(fd, events, 31);
    usleep(50000);
    write_all(fd, events + 31, 69);
    usleep(50000);
    write_all(fd, events + 100, strlen(events) - 100);
  } else if (server_mode == M_REVERSE) {
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                       "{\"usage\":{\"input_tokens\":321,\"output_tokens\":5}}";
    write_all(fd, resp, strlen(resp));
  } else if (server_mode == M_SSE) {
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: 27\r\n\r\n";
    write_all(fd, resp, strlen(resp));
    write_all(fd, "data: first\n\n", 13);
    unsigned char go = 0;
    if (release_r >= 0)
      read_full(release_r, &go, 1);
    write_all(fd, "data: second\n\n", 14);
  } else {
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n\r\n";
    write_all(fd, resp, strlen(resp));
    unsigned char first[] = {100, 97, 116, 97, 58, 32, 0xe2};
    write_all(fd, first, sizeof first);
    unsigned char go = 0;
    if (release_r >= 0)
      read_full(release_r, &go, 1);
    unsigned char tail[] = {0x82, 0xac, 10, 10};
    write_all(fd, tail, sizeof tail);
  }
  free(head);
  free(rest);
  free(body);
  close(fd);
}
typedef struct {
  char *root, *script, *cdir, *ready, *err_path, *upstream;
  int port, upstream_port, release_w, body;
  pid_t child, group, server;
} Fix;
static char **merge_env(const char *const *extra) {
  extern char **environ;
  size_t base = 0, more = 0;
  while (environ[base])
    base++;
  while (extra[more])
    more++;
  char **out = calloc(base + more + 1, sizeof *out);
  for (size_t i = 0; i < base; i++)
    out[i] = strdup(environ[i]);
  for (size_t i = 0; i < more; i++) {
    const char *eq = strchr(extra[i], '=');
    size_t key = eq ? (size_t)(eq - extra[i]) : strlen(extra[i]);
    size_t at = 0;
    for (; at < base; at++)
      if (!strncmp(out[at], extra[i], key) && out[at][key] == '=')
        break;
    char *copy = strdup(extra[i]);
    if (at < base) {
      free(out[at]);
      out[at] = copy;
    } else
      out[base++] = copy;
  }
  return out;
}
static void free_env(char **env) {
  for (size_t i = 0; env && env[i]; i++)
    free(env[i]);
  free(env);
}
static int rewrite_script(Fix *fix) {
  size_t n = 0;
  unsigned char *raw = check_read(SCRIPT, &n);
  if (!raw)
    return -1;
  const char *needle = "port=8302";
  unsigned char *found = memmem(raw, n, needle, strlen(needle));
  if (!found) {
    free(raw);
    return -1;
  }
  char port[32];
  int wrote = snprintf(port, sizeof port, "port=%d", fix->port);
  size_t out_len = n - strlen(needle) + (size_t)wrote;
  char *out = malloc(out_len);
  size_t head = (size_t)(found - raw);
  memcpy(out, raw, head);
  memcpy(out + head, port, (size_t)wrote);
  memcpy(out + head + (size_t)wrote, found + strlen(needle), n - head - strlen(needle));
  free(raw);
  int rc = check_write(fix->script, out, out_len);
  free(out);
  chmod(fix->script, 0700);
  return rc;
}
static char **fix_env(Fix *fix) {
  char mitm[512], flock[512], state[512], body[64], upstream[512];
  snprintf(mitm, sizeof mitm, "MITMDUMP=%s", adapter);
  snprintf(flock, sizeof flock, "FLOCK=%s", flock_bin);
  snprintf(state, sizeof state, "XDG_STATE_HOME=%s", fix->root);
  snprintf(body, sizeof body, "PROMPT_CAPTURE_RESPONSE_BODY=%d", fix->body);
  snprintf(upstream, sizeof upstream, "PROMPT_CAPTURE_UPSTREAM=%s", fix->upstream ? fix->upstream : "");
  const char *extra[] = {mitm, flock, state, body, upstream, NULL};
  return merge_env(extra);
}
static pid_t spawn_at(Fix *fix, const char *const *args) {
  char **env = fix_env(fix);
  pid_t pid = fork();
  if (!pid) {
    setpgid(0, 0);
    int err = open(fix->err_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (err >= 0)
      dup2(err, STDERR_FILENO);
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0)
      dup2(null, STDIN_FILENO);
    execve(bash_bin, (char *const *)args, env);
    _exit(127);
  }
  setpgid(pid, pid);
  free_env(env);
  return pid;
}
static int wait_ready(Fix *fix) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (1) {
    if (access(fix->ready, F_OK) == 0)
      return 0;
    int status = 0;
    if (fix->child > 0 && waitpid(fix->child, &status, WNOHANG) == fix->child) {
      fix->child = 0;
      return -1;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9 > 45)
      return -1;
    struct timespec pause = {.tv_nsec = 50 * 1000 * 1000};
    nanosleep(&pause, NULL);
  }
}
static void stop_group(pid_t pid) {
  if (pid <= 0)
    return;
  kill(-pid, SIGTERM);
  for (int i = 0; i < 50; i++) {
    if (waitpid(pid, NULL, WNOHANG) == pid)
      break;
    struct timespec pause = {.tv_nsec = 50 * 1000 * 1000};
    nanosleep(&pause, NULL);
  }
  kill(-pid, SIGKILL);
  waitpid(pid, NULL, 0);
}
static void fix_end(Fix *fix) {
  if (fix->release_w >= 0) {
    unsigned char go = 1;
    write_all(fix->release_w, &go, 1);
    close(fix->release_w);
  }
  stop_group(fix->child ? fix->child : fix->group);
  stop_group(fix->server);
  char *log = check_join(fix->cdir, "pi-server.log");
  size_t n = 0;
  unsigned char *text = check_read(log, &n);
  if (text && (contains_text(text, n, "addon error") ||
               contains_text(text, n, "native capture adapter failed")))
    check_fail("adapter diagnostic in server log");
  free(text);
  free(log);
  check_rm_rf(fix->root);
  free(fix->root);
  free(fix->script);
  free(fix->cdir);
  free(fix->ready);
  free(fix->err_path);
  free(fix->upstream);
}
static int fix_start(Fix *fix) {
  unlink(fix->ready);
  const char *args[] = {bash_bin, fix->script, "pi", "--", ready_bin, fix->ready, NULL};
  fix->child = spawn_at(fix, args);
  fix->group = fix->child;
  if (wait_ready(fix)) {
    size_t n = 0, ln = 0;
    unsigned char *err = check_read(fix->err_path, &n);
    char *log = check_join(fix->cdir, "pi-server.log");
    unsigned char *server = check_read(log, &ln);
    check_fail("capture did not start: %s %s", err ? (char *)err : "", server ? (char *)server : "");
    free(err);
    free(server);
    free(log);
    return -1;
  }
  return 0;
}
static Fix fix_new(int body, int mode, int reverse) {
  Fix fix = {.body = body, .release_w = -1};
  int listen_fd = listen_port(&fix.port);
  close(listen_fd);
  fix.root = check_temp("capture-native-");
  fix.script = check_join(fix.root, "capture.sh");
  fix.cdir = check_join(fix.root, "prompt-capture");
  fix.ready = check_join(fix.root, "ready");
  fix.err_path = check_join(fix.root, "stderr");
  if (rewrite_script(&fix))
    check_fail("rewrite script");
  if (mode >= 0) {
    int pipes[2];
    pipe(pipes);
    server_mode = mode;
    note_dir = fix.root;
    release_r = pipes[0];
    int fd = listen_port(&fix.upstream_port);
    fix.server = fork();
    if (!fix.server) {
      setpgid(0, 0);
      signal(SIGPIPE, SIG_IGN);
      signal(SIGCHLD, SIG_IGN);
      close(pipes[1]);
      for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client < 0)
          continue;
        if (!fork()) {
          handle_client(client);
          _exit(0);
        }
        close(client);
      }
    }
    setpgid(fix.server, fix.server);
    close(fd);
    close(pipes[0]);
    fix.release_w = pipes[1];
    release_r = -1;
  }
  if (reverse) {
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", fix.upstream_port);
    fix.upstream = strdup(url);
  }
  return fix;
}
static int connect_port(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  struct timeval tv = {.tv_sec = 20};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
    close(fd);
    return -1;
  }
  return fd;
}
static int append_buf(unsigned char **data, size_t *len, size_t *cap, const void *src, size_t n) {
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1)
      next *= 2;
    unsigned char *grown = realloc(*data, next);
    if (!grown)
      return -1;
    *data = grown;
    *cap = next;
  }
  memcpy(*data + *len, src, n);
  *len += n;
  (*data)[*len] = 0;
  return 0;
}
static int take_line(int fd, unsigned char **pre, size_t *pre_len, char *line, size_t cap) {
  size_t at = 0;
  while (at + 1 < cap) {
    unsigned char ch;
    if (*pre_len) {
      ch = (*pre)[0];
      memmove(*pre, *pre + 1, --(*pre_len));
    } else if (read_full(fd, &ch, 1))
      return -1;
    line[at++] = (char)ch;
    if (at >= 2 && line[at - 2] == '\r' && line[at - 1] == '\n') {
      line[at - 2] = 0;
      return 0;
    }
  }
  return -1;
}
static int take_bytes(int fd, unsigned char **pre, size_t *pre_len, unsigned char *out, size_t n) {
  size_t have = *pre_len < n ? *pre_len : n;
  if (have)
    memcpy(out, *pre, have);
  if (have < *pre_len)
    memmove(*pre, *pre + have, *pre_len - have);
  *pre_len -= have;
  if (have < n && read_full(fd, out + have, n - have))
    return -1;
  return 0;
}
static int read_response(Fix *fix, int fd, int want_first, unsigned char **first, size_t *first_len,
                         unsigned char **all, size_t *all_len, int *status) {
  char *head = NULL;
  unsigned char *rest = NULL;
  size_t rest_len = 0;
  if (read_http_head(fd, &head, &rest, &rest_len))
    return -1;
  *status = 0;
  char *sp = strchr(head, ' ');
  if (sp)
    *status = atoi(sp + 1);
  int chunked = header_line(head, "transfer-encoding") &&
                strcasestr(header_line(head, "transfer-encoding"), "chunked");
  const char *cl = header_line(head, "content-length");
  long length = cl && !chunked ? strtol(cl, NULL, 10) : -1;
  free(head);
  unsigned char *body = NULL;
  size_t len = 0, cap = 0;
  int released = 0;
  if (chunked) {
    while (1) {
      char line[64];
      if (take_line(fd, &rest, &rest_len, line, sizeof line))
        break;
      char *end = NULL;
      unsigned long chunk = strtoul(line, &end, 16);
      if (end == line)
        break;
      if (!chunk)
        break;
      unsigned char *piece = malloc(chunk);
      if (take_bytes(fd, &rest, &rest_len, piece, chunk)) {
        free(piece);
        break;
      }
      unsigned char crlf[2];
      take_bytes(fd, &rest, &rest_len, crlf, 2);
      if (want_first && !released) {
        *first = malloc(chunk);
        memcpy(*first, piece, chunk);
        *first_len = chunk;
        if (fix->release_w >= 0) {
          unsigned char go = 1;
          write_all(fix->release_w, &go, 1);
          close(fix->release_w);
          fix->release_w = -1;
        }
        released = 1;
      }
      append_buf(&body, &len, &cap, piece, chunk);
      free(piece);
    }
  } else {
    if (rest_len)
      append_buf(&body, &len, &cap, rest, rest_len);
    if (want_first) {
      if (!len) {
        unsigned char buf[4096];
        size_t got = 0;
        if (timed_read(fd, buf, sizeof buf, &got, 10000) > 0)
          append_buf(&body, &len, &cap, buf, got);
      }
      *first = malloc(len ? len : 1);
      if (len)
        memcpy(*first, body, len);
      *first_len = len;
      if (fix->release_w >= 0) {
        unsigned char go = 1;
        write_all(fix->release_w, &go, 1);
        close(fix->release_w);
        fix->release_w = -1;
      }
    }
    while (length < 0 || len < (size_t)length) {
      unsigned char buf[4096];
      size_t got = 0;
      if (timed_read(fd, buf, sizeof buf, &got, 10000) <= 0)
        break;
      append_buf(&body, &len, &cap, buf, got);
    }
  }
  free(rest);
  *all = body ? body : calloc(1, 1);
  *all_len = body ? len : 0;
  return 0;
}
static int proxy_exchange(Fix *fix, const char *method, const char *path, const void *payload,
                          size_t payload_len, const char *extra, int want_first, int *status,
                          unsigned char **first, size_t *first_len, unsigned char **all, size_t *all_len) {
  int fd = connect_port(fix->port);
  if (fd < 0)
    return -1;
  char top[1200];
  int wrote;
  if (fix->upstream)
    wrote = snprintf(top, sizeof top,
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",
                     method, path, fix->port, payload_len, extra ? extra : "");
  else
    wrote = snprintf(top, sizeof top,
                     "%s http://127.0.0.1:%d%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n%s\r\n",
                     method, fix->upstream_port, path, fix->upstream_port, payload_len, extra ? extra : "");
  if (wrote < 0 || (size_t)wrote >= sizeof top || write_all(fd, top, (size_t)wrote) ||
      (payload_len && write_all(fd, payload, payload_len)) ||
      read_response(fix, fd, want_first, first, first_len, all, all_len, status)) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}
static int read_lines(const char *path, char ***lines, size_t *count) {
  for (int i = 0; i < 50; i++) {
    size_t n = 0;
    unsigned char *raw = check_read(path, &n);
    if (raw && n) {
      size_t rows = 0;
      for (size_t j = 0; j < n; j++)
        if (raw[j] == '\n')
          rows++;
      char **all = calloc(rows + 1, sizeof *all);
      size_t used = 0, at = 0;
      for (size_t j = 0; j <= n; j++) {
        if (j < n && raw[j] != '\n')
          continue;
        if (j > at) {
          all[used] = malloc(j - at + 1);
          memcpy(all[used], raw + at, j - at);
          all[used][j - at] = 0;
          used++;
        }
        at = j + 1;
      }
      free(raw);
      if (used) {
        *lines = all;
        *count = used;
        return 0;
      }
      free(all);
    }
    free(raw);
    struct timespec pause = {.tv_nsec = 100 * 1000 * 1000};
    nanosleep(&pause, NULL);
  }
  *lines = NULL;
  *count = 0;
  return -1;
}
static void free_lines(char **lines, size_t count) {
  for (size_t i = 0; i < count; i++)
    free(lines[i]);
  free(lines);
}
static const char *after_key(const char *json, const char *key) {
  char needle[96];
  snprintf(needle, sizeof needle, "\"%s\"", key);
  size_t n = strlen(needle);
  const char *p = json;
  while (p && *p && (p = strstr(p, needle))) {
    const char *q = p + n;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
      q++;
    if (*q == ':') {
      q++;
      while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
        q++;
      return q;
    }
    p += n;
  }
  return NULL;
}
static int kind_has(const char *json, const char *kind) {
  const char *v = after_key(json, "kind");
  size_t n = strlen(kind);
  return v && *v == '"' && !strncmp(v + 1, kind, n) && v[1 + n] == '"';
}
static int kind_prefix(const char *json, const char *prefix) {
  const char *v = after_key(json, "kind");
  size_t n = strlen(prefix);
  return v && *v == '"' && !strncmp(v + 1, prefix, n);
}
static const char *kind_line(char **lines, size_t count, const char *kind, size_t index) {
  size_t seen = 0;
  for (size_t i = 0; i < count; i++)
    if (kind_has(lines[i], kind) && seen++ == index)
      return lines[i];
  return NULL;
}
static int has_key(const char *json, const char *key) {
  char needle[96];
  snprintf(needle, sizeof needle, "\"%s\"", key);
  return json && strstr(json, needle) != NULL;
}
static int hex_byte(const char *text, unsigned *out) {
  unsigned value = 0;
  for (int i = 0; i < 4; i++) {
    char c = text[i];
    int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                        : c >= 'A' && c <= 'F'   ? c - 'A' + 10
                                                                 : -1;
    if (n < 0)
      return -1;
    value = (value << 4) | (unsigned)n;
  }
  *out = value;
  return 0;
}
static int string_field(const char *json, const char *key, unsigned char **out, size_t *len) {
  const char *start = json ? after_key(json, key) : NULL;
  if (!start || *start != '"')
    return -1;
  start++;
  unsigned char *buf = malloc(strlen(start) + 1);
  size_t n = 0;
  for (const char *p = start; *p && *p != '"'; p++) {
    if (*p != '\\') {
      buf[n++] = (unsigned char)*p;
      continue;
    }
    p++;
    if (*p == 'u') {
      unsigned code = 0;
      if (hex_byte(p + 1, &code))
        break;
      if (code < 0x80)
        buf[n++] = (unsigned char)code;
      else if (code < 0x800) {
        buf[n++] = (unsigned char)(0xc0 | (code >> 6));
        buf[n++] = (unsigned char)(0x80 | (code & 0x3f));
      } else {
        buf[n++] = (unsigned char)(0xe0 | (code >> 12));
        buf[n++] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
        buf[n++] = (unsigned char)(0x80 | (code & 0x3f));
      }
      p += 4;
      continue;
    }
    buf[n++] = (unsigned char)(*p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p);
  }
  buf[n] = 0;
  *out = buf;
  *len = n;
  return 0;
}
static int number_field(const char *json, const char *key, double *out) {
  const char *start = json ? after_key(json, key) : NULL;
  if (!start)
    return -1;
  char *end = NULL;
  *out = strtod(start, &end);
  return end == start ? -1 : 0;
}
static int near_field(const char *json, const char *key, double expected) {
  double got = 0;
  if (number_field(json, key, &got))
    return 0;
  double delta = got - expected;
  if (delta < 0)
    delta = -delta;
  return delta < 1e-6;
}
static void utf8_stream(void) {
  check_begin("streaming preserves split UTF-8 and forwards original bytes immediately");
  Fix fix = fix_new(1, M_UTF8, 0);
  if (!fix_start(&fix)) {
    int status = 0;
    unsigned char *first = NULL, *all = NULL;
    size_t first_len = 0, all_len = 0;
    const unsigned char expected[] = {100, 97, 116, 97, 58, 32, 0xe2};
    expect(proxy_exchange(&fix, "GET", "/events", NULL, 0, NULL, 1, &status, &first, &first_len, &all,
                          &all_len) == 0,
           "request");
    expect(first_len == sizeof expected && first && !memcmp(first, expected, sizeof expected),
           "first chunk");
    const unsigned char full[] = {100, 97, 116, 97, 58, 32, 0xe2, 0x82, 0xac, 10, 10};
    expect(all_len == sizeof full && all && !memcmp(all, full, sizeof full), "full stream");
    char *log = check_join(fix.cdir, "pi.jsonl");
    char **lines = NULL;
    size_t count = 0;
    read_lines(log, &lines, &count);
    unsigned char *joined = NULL;
    size_t jlen = 0, jcap = 0;
    for (size_t i = 0; i < count; i++) {
      if (!kind_has(lines[i], "response_chunk"))
        continue;
      unsigned char *text = NULL;
      size_t n = 0;
      if (!string_field(lines[i], "response_body", &text, &n))
        append_buf(&joined, &jlen, &jcap, text, n);
      free(text);
    }
    expect(joined && jlen == sizeof full && !memcmp(joined, full, sizeof full), "recorded chunks");
    free(joined);
    free_lines(lines, count);
    free(log);
    free(first);
    free(all);
  }
  fix_end(&fix);
}
static Proc run_fix(Fix *fix, const char *const *args, int timeout) {
  char **env = fix_env(fix);
  Proc proc = check_run(args[0], args, NULL, 0, (const char *const *)env, timeout);
  free_env(env);
  return proc;
}
static void large_request(void) {
  check_begin("large requests remain complete with metrics and redacted credentials");
  Fix fix = fix_new(1, M_ECHO, 0);
  if (!fix_start(&fix)) {
    const char *prefix = "{\"model\":\"qwen\",\"messages\":[{\"content\":\"";
    const char *suffix = "\"}],\"tools\":[{\"name\":\"read\"}]}";
    size_t chars = strlen(prefix) + 210000 + strlen(suffix);
    size_t bytes = strlen(prefix) + 210000 * 3 + strlen(suffix);
    unsigned char *body = malloc(bytes);
    memcpy(body, prefix, strlen(prefix));
    for (size_t i = 0; i < 210000; i++)
      memcpy(body + strlen(prefix) + i * 3, "\xe2\x82\xac", 3);
    memcpy(body + strlen(prefix) + 210000 * 3, suffix, strlen(suffix));
    int status = 0;
    unsigned char *all = NULL;
    size_t all_len = 0;
    const char *extra = "Authorization: fixture-secret\r\nx-api-key: fixture-secret\r\nCookie: fixture-secret\r\n"
                        "Content-Type: application/json\r\n";
    expect(proxy_exchange(&fix, "POST", "/events", body, bytes, extra, 0, &status, NULL, NULL, &all,
                          &all_len) == 0,
           "post");
    char *log = check_join(fix.cdir, "pi.jsonl");
    char **lines = NULL;
    size_t count = 0;
    read_lines(log, &lines, &count);
    const char *record = kind_line(lines, count, "request", 0);
    unsigned char *stored = NULL;
    size_t n = 0;
    string_field(record, "request_body", &stored, &n);
    expect(stored && n == bytes && !memcmp(stored, body, bytes), "body");
    expect(near_field(record, "request_chars", (double)chars), "chars");
    expect(near_field(record, "request_bytes", (double)bytes), "bytes");
    unsigned char *model = NULL;
    size_t mn = 0;
    string_field(record, "model", &model, &mn);
    expect(model && mn == 4 && !memcmp(model, "qwen", 4), "model");
    free(model);
    expect(near_field(record, "message_count", 1), "messages");
    expect(near_field(record, "tool_count", 1), "tools");
    const char *auth = after_key(record, "Authorization");
    expect(auth && !strncmp(auth, "\"<redacted>\"", 12), "auth");
    size_t raw_len = 0;
    unsigned char *raw = check_read(log, &raw_len);
    expect(raw && !contains_text(raw, raw_len, "fixture-secret"), "redacted");
    free(raw);
    free(stored);
    free(all);
    free(body);
    free_lines(lines, count);
    free(log);
  }
  fix_end(&fix);
}
static void no_body(void) {
  check_begin("streaming without body capture omits response content");
  Fix fix = fix_new(0, M_PRIVATE, 0);
  if (!fix_start(&fix)) {
    int status = 0;
    unsigned char *all = NULL;
    size_t all_len = 0;
    const char *expected = "data: {\"private\":\"response\"}\n\n";
    expect(proxy_exchange(&fix, "GET", "/events", NULL, 0, NULL, 0, &status, NULL, NULL, &all, &all_len) == 0,
           "get");
    expect(all && all_len == strlen(expected) && !memcmp(all, expected, all_len), "forwarded");
    char *log = check_join(fix.cdir, "pi.jsonl");
    char **lines = NULL;
    size_t count = 0;
    read_lines(log, &lines, &count);
    int clean = count > 0;
    for (size_t i = 0; i < count; i++)
      if (has_key(lines[i], "response_body"))
        clean = 0;
    expect(clean, "no body field");
    free_lines(lines, count);
    free(all);
    free(log);
  }
  fix_end(&fix);
}
static void anthropic(void) {
  check_begin("Anthropic usage survives chunking without logging response bodies");
  Fix fix = fix_new(0, M_ANTHROPIC, 0);
  if (!fix_start(&fix)) {
    const char *events =
        "event: message_start\r\ndata: {\"message\":{\"usage\":{\"input_tokens\":123,\"output_tokens\":0}}}\r\n\r\n"
        "data: {\"usage\":{\"output_tokens\":7}}\n\n";
    int status = 0;
    unsigned char *all = NULL;
    size_t all_len = 0;
    expect(proxy_exchange(&fix, "GET", "/events", NULL, 0, NULL, 0, &status, NULL, NULL, &all, &all_len) == 0,
           "get");
    expect(all && all_len == strlen(events) && !memcmp(all, events, all_len), "events");
    char *log = check_join(fix.cdir, "pi.jsonl");
    char **lines = NULL;
    size_t count = 0;
    read_lines(log, &lines, &count);
    const char *first = kind_line(lines, count, "usage", 0);
    const char *second = kind_line(lines, count, "usage", 1);
    expect(near_field(first, "input_tokens", 123), "input");
    expect(near_field(first, "output_tokens", 0), "output0");
    expect(near_field(second, "output_tokens", 7) && !has_key(second, "input_tokens"), "output7");
    int clean = 1;
    for (size_t i = 0; i < count; i++)
      if (has_key(lines[i], "response_body"))
        clean = 0;
    expect(clean, "no bodies");
    free_lines(lines, count);
    free(all);
    free(log);
  }
  fix_end(&fix);
}
static void reverse_proxy(void) {
  check_begin("reverse proxy forwards local requests and records nonstreaming usage");
  Fix fix = fix_new(1, M_REVERSE, 1);
  if (!fix_start(&fix)) {
    size_t ready_len = 0;
    unsigned char *ready = check_read(fix.ready, &ready_len);
    char expect_base[80];
    snprintf(expect_base, sizeof expect_base, "\"base\":\"http://127.0.0.1:%d\"", fix.port);
    expect(ready && contains_text(ready, ready_len, expect_base), "base url");
    free(ready);
    const char *body = "{\"model\":\"qwen\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    int status = 0;
    unsigned char *all = NULL;
    size_t all_len = 0;
    expect(proxy_exchange(&fix, "POST", "/v1/messages", body, strlen(body), "x-api-key: fixture-secret\r\n", 0,
                          &status, NULL, NULL, &all, &all_len) == 0,
           "post");
    expect(status == 200, "status");
    char *got_path = check_join(fix.root, "upstream-path");
    char *got_body = check_join(fix.root, "upstream-body");
    size_t n = 0;
    unsigned char *path = check_read(got_path, &n);
    expect(path && n == strlen("/v1/messages") && !memcmp(path, "/v1/messages", n), "upstream path");
    unsigned char *received = check_read(got_body, &n);
    expect(received && n == strlen(body) && !memcmp(received, body, n), "upstream body");
    char *log = check_join(fix.cdir, "pi.jsonl");
    char **lines = NULL;
    size_t count = 0;
    read_lines(log, &lines, &count);
    expect(near_field(kind_line(lines, count, "usage", 0), "input_tokens", 321), "usage");
    size_t raw_len = 0;
    unsigned char *raw = check_read(log, &raw_len);
    expect(raw && !contains_text(raw, raw_len, "fixture-secret"), "secret");
    free(raw);
    free_lines(lines, count);
    free(received);
    free(path);
    free(got_body);
    free(got_path);
    free(all);
    free(log);
    stop_group(fix.child);
    fix.child = 0;
  }
  fix_end(&fix);
}
static void early_sse(void) {
  check_begin("SSE reaches the client before the upstream response finishes");
  Fix fix = fix_new(1, M_SSE, 0);
  if (!fix_start(&fix)) {
    int status = 0;
    unsigned char *first = NULL, *all = NULL;
    size_t first_len = 0, all_len = 0;
    expect(proxy_exchange(&fix, "GET", "/events", NULL, 0, NULL, 1, &status, &first, &first_len, &all,
                          &all_len) == 0,
           "get");
    expect(first && first_len == 13 && !memcmp(first, "data: first\n\n", 13), "first");
    expect(all && all_len == 27 && !memcmp(all, "data: first\n\ndata: second\n\n", 27), "all");
    char *log = check_join(fix.cdir, "pi.jsonl");
    char **lines = NULL;
    size_t count = 0;
    read_lines(log, &lines, &count);
    unsigned char *joined = NULL;
    size_t jlen = 0, jcap = 0;
    for (size_t i = 0; i < count; i++) {
      if (!kind_has(lines[i], "response_chunk"))
        continue;
      unsigned char *text = NULL;
      size_t n = 0;
      if (!string_field(lines[i], "response_body", &text, &n))
        append_buf(&joined, &jlen, &jcap, text, n);
      free(text);
    }
    expect(joined && jlen == all_len && !memcmp(joined, all, all_len), "chunks");
    free(joined);
    free_lines(lines, count);
    free(log);
    free(first);
    free(all);
  }
  fix_end(&fix);
}
static void overlap(void) {
  check_begin("overlapping capture leaves the original proxy and lock intact");
  Fix fix = fix_new(1, M_NONE, 0);
  if (!fix_start(&fix)) {
    char *pidfile = check_join(fix.cdir, "pi.pid");
    size_t n = 0;
    unsigned char *pid_text = check_read(pidfile, &n);
    char *true_sh = check_join(fix.root, "true.sh");
    check_write(true_sh, "#!/bin/sh\nexit 0\n", 17);
    chmod(true_sh, 0755);
    const char *args[] = {bash_bin, fix.script, "pi", "--", true_sh, NULL};
    Proc second = run_fix(&fix, args, 20);
    expect(second.status != 0 && contains_text(second.err, second.err_len, "already active"), "active");
    size_t again = 0;
    unsigned char *still = check_read(pidfile, &again);
    expect(still && again == n && !memcmp(still, pid_text, n), "pidfile");
    int pid = atoi((char *)pid_text);
    expect(kill(pid, 0) == 0, "proxy alive");
    expect(fix.child > 0 && waitpid(fix.child, NULL, WNOHANG) == 0, "wrapper alive");
    proc_free(&second);
    free(still);
    free(pid_text);
    free(true_sh);
    free(pidfile);
    stop_group(fix.child);
    fix.child = 0;
    expect(fix_start(&fix) == 0, "restart");
  }
  fix_end(&fix);
}
static void permissions(void) {
  check_begin("private state permissions are repaired and trust bundles contain no keys");
  Fix fix = fix_new(1, M_NONE, 0);
  check_mkdir(fix.cdir);
  chmod(fix.cdir, 0755);
  const char *names[] = {"pi.jsonl", "pi-ca.pem", "pi-server.log"};
  for (size_t i = 0; i < 3; i++) {
    char *path = check_join(fix.cdir, names[i]);
    check_write(path, "", 0);
    chmod(path, 0644);
    free(path);
  }
  if (!fix_start(&fix)) {
    expect((check_mode(fix.cdir) & 0777) == 0700, "dir mode");
    const char *private[] = {"pi.jsonl", "pi-ca.pem", "pi-server.log", "pi.pid"};
    for (size_t i = 0; i < 4; i++) {
      char *path = check_join(fix.cdir, private[i]);
      expect((check_mode(path) & 0777) == 0600, private[i]);
      free(path);
    }
    char *bundle_path = check_join(fix.cdir, "pi-ca.pem");
    size_t n = 0;
    unsigned char *bundle = check_read(bundle_path, &n);
    expect(bundle && contains_text(bundle, n, "BEGIN CERTIFICATE"), "certificate");
    expect(bundle && !contains_text(bundle, n, "PRIVATE KEY"), "no key");
    free(bundle);
    free(bundle_path);
    size_t ready_len = 0;
    unsigned char *ready = check_read(fix.ready, &ready_len);
    mode_t mask = umask(0);
    umask(mask);
    expect(ready && near_field((char *)ready, "umask", (double)mask), "umask");
    free(ready);
  }
  fix_end(&fix);
}
static void busy_port(void) {
  check_begin("a busy port cannot launch the wrapped command");
  Fix fix = fix_new(1, M_NONE, 0);
  int held = listen_port(&(int){0});
  int one = 1;
  setsockopt(held, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  close(held);
  held = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)fix.port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  expect(bind(held, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(held, 1) == 0, "bind");
  char *ran = check_join(fix.root, "ran");
  char *touch = check_join(fix.root, "touch.sh");
  char script[256];
  snprintf(script, sizeof script, "#!/bin/sh\ntouch %s\n", ran);
  check_write(touch, script, strlen(script));
  chmod(touch, 0755);
  const char *args[] = {bash_bin, fix.script, "pi", "--", touch, NULL};
  Proc proc = run_fix(&fix, args, 20);
  expect(proc.status != 0 && contains_text(proc.err, proc.err_len, "already in use"), "busy");
  expect(access(ran, F_OK) != 0, "not launched");
  proc_free(&proc);
  close(held);
  free(touch);
  free(ran);
  fix_end(&fix);
}
static void stranded(void) {
  check_begin("stop cleans up a stranded server and permits a new capture");
  Fix fix = fix_new(1, M_NONE, 0);
  if (!fix_start(&fix)) {
    pid_t old = fix.child;
    kill(old, SIGKILL);
    waitpid(old, NULL, 0);
    fix.child = 0;
    char *true_sh = check_join(fix.root, "true.sh");
    check_write(true_sh, "#!/bin/sh\nexit 0\n", 17);
    chmod(true_sh, 0755);
    const char *again[] = {bash_bin, fix.script, "pi", "--", true_sh, NULL};
    Proc failed = run_fix(&fix, again, 20);
    expect(failed.status != 0, "still blocked");
    proc_free(&failed);
    const char *stop[] = {bash_bin, fix.script, "stop", "pi", NULL};
    Proc stopped = run_fix(&fix, stop, 20);
    expect(stopped.status == 0, "stop");
    proc_free(&stopped);
    kill(-old, SIGKILL);
    expect(fix_start(&fix) == 0, "restart");
    free(true_sh);
  }
  fix_end(&fix);
}
static void websocket(void) {
  check_begin("WebSocket text and binary messages keep direction and representation");
  Fix fix = fix_new(1, M_WS, 1);
  if (!fix_start(&fix)) {
    int fd = connect_port(fix.port);
    unsigned char key_raw[16];
    int urandom = open("/dev/urandom", O_RDONLY);
    expect(urandom >= 0 && read_full(urandom, key_raw, sizeof key_raw) == 0, "random");
    if (urandom >= 0)
      close(urandom);
    char *key = b64(key_raw, sizeof key_raw);
    char req[512];
    int wrote = snprintf(req, sizeof req,
                         "GET /socket HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                         fix.port, key);
    expect(write_all(fd, req, (size_t)wrote) == 0, "handshake");
    char *head = NULL;
    unsigned char *rest = NULL;
    size_t rest_len = 0;
    expect(read_http_head(fd, &head, &rest, &rest_len) == 0 && strstr(head, "101"), "switching");
    unsigned char mask[4] = {1, 2, 3, 4};
    unsigned char frame[11] = {0x81, 0x85, 1, 2, 3, 4};
    const char *hello = "hello";
    for (int i = 0; i < 5; i++)
      frame[6 + i] = (unsigned char)hello[i] ^ mask[i % 4];
    expect(write_all(fd, frame, sizeof frame) == 0, "send");
    unsigned char *payloads[2] = {0};
    size_t lengths[2] = {0};
    int opcodes[2] = {0};
    size_t got = 0;
    unsigned char pending[64];
    size_t pending_len = rest_len < sizeof pending ? rest_len : sizeof pending;
    if (pending_len)
      memcpy(pending, rest, pending_len);
    while (got < 2) {
      unsigned char hdr[2];
      if (pending_len >= 2) {
        memcpy(hdr, pending, 2);
        memmove(pending, pending + 2, pending_len - 2);
        pending_len -= 2;
      } else if (read_full(fd, hdr, 2))
        break;
      int opcode = hdr[0] & 0x0f;
      size_t n = hdr[1] & 0x7f;
      if (n == 126) {
        unsigned char ext[2];
        if (read_full(fd, ext, 2))
          break;
        n = ((size_t)ext[0] << 8) | ext[1];
      }
      unsigned char *payload = malloc(n + 1);
      size_t have = pending_len < n ? pending_len : n;
      if (have)
        memcpy(payload, pending, have);
      if (have < pending_len)
        memmove(pending, pending + have, pending_len - have);
      pending_len -= have;
      if (have < n && read_full(fd, payload + have, n - have)) {
        free(payload);
        break;
      }
      payload[n] = 0;
      if (opcode == 1 || opcode == 2) {
        payloads[got] = payload;
        lengths[got] = n;
        opcodes[got] = opcode;
        got++;
      } else
        free(payload);
    }
    expect(got == 2 && opcodes[0] == 1 && lengths[0] == 5 && !memcmp(payloads[0], "world", 5), "text");
    expect(got == 2 && opcodes[1] == 2 && lengths[1] == 2 && payloads[1][0] == 0 && payloads[1][1] == 0xff,
           "binary");
    char *log = check_join(fix.cdir, "pi.jsonl");
    char **lines = NULL;
    size_t count = 0;
    for (int i = 0; i < 50 && got == 2; i++) {
      free_lines(lines, count);
      lines = NULL;
      count = 0;
      read_lines(log, &lines, &count);
      size_t ws = 0;
      for (size_t j = 0; j < count; j++)
        if (kind_prefix(lines[j], "ws_"))
          ws++;
      if (ws >= 3)
        break;
      struct timespec pause = {.tv_nsec = 100 * 1000 * 1000};
      nanosleep(&pause, NULL);
    }
    const char *want_kind[] = {"ws_request", "ws_response", "ws_response"};
    const char *want_data[] = {"hello", "world", "b'\\x00\\xff'"};
    size_t seen = 0;
    for (size_t i = 0; i < count; i++) {
      if (!kind_prefix(lines[i], "ws_"))
        continue;
      unsigned char *kind = NULL, *data = NULL;
      size_t kn = 0, dn = 0;
      string_field(lines[i], "kind", &kind, &kn);
      string_field(lines[i], "data", &data, &dn);
      expect(seen < 3 && kind && data && !strcmp((char *)kind, want_kind[seen]) &&
                 !strcmp((char *)data, want_data[seen]),
             "ws row");
      free(kind);
      free(data);
      seen++;
    }
    expect(seen == 3, "ws count");
    free_lines(lines, count);
    free(payloads[0]);
    free(payloads[1]);
    free(rest);
    free(head);
    free(key);
    free(log);
    close(fd);
  }
  fix_end(&fix);
}
int main(void) {
  char *exe = check_exe_dir();
  adapter = check_join(exe, "prompt-capture-mitm");
  ready_bin = check_join(exe, "capture-ready");
  bash_bin = which("bash");
  flock_bin = which("flock");
  expect(bash_bin && flock_bin && access(adapter, X_OK) == 0, "tools");
  unsigned char dig[20];
  sha1((const unsigned char *)"abc", 3, dig);
  const unsigned char abc[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
  expect(!memcmp(dig, abc, 20), "sha1");
  utf8_stream();
  large_request();
  no_body();
  anthropic();
  reverse_proxy();
  early_sse();
  overlap();
  permissions();
  busy_port();
  stranded();
  websocket();
  check_begin("prompt-capture-checks");
  free(exe);
  return check_finish();
}
