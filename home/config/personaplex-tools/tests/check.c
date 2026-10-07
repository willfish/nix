#define _GNU_SOURCE
#include "tools.h"
#include <curl/curl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>
#include <yyjson.h>

static const char *token = "fixture-not-a-real-hf-token";
static const char *handler =
    "    async def handle_chat(self, request):\n        ws = web.WebSocketResponse()";
static const char *web_root =
    "return web.FileResponse(os.path.join(static_path, \"index.html\"))";
static const char *current_test;
static int failures;
static char *fixture_bin, *models_bin, *patch_bin, *tls_dir, *cert_path, *key_path;

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
static int began;
static void begin(const char *name) {
  current_test = name;
  began = failures;
  printf("RUN %s\n", name);
  fflush(stdout);
}
static void end_case(void) {
  if (failures == began)
    printf("PASS %s\n", current_test);
}

static char *sibling(const char *env, const char *name) {
  const char *v = getenv(env);
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
  char *out, *err;
  size_t out_len, err_len;
  bool spawn_error;
} Proc;

static void proc_clear(Proc *p) {
  g_free(p->out);
  g_free(p->err);
  memset(p, 0, sizeof *p);
}

static bool grow(char **buf, size_t *len, size_t *cap, const void *add, size_t n) {
  if (*len > SIZE_MAX - n - 1)
    return false;
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + n + 1) {
      if (next > SIZE_MAX / 2)
        return false;
      next *= 2;
    }
    char *p = g_try_realloc(*buf, next);
    if (!p)
      return false;
    *buf = p;
    *cap = next;
  }
  if (n)
    memcpy(*buf + *len, add, n);
  *len += n;
  (*buf)[*len] = 0;
  return true;
}

static Proc proc_run(const char *cmd, char *const *argv, const void *input,
                     size_t input_len, const char *const *env_set,
                     const char *const *env_unset, int timeout_ms) {
  Proc p = {.status = -1};
  int outp[2] = {-1, -1}, errp[2] = {-1, -1}, inp[2] = {-1, -1};
  if (pipe(outp) || pipe(errp) || pipe(inp)) {
    p.spawn_error = true;
    return p;
  }
  pid_t pid = fork();
  if (pid < 0) {
    p.spawn_error = true;
    return p;
  }
  if (pid == 0) {
    dup2(inp[0], 0);
    dup2(outp[1], 1);
    dup2(errp[1], 2);
    close(inp[0]);
    close(inp[1]);
    close(outp[0]);
    close(outp[1]);
    close(errp[0]);
    close(errp[1]);
    if (env_unset)
      for (size_t i = 0; env_unset[i]; i++)
        unsetenv(env_unset[i]);
    if (env_set)
      for (size_t i = 0; env_set[i]; i++)
        putenv((char *)env_set[i]);
    execvp(cmd, argv);
    _exit(127);
  }
  close(inp[0]);
  close(outp[1]);
  close(errp[1]);
  size_t in_off = 0, ocap = 0, ecap = 0;
  int64_t deadline = now_ms() + timeout_ms;
  bool done = false;
  while (!done) {
    if (now_ms() > deadline) {
      kill(pid, SIGKILL);
      p.spawn_error = true;
      break;
    }
    struct pollfd fds[3];
    nfds_t nfd = 0;
    int ii = -1, oi = -1, ei = -1;
    if (inp[1] >= 0 && input && in_off < input_len) {
      ii = (int)nfd;
      fds[nfd++] = (struct pollfd){inp[1], POLLOUT, 0};
    }
    if (outp[0] >= 0) {
      oi = (int)nfd;
      fds[nfd++] = (struct pollfd){outp[0], POLLIN, 0};
    }
    if (errp[0] >= 0) {
      ei = (int)nfd;
      fds[nfd++] = (struct pollfd){errp[0], POLLIN, 0};
    }
    poll(fds, nfd, 50);
    if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
      ssize_t w = write(inp[1], (const char *)input + in_off, input_len - in_off);
      if (w > 0)
        in_off += (size_t)w;
      else
        in_off = input_len;
    }
    if (inp[1] >= 0 && (!input || in_off >= input_len)) {
      close(inp[1]);
      inp[1] = -1;
    }
    char tmp[4096];
    if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP))) {
      ssize_t r = read(outp[0], tmp, sizeof tmp);
      if (r > 0)
        grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r);
      else {
        close(outp[0]);
        outp[0] = -1;
      }
    }
    if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP))) {
      ssize_t r = read(errp[0], tmp, sizeof tmp);
      if (r > 0)
        grow(&p.err, &p.err_len, &ecap, tmp, (size_t)r);
      else {
        close(errp[0]);
        errp[0] = -1;
      }
    }
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == pid) {
      done = true;
      p.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
  }
  if (inp[1] >= 0)
    close(inp[1]);
  if (!done) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
  if (outp[0] >= 0) {
    char tmp[4096];
    ssize_t r;
    while ((r = read(outp[0], tmp, sizeof tmp)) > 0)
      grow(&p.out, &p.out_len, &ocap, tmp, (size_t)r);
    close(outp[0]);
  }
  if (errp[0] >= 0) {
    char tmp[4096];
    ssize_t r;
    while ((r = read(errp[0], tmp, sizeof tmp)) > 0)
      grow(&p.err, &p.err_len, &ecap, tmp, (size_t)r);
    close(errp[0]);
  }
  if (!p.out)
    grow(&p.out, &p.out_len, &ocap, "", 0);
  if (!p.err)
    grow(&p.err, &p.err_len, &ecap, "", 0);
  return p;
}

static char *env_join(const char *key, const char *value) {
  return g_strconcat(key, "=", value, NULL);
}

static Proc invoke(const char *cmd, char *const *args, const void *input, size_t input_len,
                   const char *const *extra, const char *const *unset, int timeout_ms) {
  GPtrArray *set = g_ptr_array_new_with_free_func(g_free);
  bool skip_token = false, skip_home = false, skip_ssl = false;
  if (unset) {
    for (size_t i = 0; unset[i]; i++) {
      if (!strcmp(unset[i], "HF_TOKEN"))
        skip_token = true;
      if (!strcmp(unset[i], "HF_HOME"))
        skip_home = true;
      if (!strcmp(unset[i], "SSL_CERT_FILE"))
        skip_ssl = true;
    }
  }
  if (extra) {
    for (size_t i = 0; extra[i]; i++) {
      if (g_str_has_prefix(extra[i], "HF_TOKEN="))
        skip_token = true;
      if (g_str_has_prefix(extra[i], "HF_HOME="))
        skip_home = true;
      if (g_str_has_prefix(extra[i], "SSL_CERT_FILE="))
        skip_ssl = true;
      g_ptr_array_add(set, g_strdup(extra[i]));
    }
  }
  if (!skip_token)
    g_ptr_array_add(set, env_join("HF_TOKEN", token));
  if (!skip_home)
    g_ptr_array_add(set, env_join("HF_HOME", tls_dir));
  if (!skip_ssl && cert_path)
    g_ptr_array_add(set, env_join("SSL_CERT_FILE", cert_path));
  g_ptr_array_add(set, NULL);
  Proc p = proc_run(cmd, args, input, input_len, (const char *const *)set->pdata, unset,
                    timeout_ms);
  g_ptr_array_free(set, TRUE);
  return p;
}

static char *temp_dir(const char *prefix) {
  char *tmpl = g_build_filename(g_get_tmp_dir(), prefix, NULL);
  char *made = g_mkdtemp(tmpl);
  if (!made)
    g_free(tmpl);
  return made;
}

static void rm_rf(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) {
    chmod(path, 0700);
    unlink(path);
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(dir))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    char *child = g_build_filename(path, ent->d_name, NULL);
    struct stat st;
    if (!lstat(child, &st) && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
      rm_rf(child);
    else
      unlink(child);
    g_free(child);
  }
  closedir(dir);
  rmdir(path);
}

static bool write_file(const char *path, const void *data, size_t len, mode_t mode) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0)
    return false;
  const char *s = data;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, s + off, len - off);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      return false;
    }
    off += (size_t)n;
  }
  if (mode && fchmod(fd, mode) != 0) {
    close(fd);
    return false;
  }
  return close(fd) == 0;
}

static bool read_file(const char *path, char **data, size_t *len) {
  gsize n = 0;
  if (!g_file_get_contents(path, data, &n, NULL))
    return false;
  *len = n;
  return true;
}

static bool contains(const char *hay, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (!hay || m > n)
    return false;
  for (size_t i = 0; i + m <= n; i++)
    if (!memcmp(hay + i, needle, m))
      return true;
  return false;
}

static int count_substr(const char *hay, size_t n, const char *needle) {
  size_t m = strlen(needle);
  int c = 0;
  if (m == 0 || m > n)
    return 0;
  for (size_t i = 0; i + m <= n; i++)
    if (!memcmp(hay + i, needle, m))
      c++;
  return c;
}

static bool has_partial(const char *path) {
  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    return false;
  DIR *dir = opendir(path);
  if (!dir)
    return g_str_has_suffix(path, ".partial");
  struct dirent *ent;
  bool bad = false;
  while (!bad && (ent = readdir(dir))) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    if (g_str_has_suffix(ent->d_name, ".partial"))
      bad = true;
    char *child = g_build_filename(path, ent->d_name, NULL);
    struct stat st;
    if (!bad && !lstat(child, &st) && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
      bad = has_partial(child);
    g_free(child);
  }
  closedir(dir);
  return bad;
}

typedef struct SslApi {
  void *lib;
  const void *(*TLS_server_method)(void);
  void *(*SSL_CTX_new)(const void *);
  int (*SSL_CTX_use_certificate_file)(void *, const char *, int);
  int (*SSL_CTX_use_PrivateKey_file)(void *, const char *, int);
  void *(*SSL_new)(void *);
  int (*SSL_set_fd)(void *, int);
  int (*SSL_accept)(void *);
  int (*SSL_read)(void *, void *, int);
  int (*SSL_write)(void *, const void *, int);
  int (*SSL_shutdown)(void *);
  void (*SSL_free)(void *);
  void (*SSL_CTX_free)(void *);
} SslApi;

static SslApi ssl_api;

static bool load_ssl(void) {
  FILE *maps = fopen("/proc/self/maps", "r");
  char path[1024] = {0};
  if (maps) {
    char line[1024];
    while (fgets(line, sizeof line, maps)) {
      char *at = strstr(line, "libssl.so");
      if (!at)
        continue;
      char *start = strchr(line, '/');
      if (!start)
        continue;
      size_t n = strcspn(start, " \n");
      if (n < sizeof path) {
        memcpy(path, start, n);
        path[n] = 0;
        break;
      }
    }
    fclose(maps);
  }
  void *lib = path[0] ? dlopen(path, RTLD_NOW) : dlopen("libssl.so.3", RTLD_NOW);
  if (!lib)
    return false;
  ssl_api.lib = lib;
#define LOAD(name)                                                             \
  do {                                                                         \
    void *sym = dlsym(lib, #name);                                             \
    if (!sym)                                                                  \
      return false;                                                            \
    memcpy(&ssl_api.name, &sym, sizeof ssl_api.name);                          \
  } while (0)
  LOAD(TLS_server_method);
  LOAD(SSL_CTX_new);
  LOAD(SSL_CTX_use_certificate_file);
  LOAD(SSL_CTX_use_PrivateKey_file);
  LOAD(SSL_new);
  LOAD(SSL_set_fd);
  LOAD(SSL_accept);
  LOAD(SSL_read);
  LOAD(SSL_write);
  LOAD(SSL_shutdown);
  LOAD(SSL_free);
  LOAD(SSL_CTX_free);
#undef LOAD
  return true;
}

typedef struct {
  char *path;
  char *auth;
} Req;

typedef struct {
  char *name;
  unsigned char *data;
  size_t len;
} Blob;

enum {
  H_SERVE = 1,
  H_403,
  H_OVERSIZE,
  H_HASH,
  H_LENGTH,
  H_CHUNKED,
  H_REDIRECT_SAME,
  H_REDIRECT_OTHER,
  H_REDIRECT_ORIG,
  H_REDIRECT_CASE,
  H_SLOW,
  H_STALL,
  H_REDIRECT_TO,
  H_NEVER
};

typedef struct Server {
  int fd;
  int port;
  char *url;
  GThread *thread;
  GMutex mu;
  Req *reqs;
  size_t nreqs, cap;
  volatile int stop;
  void *ctx;
  int kind;
  Blob *blobs;
  size_t nblobs;
  char *peer;
  char *location;
  char *key;
  char *cert;
} Server;

static Blob *find_blob(Server *s, const char *name) {
  for (size_t i = 0; i < s->nblobs; i++)
    if (!strcmp(s->blobs[i].name, name))
      return &s->blobs[i];
  return NULL;
}

static bool ssl_write_all(void *ssl, const void *data, size_t n) {
  const unsigned char *p = data;
  while (n) {
    int w = ssl_api.SSL_write(ssl, p, n > 16384 ? 16384 : (int)n);
    if (w <= 0)
      return false;
    p += w;
    n -= (size_t)w;
  }
  return true;
}

static void http_status(void *ssl, int code, const char *extra, const void *body, size_t n) {
  GString *h = g_string_new(NULL);
  g_string_append_printf(h, "HTTP/1.1 %d %s\r\nConnection: close\r\n", code,
                         code == 200 ? "OK" : code == 302 ? "Found" : "Forbidden");
  if (extra)
    g_string_append(h, extra);
  if (body)
    g_string_append_printf(h, "Content-Length: %zu\r\n", n);
  g_string_append(h, "\r\n");
  ssl_write_all(ssl, h->str, h->len);
  g_string_free(h, TRUE);
  if (body && n)
    ssl_write_all(ssl, body, n);
}

static const char *blob_name(const char *path, bool final_only) {
  if (final_only) {
    if (g_str_has_prefix(path, "/final/"))
      return path + 7;
    return NULL;
  }
  if (g_str_has_prefix(path, "/final/"))
    return path + 7;
  return path[0] == '/' ? path + 1 : path;
}

static void serve_blob(Server *s, void *ssl, const char *path, bool delayed) {
  const char *name = blob_name(path, false);
  Blob *b = name ? find_blob(s, name) : NULL;
  if (!b) {
    http_status(ssl, 404, NULL, "missing", 7);
    return;
  }
  if (!delayed) {
    http_status(ssl, 200, NULL, b->data, b->len);
    return;
  }
  GString *h = g_string_new(NULL);
  g_string_append_printf(h,
                         "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n",
                         b->len);
  ssl_write_all(ssl, h->str, h->len);
  g_string_free(h, TRUE);
  for (size_t off = 0; off < b->len;) {
    g_usleep(40 * 1000);
    size_t n = b->len - off > 8 ? 8 : b->len - off;
    if (!ssl_write_all(ssl, b->data + off, n))
      return;
    off += n;
  }
  g_usleep(40 * 1000);
}

static void handle_one(Server *s, int cfd) {
  struct timeval tv = {.tv_sec = 5};
  setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  void *ssl = ssl_api.SSL_new(s->ctx);
  ssl_api.SSL_set_fd(ssl, cfd);
  if (ssl_api.SSL_accept(ssl) != 1) {
    ssl_api.SSL_free(ssl);
    close(cfd);
    return;
  }
  char buf[8192];
  size_t n = 0;
  while (n < sizeof buf - 1) {
    int r = ssl_api.SSL_read(ssl, buf + n, (int)(sizeof buf - 1 - n));
    if (r <= 0)
      break;
    n += (size_t)r;
    buf[n] = 0;
    if (strstr(buf, "\r\n\r\n"))
      break;
  }
  if (!strstr(buf, "\r\n\r\n") || strncmp(buf, "GET ", 4)) {
    ssl_api.SSL_free(ssl);
    close(cfd);
    return;
  }
  char *sp = strchr(buf + 4, ' ');
  char *path = sp ? g_strndup(buf + 4, (size_t)(sp - (buf + 4))) : g_strdup("/");
  char *auth = NULL;
  char *line = buf;
  while (line && *line) {
    char *eol = strstr(line, "\r\n");
    size_t ln = eol ? (size_t)(eol - line) : strlen(line);
    if (ln >= 14 && !g_ascii_strncasecmp(line, "Authorization:", 14)) {
      const char *v = line + 14;
      size_t vn = ln - 14;
      while (vn && (*v == ' ' || *v == '\t')) {
        v++;
        vn--;
      }
      auth = g_strndup(v, vn);
      break;
    }
    if (!eol || ln == 0)
      break;
    line = eol + 2;
  }
  g_mutex_lock(&s->mu);
  if (s->nreqs == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 8;
    Req *next = g_try_realloc(s->reqs, s->cap * sizeof *s->reqs);
    if (next) {
      s->reqs = next;
      s->reqs[s->nreqs].path = path;
      s->reqs[s->nreqs].auth = auth;
      s->nreqs++;
      path = NULL;
      auth = NULL;
    }
  } else {
    s->reqs[s->nreqs].path = path;
    s->reqs[s->nreqs].auth = auth;
    s->nreqs++;
    path = NULL;
    auth = NULL;
  }
  g_mutex_unlock(&s->mu);
  g_free(path);
  g_free(auth);
  const char *req_path = s->reqs[s->nreqs - 1].path;
  if (s->kind == H_403)
    http_status(ssl, 403, "Content-Length: 10000\r\n", NULL, 0);
  else if (s->kind == H_OVERSIZE) {
    char *body = g_malloc(1000);
    memset(body, 'x', 1000);
    http_status(ssl, 200, NULL, body, 1000);
    g_free(body);
  } else if (s->kind == H_HASH)
    http_status(ssl, 200, NULL, "wrong-size", 10);
  else if (s->kind == H_LENGTH) {
    const char *wanted = blob_name(req_path, false);
    Blob *b = find_blob(s, wanted);
    if (b) {
      char extra[64];
      snprintf(extra, sizeof extra, "Content-Length: %zu\r\n", b->len + 100);
      GString *h = g_string_new(NULL);
      g_string_append_printf(h, "HTTP/1.1 200 OK\r\nConnection: close\r\n%s\r\n", extra);
      ssl_write_all(ssl, h->str, h->len);
      g_string_free(h, TRUE);
      ssl_write_all(ssl, b->data, b->len);
    }
  } else if (s->kind == H_CHUNKED) {
    Blob *b = find_blob(s, blob_name(req_path, false));
    if (b) {
      char hex[32];
      snprintf(hex, sizeof hex, "%zx", b->len);
      GString *msg = g_string_new(NULL);
      g_string_append(msg, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
      g_string_append(msg, hex);
      g_string_append(msg, "\r\n");
      g_string_append_len(msg, (char *)b->data, (gssize)b->len);
      g_string_append(msg, "\r\n");
      ssl_write_all(ssl, msg->str, msg->len);
      g_string_free(msg, TRUE);
    }
  } else if (s->kind == H_REDIRECT_SAME) {
    if (g_str_has_prefix(req_path, "/final/"))
      serve_blob(s, ssl, req_path, false);
    else {
      char *loc = g_strconcat("Location: /final", req_path, "\r\n", NULL);
      http_status(ssl, 302, loc, "ignored body", 12);
      g_free(loc);
    }
  } else if (s->kind == H_REDIRECT_OTHER) {
    char *loc = g_strconcat("Location: ", s->peer, "/final", req_path, "\r\n", NULL);
    http_status(ssl, 302, loc, "", 0);
    g_free(loc);
  } else if (s->kind == H_REDIRECT_ORIG) {
    if (g_str_has_prefix(req_path, "/final/"))
      serve_blob(s, ssl, req_path, false);
    else {
      char *loc = g_strconcat("Location: ", s->peer, req_path, "\r\n", NULL);
      http_status(ssl, 302, loc, "", 0);
      g_free(loc);
    }
  } else if (s->kind == H_REDIRECT_CASE) {
    if (g_str_has_prefix(req_path, "/final/"))
      serve_blob(s, ssl, req_path, false);
    else {
      char *alt = g_strdup(s->url);
      char *ip = strstr(alt, "127.0.0.1");
      if (ip)
        memcpy(ip, "LOCALHOST", 9);
      /* 127.0.0.1 is 9 chars, LOCALHOST is 9 chars */
      char *loc = g_strconcat("Location: ", alt, "/final", req_path, "\r\n", NULL);
      http_status(ssl, 302, loc, "", 0);
      g_free(loc);
      g_free(alt);
    }
  } else if (s->kind == H_SLOW)
    serve_blob(s, ssl, req_path, true);
  else if (s->kind == H_STALL) {
    ssl_write_all(ssl, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n", 36);
    char sink[64];
    while (ssl_api.SSL_read(ssl, sink, sizeof sink) > 0)
      ;
  } else if (s->kind == H_REDIRECT_TO) {
    char *loc = g_strconcat("Location: ", s->location, "\r\n", NULL);
    http_status(ssl, 302, loc, "", 0);
    g_free(loc);
  } else if (s->kind == H_NEVER)
    http_status(ssl, 200, NULL, "never read", 10);
  else
    serve_blob(s, ssl, req_path, false);
  ssl_api.SSL_shutdown(ssl);
  ssl_api.SSL_free(ssl);
  close(cfd);
}

static gpointer serve_loop(gpointer data) {
  Server *s = data;
  while (!s->stop) {
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    int cfd = accept(s->fd, (struct sockaddr *)&addr, &len);
    if (cfd < 0)
      continue;
    if (s->stop) {
      close(cfd);
      break;
    }
    handle_one(s, cfd);
  }
  return NULL;
}

static Server *server_new(int kind, const char *key, const char *cert) {
  Server *s = g_new0(Server, 1);
  s->kind = kind;
  s->fd = -1;
  s->key = g_strdup(key ? key : key_path);
  s->cert = g_strdup(cert ? cert : cert_path);
  g_mutex_init(&s->mu);
  s->ctx = ssl_api.SSL_CTX_new(ssl_api.TLS_server_method());
  if (!s->ctx || ssl_api.SSL_CTX_use_certificate_file(s->ctx, s->cert, 1) != 1 ||
      ssl_api.SSL_CTX_use_PrivateKey_file(s->ctx, s->key, 1) != 1) {
    g_free(s->key);
    g_free(s->cert);
    g_free(s);
    return NULL;
  }
  s->fd = socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(s->fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s->fd, 16) != 0) {
    close(s->fd);
    ssl_api.SSL_CTX_free(s->ctx);
    g_free(s->key);
    g_free(s->cert);
    g_free(s);
    return NULL;
  }
  socklen_t alen = sizeof addr;
  getsockname(s->fd, (struct sockaddr *)&addr, &alen);
  s->port = ntohs(addr.sin_port);
  s->url = g_strdup_printf("https://127.0.0.1:%d", s->port);
  s->thread = g_thread_new("tls", serve_loop, s);
  return s;
}

static void server_stop(Server *s) {
  if (!s)
    return;
  s->stop = 1;
  int poke = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)s->port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (poke >= 0) {
    connect(poke, (struct sockaddr *)&addr, sizeof addr);
    close(poke);
  }
  shutdown(s->fd, SHUT_RDWR);
  g_thread_join(s->thread);
  close(s->fd);
  ssl_api.SSL_CTX_free(s->ctx);
  for (size_t i = 0; i < s->nreqs; i++) {
    g_free(s->reqs[i].path);
    g_free(s->reqs[i].auth);
  }
  g_free(s->reqs);
  g_free(s->url);
  g_free(s->peer);
  g_free(s->location);
  g_free(s->key);
  g_free(s->cert);
  g_mutex_clear(&s->mu);
  g_free(s);
}

static bool make_cert(const char *key, const char *cert, const char *cn, const char *san) {
  char *argv[] = {"openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
                  (char *)key, "-out", (char *)cert, "-days", "1", "-subj", (char *)cn,
                  "-addext", (char *)san, NULL};
  Proc r = proc_run("openssl", argv, NULL, 0, NULL, NULL, 20000);
  bool ok = !r.spawn_error && r.status == 0;
  if (!ok)
    FAIL("openssl cert: %s", r.err ? r.err : "");
  proc_clear(&r);
  return ok;
}

static void tar_field(unsigned char *h, size_t off, size_t len, const char *value) {
  size_t n = strlen(value);
  if (n > len)
    n = len;
  memcpy(h + off, value, n);
}

static void tar_octal(unsigned char *dst, size_t len, uint64_t value) {
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%0*llo", (int)(len - 1), (unsigned long long)value);
  memcpy(dst, tmp, len - 1);
  dst[len - 1] = 0;
}

static unsigned char *tar_gzip(const char names[][100], const char *types, const unsigned *modes,
                               const char *links[], const char *datas[], size_t count, size_t *out_n) {
  GByteArray *tar = g_byte_array_new();
  for (size_t i = 0; i < count; i++) {
    unsigned char h[512] = {0};
    tar_field(h, 0, 100, names[i]);
    tar_octal(h + 100, 8, modes[i]);
    tar_octal(h + 108, 8, 0);
    tar_octal(h + 116, 8, 0);
    size_t dlen = datas[i] ? strlen(datas[i]) : 0;
    tar_octal(h + 124, 12, dlen);
    tar_octal(h + 136, 12, 1234567890);
    memset(h + 148, ' ', 8);
    h[156] = types[i] ? (unsigned char)types[i] : '0';
    if (links[i])
      tar_field(h, 157, 100, links[i]);
    tar_field(h, 257, 6, "ustar");
    h[262] = 0;
    tar_field(h, 263, 2, "00");
    unsigned sum = 0;
    for (size_t b = 0; b < 512; b++)
      sum += h[b];
    char chk[16];
    snprintf(chk, sizeof chk, "%06o", sum);
    memcpy(h + 148, chk, 6);
    h[154] = 0;
    h[155] = ' ';
    g_byte_array_append(tar, h, 512);
    if (dlen) {
      g_byte_array_append(tar, (const guint8 *)datas[i], dlen);
      size_t pad = (512 - (dlen % 512)) % 512;
      if (pad) {
        unsigned char z[512] = {0};
        g_byte_array_append(tar, z, pad);
      }
    }
  }
  unsigned char z[1024] = {0};
  g_byte_array_append(tar, z, 1024);
  char *raw = temp_dir("personaplex-tar-XXXXXX");
  char *file = g_build_filename(raw, "a.tar", NULL);
  write_file(file, tar->data, tar->len, 0644);
  g_byte_array_free(tar, TRUE);
  char *argv[] = {"gzip", "-n", "-c", file, NULL};
  Proc r = proc_run("gzip", argv, NULL, 0, NULL, NULL, 10000);
  unsigned char *out = NULL;
  if (!r.spawn_error && r.status == 0) {
    out = g_malloc(r.out_len);
    memcpy(out, r.out, r.out_len);
    *out_n = r.out_len;
  }
  proc_clear(&r);
  rm_rf(raw);
  g_free(file);
  g_free(raw);
  return out;
}

typedef struct {
  char *dir, *destination, *manifest;
  Blob blobs[4];
} Data;

static char *sha256_hex(const unsigned char *data, size_t n) {
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(sum, data, n);
  char *hex = g_strdup(g_checksum_get_string(sum));
  g_checksum_free(sum);
  return hex;
}

static Data *data_new(void) {
  Data *d = g_new0(Data, 1);
  d->dir = temp_dir("personaplex-tools-XXXXXX");
  d->destination = g_build_filename(d->dir, "data", NULL);
  d->manifest = g_build_filename(d->dir, "assets.json", NULL);
  const char *names[] = {"model.safetensors", "tokenizer.bin", "voices.tgz", "dist.tgz"};
  unsigned char *voice = NULL, *dist = NULL;
  size_t voice_n = 0, dist_n = 0;
  const char n1[][100] = {"voices", "voices/NATF2.pt"};
  unsigned m1[] = {0777, 0644};
  const char *l1[] = {NULL, NULL};
  const char *b1[] = {NULL, "voice-data"};
  char types1[] = {'5', '0'};
  voice = tar_gzip(n1, types1, m1, l1, b1, 2, &voice_n);
  const char n2[][100] = {"dist", "dist/index.html"};
  unsigned m2[] = {0755, 0644};
  const char *b2[] = {NULL, "<head>test</head>"};
  char types2[] = {'5', '0'};
  dist = tar_gzip(n2, types2, m2, l1, b2, 2, &dist_n);
  const char *raw[] = {"model-data", "tokenizer-data"};
  GString *json = g_string_new("[");
  for (int i = 0; i < 4; i++) {
    d->blobs[i].name = g_strdup(names[i]);
    if (i < 2) {
      d->blobs[i].len = strlen(raw[i]);
      d->blobs[i].data = g_malloc(d->blobs[i].len);
      memcpy(d->blobs[i].data, raw[i], d->blobs[i].len);
    } else if (i == 2) {
      d->blobs[i].data = voice;
      d->blobs[i].len = voice_n;
    } else {
      d->blobs[i].data = dist;
      d->blobs[i].len = dist_n;
    }
    char *hex = sha256_hex(d->blobs[i].data, d->blobs[i].len);
    if (i)
      g_string_append_c(json, ',');
    g_string_append_printf(json, "{\"name\":\"%s\",\"bytes\":%zu,\"sha256\":\"%s\"}", names[i],
                           d->blobs[i].len, hex);
    g_free(hex);
  }
  g_string_append_c(json, ']');
  write_file(d->manifest, json->str, json->len, 0644);
  g_string_free(json, TRUE);
  return d;
}

static void data_free(Data *d) {
  if (!d)
    return;
  for (int i = 0; i < 4; i++) {
    g_free(d->blobs[i].name);
    g_free(d->blobs[i].data);
  }
  rm_rf(d->dir);
  g_free(d->dir);
  g_free(d->destination);
  g_free(d->manifest);
  g_free(d);
}

static void server_blobs(Server *s, Data *d) {
  s->blobs = d->blobs;
  s->nblobs = 4;
}

static Proc install_run(Data *d, const char *base, bool check, const char *idle,
                        const char *const *extra, const char *const *unset) {
  char *argv[] = {fixture_bin, "install", d->destination, d->manifest, (char *)base,
                  check ? "1" : "0", (char *)(idle ? idle : "2000"), NULL};
  return invoke(fixture_bin, argv, NULL, 0, extra, unset, 30000);
}

static bool auth_is(const Req *q, const char *want) {
  if (!want)
    return q->auth == NULL;
  return q->auth && !strcmp(q->auth, want);
}

static void test_assets(void) {
  begin("manifest pins all five assets, the revision, order, 64-bit sizes and timeout");
  char *argv[] = {fixture_bin, "assets", NULL};
  Proc r = invoke(fixture_bin, argv, NULL, 0, NULL, NULL, 5000);
  EXPECT(!r.spawn_error && r.status == 0);
  yyjson_doc *doc = yyjson_read(r.out, r.out_len, 0);
  yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
  EXPECT(root && !strcmp(yyjson_get_str(yyjson_obj_get(root, "base")),
                         "https://huggingface.co/nvidia/personaplex-7b-v1/resolve/"
                         "fdaf4090a61cb315c138a1faee287ffd6c716309"));
  EXPECT(root && yyjson_get_uint(yyjson_obj_get(root, "idle_ms")) == 60000);
  yyjson_val *assets = root ? yyjson_obj_get(root, "assets") : NULL;
  const char *names[] = {"model.safetensors", "tokenizer-e351c8d8-checkpoint125.safetensors",
                         "tokenizer_spm_32k_3.model", "voices.tgz", "dist.tgz"};
  uint64_t bytes[] = {16742874000ULL, 384644900ULL, 552778ULL, 6095521ULL, 598195ULL};
  const char *hashes[] = {
      "db1290db583cdaa6cb4de444ed279e0b586ca2a372b41434b07a7461c8c0e2f4",
      "09b782f0629851a271227fb9d36db65c041790365f11bbe5d3d59369cf863f50",
      "78d4336533ddc26f9acf7250d7fb83492152196c6ea4212c841df76933f18d2d",
      "8564e9ca7a06ca723b07c3a77c623f0faa5937d04b2647b3a727b06c5ca0b7bb",
      "8de47fe2477491fac3dca404185d0430c8d39f9e0daf4c90ada5f03fdf830f45"};
  EXPECT(assets && yyjson_arr_size(assets) == 5);
  for (size_t i = 0; assets && i < 5; i++) {
    yyjson_val *a = yyjson_arr_get(assets, i);
    EXPECT(!strcmp(yyjson_get_str(yyjson_obj_get(a, "name")), names[i]));
    EXPECT(yyjson_get_uint(yyjson_obj_get(a, "bytes")) == bytes[i]);
    EXPECT(!strcmp(yyjson_get_str(yyjson_obj_get(a, "sha256")), hashes[i]));
  }
  yyjson_doc_free(doc);
  proc_clear(&r);
  end_case();
}

static void test_cli(void) {
  begin("public CLI validates arguments and check-only creates a private root without accessing credentials");
  char *empty[] = {models_bin, NULL};
  char *bad[] = {models_bin, "--bad", NULL};
  char *nodir[] = {models_bin, "--data-dir", NULL};
  char **sets[] = {empty, bad, nodir};
  for (size_t i = 0; i < 3; i++) {
    Proc r = invoke(models_bin, sets[i], NULL, 0, NULL, NULL, 5000);
    EXPECT(r.status == 2);
    proc_clear(&r);
  }
  char *help[] = {models_bin, "--help", NULL};
  Proc r = invoke(models_bin, help, NULL, 0, NULL, NULL, 5000);
  EXPECT(r.status == 0);
  proc_clear(&r);
  char *dir = temp_dir("personaplex-tools-XXXXXX");
  char *path = g_build_filename(dir, "parent/data", NULL);
  char *absent = g_build_filename(dir, "absent", NULL);
  char *argv[] = {models_bin, "--data-dir", path, "--check-only", NULL};
  char *home = env_join("HF_HOME", absent);
  const char *extra[] = {home, NULL};
  const char *unset[] = {"HF_TOKEN", NULL};
  r = invoke(models_bin, argv, NULL, 0, extra, unset, 5000);
  EXPECT(r.status == 1);
  EXPECT(contains(r.err, r.err_len, "PersonaPlex setup: Missing or invalid model.safetensors"));
  struct stat st;
  EXPECT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0700);
  DIR *d = opendir(path);
  int n = 0;
  struct dirent *ent;
  while (d && (ent = readdir(d)))
    if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
      n++;
  if (d)
    closedir(d);
  EXPECT(n == 0);
  proc_clear(&r);
  g_free(home);
  g_free(path);
  g_free(absent);
  rm_rf(dir);
  g_free(dir);
  end_case();
}

static void test_credentials(void) {
  begin("credentials prefer the environment, fall back through HF_HOME, and trim Python Unicode whitespace");
  char *dir = temp_dir("personaplex-tools-XXXXXX");
  char *file = g_build_filename(dir, "token", NULL);
  char *body = g_strconcat("\u2007", token, "\xC2\x85\x1c", NULL);
  EXPECT(write_file(file, body, strlen(body), 0600));
  char *argv[] = {fixture_bin, "token", (char *)token, NULL};
  const char *values[] = {token, ""};
  for (size_t i = 0; i < 2; i++) {
    char *hf = env_join("HF_TOKEN", values[i]);
    char *home = env_join("HF_HOME", dir);
    const char *extra[] = {hf, home, NULL};
    Proc r = invoke(fixture_bin, argv, NULL, 0, extra, NULL, 5000);
    EXPECT(r.status == 0);
    EXPECT(r.out && !strcmp(r.out, "match\n"));
    proc_clear(&r);
    g_free(hf);
    g_free(home);
  }
  char *home = temp_dir("personaplex-tools-XXXXXX");
  char *cache = g_build_filename(home, ".cache/huggingface", NULL);
  g_mkdir_with_parents(cache, 0700);
  char *tok = g_build_filename(cache, "token", NULL);
  char *line = g_strconcat(token, "\n", NULL);
  EXPECT(write_file(tok, line, strlen(line), 0600));
  char *home_env = env_join("HOME", home);
  const char *extra[] = {home_env, NULL};
  const char *unset[] = {"HF_TOKEN", "HF_HOME", NULL};
  Proc r = invoke(fixture_bin, argv, NULL, 0, extra, unset, 5000);
  EXPECT(r.status == 0);
  proc_clear(&r);
  g_free(body);
  g_free(file);
  g_free(line);
  g_free(tok);
  g_free(cache);
  g_free(home_env);
  rm_rf(dir);
  rm_rf(home);
  g_free(dir);
  g_free(home);
  end_case();
}

static void test_whitespace(void) {
  begin("token files strip every Python whitespace character but explicit environment tokens remain literal");
  char *dir = temp_dir("personaplex-tools-XXXXXX");
  char *file = g_build_filename(dir, "token", NULL);
  gunichar cps[] = {9, 10, 11, 12, 13, 28, 29, 30, 31, 32, 0x85, 0xa0, 0x1680, 0x2000,
                    0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009,
                    0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000};
  char *argv[] = {fixture_bin, "token", (char *)token, NULL};
  for (size_t i = 0; i < G_N_ELEMENTS(cps); i++) {
    char sp[8];
    int n = g_unichar_to_utf8(cps[i], sp);
    GString *body = g_string_new(NULL);
    g_string_append_len(body, sp, n);
    g_string_append(body, token);
    g_string_append_len(body, sp, n);
    EXPECT(write_file(file, body->str, body->len, 0600));
    g_string_free(body, TRUE);
    char *home = env_join("HF_HOME", dir);
    const char *extra[] = {"HF_TOKEN=", home, NULL};
    Proc r = invoke(fixture_bin, argv, NULL, 0, extra, NULL, 5000);
    EXPECT(r.status == 0);
    EXPECT(r.out && !strcmp(r.out, "match\n"));
    proc_clear(&r);
    g_free(home);
  }
  char *raw = g_strconcat("\v", token, "\v", NULL);
  char *want[] = {fixture_bin, "token", raw, NULL};
  char *hf = env_join("HF_TOKEN", raw);
  char *home = env_join("HF_HOME", dir);
  const char *extra[] = {hf, home, NULL};
  Proc r = invoke(fixture_bin, want, NULL, 0, extra, NULL, 5000);
  EXPECT(r.status == 0);
  EXPECT(r.out && !strcmp(r.out, "match\n"));
  proc_clear(&r);
  g_free(raw);
  g_free(hf);
  g_free(home);
  g_free(file);
  rm_rf(dir);
  g_free(dir);
  end_case();
}

static void test_bad_credentials(void) {
  begin("missing or malformed credentials fail closed without printing their contents");
  char *dir = temp_dir("personaplex-tools-XXXXXX");
  char *argv[] = {fixture_bin, "token", (char *)token, NULL};
  char *home = env_join("HF_HOME", dir);
  const char *unset[] = {"HF_TOKEN", NULL};
  const char *extra[] = {home, NULL};
  Proc r = invoke(fixture_bin, argv, NULL, 0, extra, unset, 5000);
  EXPECT(r.status == 1);
  EXPECT(!contains(r.err, r.err_len, "private"));
  proc_clear(&r);
  char *bad = env_join("HF_TOKEN", "private\r\ninjected");
  const char *extra2[] = {bad, home, NULL};
  r = invoke(fixture_bin, argv, NULL, 0, extra2, NULL, 5000);
  EXPECT(r.status == 1);
  EXPECT(!contains(r.err, r.err_len, "private"));
  proc_clear(&r);
  unsigned char ff = 255;
  char *file = g_build_filename(dir, "token", NULL);
  EXPECT(write_file(file, &ff, 1, 0600));
  r = invoke(fixture_bin, argv, NULL, 0, extra, unset, 5000);
  EXPECT(r.status == 1);
  proc_clear(&r);
  g_free(file);
  g_free(bad);
  g_free(home);
  rm_rf(dir);
  g_free(dir);
  end_case();
}

static void test_cached(void) {
  begin("cached assets and required extracted files pass check-only with no token reads or requests");
  Data *d = data_new();
  g_mkdir_with_parents(d->destination, 0700);
  for (int i = 0; i < 4; i++) {
    char *p = g_build_filename(d->destination, d->blobs[i].name, NULL);
    EXPECT(write_file(p, d->blobs[i].data, d->blobs[i].len, 0644));
    g_free(p);
  }
  char *voices = g_build_filename(d->destination, "voices", NULL);
  char *dist = g_build_filename(d->destination, "dist", NULL);
  g_mkdir_with_parents(voices, 0700);
  g_mkdir_with_parents(dist, 0700);
  char *voice = g_build_filename(voices, "NATF2.pt", NULL);
  char *html = g_build_filename(dist, "index.html", NULL);
  EXPECT(write_file(voice, "voice", 5, 0644));
  EXPECT(write_file(html, "html", 4, 0644));
  Server *s = server_new(H_SERVE, NULL, NULL);
  EXPECT(s);
  server_blobs(s, d);
  char *absent = g_build_filename(d->dir, "does-not-exist", NULL);
  char *home = env_join("HF_HOME", absent);
  const char *extra[] = {home, NULL};
  const char *unset[] = {"HF_TOKEN", NULL};
  Proc r = install_run(d, s->url, true, NULL, extra, unset);
  EXPECT(r.status == 0);
  EXPECT(s->nreqs == 0);
  GString *want = g_string_new(NULL);
  for (int i = 0; i < 4; i++) {
    if (i)
      g_string_append_c(want, '\n');
    g_string_append_printf(want, "Verified %s", d->blobs[i].name);
  }
  char *trim = g_strchomp(g_strdup(r.out));
  EXPECT(trim && !strcmp(trim, want->str));
  g_free(trim);
  g_string_free(want, TRUE);
  unlink(html);
  Proc missing = install_run(d, s->url, true, NULL, NULL, NULL);
  EXPECT(contains(missing.err, missing.err_len, "Missing dist/index.html"));
  proc_clear(&r);
  proc_clear(&missing);
  server_stop(s);
  g_free(absent);
  g_free(home);
  g_free(voices);
  g_free(dist);
  g_free(voice);
  g_free(html);
  data_free(d);
  end_case();
}

static void test_terms(void) {
  begin("the terms/access gate is reached only when a download is needed");
  Data *d = data_new();
  Server *s = server_new(H_SERVE, NULL, NULL);
  server_blobs(s, d);
  char *absent = g_build_filename(d->dir, "no-token", NULL);
  char *home = env_join("HF_HOME", absent);
  const char *extra[] = {home, NULL};
  const char *unset[] = {"HF_TOKEN", NULL};
  Proc r = install_run(d, s->url, false, NULL, extra, unset);
  EXPECT(r.status == 1);
  EXPECT(contains(r.err, r.err_len, "Accept the PersonaPlex model terms"));
  EXPECT(s->nreqs == 0);
  EXPECT(!has_partial(d->destination));
  proc_clear(&r);
  server_stop(s);
  g_free(absent);
  g_free(home);
  data_free(d);
  end_case();
}

static void test_install(void) {
  begin("valid TLS downloads install atomically, extract both archives and repeat offline without credentials");
  Data *d = data_new();
  Server *s = server_new(H_SERVE, NULL, NULL);
  server_blobs(s, d);
  Proc r = install_run(d, s->url, false, NULL, NULL, NULL);
  EXPECT(r.status == 0);
  EXPECT(s->nreqs == 4);
  char *bearer = g_strconcat("Bearer ", token, NULL);
  for (size_t i = 0; i < s->nreqs; i++)
    EXPECT(auth_is(&s->reqs[i], bearer));
  char *model = g_build_filename(d->destination, "model.safetensors", NULL);
  struct stat st;
  EXPECT(stat(model, &st) == 0 && (st.st_mode & 0777) == 0600);
  char *voice = g_build_filename(d->destination, "voices/NATF2.pt", NULL);
  char *html = g_build_filename(d->destination, "dist/index.html", NULL);
  char *data = NULL;
  size_t n = 0;
  EXPECT(read_file(voice, &data, &n) && n == 10 && !memcmp(data, "voice-data", 10));
  g_free(data);
  EXPECT(read_file(html, &data, &n) && n == strlen("<head>test</head>") &&
         !memcmp(data, "<head>test</head>", n));
  g_free(data);
  EXPECT(!has_partial(d->destination));
  char *absent = g_build_filename(d->dir, "no-token", NULL);
  char *home = env_join("HF_HOME", absent);
  const char *extra[] = {home, NULL};
  const char *unset[] = {"HF_TOKEN", NULL};
  Proc again = install_run(d, s->url, false, NULL, extra, unset);
  EXPECT(again.status == 0);
  EXPECT(s->nreqs == 4);
  proc_clear(&r);
  proc_clear(&again);
  server_stop(s);
  g_free(bearer);
  g_free(model);
  g_free(voice);
  g_free(html);
  g_free(absent);
  g_free(home);
  data_free(d);
  end_case();
}

static void test_failures(void) {
  begin("HTTP errors, oversize data and bad hashes fail once and leave previous files untouched");
  int kinds[] = {H_403, H_OVERSIZE, H_HASH};
  const char *labels[] = {"http", "oversize", "hash"};
  for (size_t k = 0; k < 3; k++) {
    Data *d = data_new();
    g_mkdir_with_parents(d->destination, 0700);
    char *model = g_build_filename(d->destination, "model.safetensors", NULL);
    EXPECT(write_file(model, "old", 3, 0644));
    Server *s = server_new(kinds[k], NULL, NULL);
    server_blobs(s, d);
    Proc r = install_run(d, s->url, false, NULL, NULL, NULL);
    EXPECT(r.status == 1);
    EXPECT(s->nreqs == 1);
    char *data = NULL;
    size_t n = 0;
    EXPECT(read_file(model, &data, &n) && n == 3 && !memcmp(data, "old", 3));
    g_free(data);
    EXPECT(!has_partial(d->destination));
    EXPECT(!contains(r.err, r.err_len, token));
    if (kinds[k] == H_403)
      EXPECT(contains(r.err, r.err_len, "HTTP 403"));
    (void)labels;
    proc_clear(&r);
    server_stop(s);
    g_free(model);
    data_free(d);
  }
  end_case();
}

static void test_framing(void) {
  begin("pinned bytes override overstated Content-Length but incomplete chunk framing still fails");
  int kinds[] = {H_LENGTH, H_CHUNKED};
  for (size_t k = 0; k < 2; k++) {
    Data *d = data_new();
    Server *s = server_new(kinds[k], NULL, NULL);
    server_blobs(s, d);
    Proc r = install_run(d, s->url, false, NULL, NULL, NULL);
    if (r.status != (kinds[k] == H_LENGTH ? 0 : 1))
      fprintf(stderr, "framing %d status %d err %s reqs %zu\n", kinds[k], r.status,
              r.err ? r.err : "", s->nreqs);
    EXPECT(r.status == (kinds[k] == H_LENGTH ? 0 : 1));
    EXPECT(s->nreqs == (kinds[k] == H_LENGTH ? 4 : 1));
    EXPECT(!has_partial(d->destination));
    proc_clear(&r);
    server_stop(s);
    data_free(d);
  }
  end_case();
}

static void test_same_redirect(void) {
  begin("same-authority HTTPS redirects keep credentials and ignore intermediate bodies");
  Data *d = data_new();
  Server *s = server_new(H_REDIRECT_SAME, NULL, NULL);
  server_blobs(s, d);
  Proc r = install_run(d, s->url, false, NULL, NULL, NULL);
  EXPECT(r.status == 0);
  char *bearer = g_strconcat("Bearer ", token, NULL);
  for (size_t i = 0; i < s->nreqs; i++)
    EXPECT(auth_is(&s->reqs[i], bearer));
  g_free(bearer);
  proc_clear(&r);
  server_stop(s);
  data_free(d);
  end_case();
}

static void test_cross_redirect(void) {
  begin("cross-authority redirects drop credentials permanently even when returning to the original host");
  Data *d = data_new();
  Server *other = server_new(H_REDIRECT_OTHER, NULL, NULL);
  Server *orig = server_new(H_REDIRECT_ORIG, NULL, NULL);
  other->peer = g_strdup(orig->url);
  orig->peer = g_strdup(other->url);
  server_blobs(orig, d);
  Proc r = install_run(d, orig->url, false, NULL, NULL, NULL);
  EXPECT(r.status == 0);
  for (size_t i = 0; i < other->nreqs; i++)
    EXPECT(auth_is(&other->reqs[i], NULL));
  for (size_t i = 0; i < orig->nreqs; i++)
    if (g_str_has_prefix(orig->reqs[i].path, "/final/"))
      EXPECT(auth_is(&orig->reqs[i], NULL));
  proc_clear(&r);
  server_stop(other);
  server_stop(orig);
  data_free(d);
  end_case();
}

static void test_case_redirect(void) {
  begin("authority spelling changes also drop credentials, matching the original netloc comparison");
  Data *d = data_new();
  Server *s = server_new(H_REDIRECT_CASE, NULL, NULL);
  server_blobs(s, d);
  char *base = g_strdup(s->url);
  char *ip = strstr(base, "127.0.0.1");
  EXPECT(ip);
  if (ip)
    memcpy(ip, "localhost", 9);
  Proc r = install_run(d, base, false, NULL, NULL, NULL);
  EXPECT(r.status == 0);
  for (size_t i = 0; i < s->nreqs; i++)
    if (g_str_has_prefix(s->reqs[i].path, "/final/"))
      EXPECT(auth_is(&s->reqs[i], NULL));
  proc_clear(&r);
  server_stop(s);
  g_free(base);
  data_free(d);
  end_case();
}

static void test_idle(void) {
  begin("progressing TLS streams can exceed the idle deadline without a whole-download timeout");
  Data *d = data_new();
  Server *s = server_new(H_SLOW, NULL, NULL);
  server_blobs(s, d);
  int64_t start = now_ms();
  Proc r = install_run(d, s->url, false, "300", NULL, NULL);
  EXPECT(r.status == 0);
  EXPECT(now_ms() - start > 600);
  proc_clear(&r);
  server_stop(s);
  data_free(d);
  end_case();
}

static void test_bad_redirects(void) {
  begin("non-HTTPS redirects and redirect loops fail without leaking credentials");
  const char *tos[] = {"http://127.0.0.1:1/model", "file:///tmp/model", "/model.safetensors"};
  for (size_t i = 0; i < 3; i++) {
    Data *d = data_new();
    Server *s = server_new(H_REDIRECT_TO, NULL, NULL);
    s->location = g_strdup(tos[i]);
    server_blobs(s, d);
    Proc r = install_run(d, s->url, false, NULL, NULL, NULL);
    EXPECT(r.status == 1);
    EXPECT(!contains(r.err, r.err_len, token));
    EXPECT(s->nreqs <= 5);
    EXPECT(!has_partial(d->destination));
    if (g_str_has_prefix(tos[i], "http:"))
      EXPECT(contains(r.err, r.err_len, "non-HTTPS"));
    proc_clear(&r);
    server_stop(s);
    data_free(d);
  }
  end_case();
}

static void test_stall_trust(void) {
  begin("TLS trust failures and stalled sockets do not retry or leave partial files");
  Data *d = data_new();
  Server *s = server_new(H_STALL, NULL, NULL);
  server_blobs(s, d);
  Proc stalled = install_run(d, s->url, false, "250", NULL, NULL);
  EXPECT(stalled.status == 1);
  EXPECT(s->nreqs == 1);
  EXPECT(!has_partial(d->destination));
  char *missing = g_build_filename(d->dir, "missing.pem", NULL);
  char *ssl = env_join("SSL_CERT_FILE", missing);
  const char *extra[] = {ssl, NULL};
  Proc bad = install_run(d, s->url, false, NULL, extra, NULL);
  EXPECT(bad.status == 1);
  EXPECT(s->nreqs == 1);
  EXPECT(!has_partial(d->destination));
  proc_clear(&stalled);
  proc_clear(&bad);
  server_stop(s);
  g_free(missing);
  g_free(ssl);
  data_free(d);
  end_case();
}

static void test_wrong_host(void) {
  begin("a trusted certificate for the wrong hostname is rejected before sending credentials");
  Data *d = data_new();
  char *k = g_build_filename(d->dir, "wrong-key.pem", NULL);
  char *c = g_build_filename(d->dir, "wrong-cert.pem", NULL);
  EXPECT(make_cert(k, c, "/CN=wrong.example", "subjectAltName=DNS:wrong.example"));
  Server *s = server_new(H_NEVER, k, c);
  EXPECT(s);
  char *ssl = env_join("SSL_CERT_FILE", c);
  const char *extra[] = {ssl, NULL};
  Proc r = install_run(d, s->url, false, NULL, extra, NULL);
  EXPECT(r.status == 1);
  EXPECT(s->nreqs == 0);
  EXPECT(!has_partial(d->destination));
  proc_clear(&r);
  server_stop(s);
  g_free(ssl);
  g_free(k);
  g_free(c);
  data_free(d);
  end_case();
}

static Proc extract_run(const char *dir, const char *name) {
  char *argv[] = {fixture_bin, "extract", (char *)dir, (char *)name, NULL};
  return invoke(fixture_bin, argv, NULL, 0, NULL, NULL, 10000);
}

static void test_safe_tar(void) {
  begin("safe tar entries preserve data-filter modes and timestamps and allow internal directory aliases");
  char *dir = temp_dir("personaplex-tools-XXXXXX");
  char *real = g_build_filename(dir, "real", NULL);
  g_mkdir_with_parents(real, 0700);
  char *link = g_build_filename(dir, "voices", NULL);
  EXPECT(symlink("real", link) == 0);
  const char names[][100] = {"voices", "voices/./NATF2.pt", "voices/nonexec"};
  char types[] = {'5', '0', '0'};
  unsigned modes[] = {0, 07777, 011};
  const char *links[] = {NULL, NULL, NULL};
  const char *datas[] = {NULL, "voice", "data"};
  size_t n = 0;
  unsigned char *gz = tar_gzip(names, types, modes, links, datas, 3, &n);
  char *archive = g_build_filename(dir, "voices.tgz", NULL);
  EXPECT(write_file(archive, gz, n, 0644));
  Proc r = extract_run(dir, "voices");
  EXPECT(r.status == 0);
  char buf[64];
  ssize_t ln = readlink(link, buf, sizeof buf - 1);
  EXPECT(ln == 4 && !memcmp(buf, "real", 4));
  char *voice = g_build_filename(real, "NATF2.pt", NULL);
  char *non = g_build_filename(real, "nonexec", NULL);
  char *data = NULL;
  size_t len = 0;
  EXPECT(read_file(voice, &data, &len) && len == 5 && !memcmp(data, "voice", 5));
  g_free(data);
  struct stat st;
  EXPECT(stat(voice, &st) == 0 && (st.st_mode & 07777) == 0755);
  EXPECT(stat(non, &st) == 0 && (st.st_mode & 0777) == 0600);
  EXPECT(stat(real, &st) == 0);
  EXPECT((int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000 == 1234567890000);
  proc_clear(&r);
  g_free(gz);
  g_free(archive);
  g_free(voice);
  g_free(non);
  g_free(link);
  g_free(real);
  rm_rf(dir);
  g_free(dir);
  end_case();
}

static void test_unsafe_members(void) {
  begin("all archive members are validated before extraction, rejecting traversal, wrong roots and special/link types");
  const char names[][100] = {"../escape", "voices/../../escape", "/voices/absolute", "other/file",
                             "voices/link", "voices/hard", "voices/fifo"};
  char types[] = {'0', '0', '0', '0', '2', '1', '6'};
  const char *links[] = {NULL, NULL, NULL, NULL, "/tmp/escape", "voices/NATF2.pt", NULL};
  for (size_t i = 0; i < 7; i++) {
    char *dir = temp_dir("personaplex-tools-XXXXXX");
    const char pair[][100] = {"voices/NATF2.pt", ""};
    memcpy((char *)pair[1], names[i], 100);
    char ty[] = {'0', types[i]};
    unsigned modes[] = {0644, 0644};
    const char *ls[] = {NULL, links[i]};
    const char *ds[] = {"would be written", "x"};
    size_t n = 0;
    unsigned char *gz = tar_gzip(pair, ty, modes, ls, ds, 2, &n);
    char *archive = g_build_filename(dir, "voices.tgz", NULL);
    EXPECT(write_file(archive, gz, n, 0644));
    Proc r = extract_run(dir, "voices");
    EXPECT(r.status == 1);
    EXPECT(contains(r.err, r.err_len, "Unsafe member"));
    char *written = g_build_filename(dir, "voices/NATF2.pt", NULL);
    EXPECT(!g_file_test(written, G_FILE_TEST_EXISTS));
    proc_clear(&r);
    g_free(gz);
    g_free(archive);
    g_free(written);
    rm_rf(dir);
    g_free(dir);
  }
  end_case();
}

static void test_escape_links(void) {
  begin("pre-existing file or directory symlinks escaping the root are rejected");
  for (int directory = 0; directory < 2; directory++) {
    char *dir = temp_dir("personaplex-tools-XXXXXX");
    char *outside = temp_dir("personaplex-tools-XXXXXX");
    char *voices = g_build_filename(dir, "voices", NULL);
    g_mkdir_with_parents(voices, 0700);
    if (directory) {
      rmdir(voices);
      EXPECT(symlink(outside, voices) == 0);
    } else {
      char *value = g_build_filename(outside, "value", NULL);
      EXPECT(write_file(value, "outside", 7, 0644));
      char *link = g_build_filename(voices, "NATF2.pt", NULL);
      EXPECT(symlink(value, link) == 0);
      g_free(value);
      g_free(link);
    }
    const char names[][100] = {"voices/NATF2.pt"};
    char types[] = {'0'};
    unsigned modes[] = {0644};
    const char *ls[] = {NULL};
    const char *ds[] = {"bad"};
    size_t n = 0;
    unsigned char *gz = tar_gzip(names, types, modes, ls, ds, 1, &n);
    char *archive = g_build_filename(dir, "voices.tgz", NULL);
    EXPECT(write_file(archive, gz, n, 0644));
    Proc r = extract_run(dir, "voices");
    EXPECT(r.status == 1);
    EXPECT(contains(r.err, r.err_len, "Unsafe member"));
    if (!directory) {
      char *value = g_build_filename(outside, "value", NULL);
      char *data = NULL;
      size_t len = 0;
      EXPECT(read_file(value, &data, &len) && len == 7 && !memcmp(data, "outside", 7));
      g_free(data);
      g_free(value);
    }
    proc_clear(&r);
    g_free(gz);
    g_free(archive);
    g_free(voices);
    rm_rf(dir);
    rm_rf(outside);
    g_free(dir);
    g_free(outside);
  }
  end_case();
}

static void test_per_entry(void) {
  begin("data-filter confinement is applied per entry after full name/type validation");
  char *dir = temp_dir("personaplex-tools-XXXXXX");
  char *outside = temp_dir("personaplex-tools-XXXXXX");
  char *voices = g_build_filename(dir, "voices", NULL);
  g_mkdir_with_parents(voices, 0700);
  char *value = g_build_filename(outside, "value", NULL);
  EXPECT(write_file(value, "outside", 7, 0644));
  char *escape = g_build_filename(voices, "escape", NULL);
  EXPECT(symlink(value, escape) == 0);
  const char names[][100] = {"voices/first", "voices/escape"};
  char types[] = {'0', '0'};
  unsigned modes[] = {0644, 0644};
  const char *ls[] = {NULL, NULL};
  const char *ds[] = {"first", "bad"};
  size_t n = 0;
  unsigned char *gz = tar_gzip(names, types, modes, ls, ds, 2, &n);
  char *archive = g_build_filename(dir, "voices.tgz", NULL);
  EXPECT(write_file(archive, gz, n, 0644));
  Proc r = extract_run(dir, "voices");
  EXPECT(r.status == 1);
  char *first = g_build_filename(voices, "first", NULL);
  char *data = NULL;
  size_t len = 0;
  EXPECT(read_file(first, &data, &len) && len == 5 && !memcmp(data, "first", 5));
  g_free(data);
  EXPECT(read_file(value, &data, &len) && len == 7 && !memcmp(data, "outside", 7));
  g_free(data);
  proc_clear(&r);
  g_free(gz);
  g_free(archive);
  g_free(first);
  g_free(escape);
  g_free(value);
  g_free(voices);
  rm_rf(dir);
  rm_rf(outside);
  g_free(dir);
  g_free(outside);
  end_case();
}

static void test_patch_guards(void) {
  begin("server patch guards origin, voice allowlist, prompt length and busy state before websocket creation");
  char *input = g_strconcat(handler, "\nint(request[\"seed\"])\nint(request[\"seed\"])", NULL);
  char *argv[] = {fixture_bin, "patch", NULL};
  Proc r = invoke(fixture_bin, argv, input, strlen(input), NULL, NULL, 5000);
  EXPECT(r.status == 0);
  const char *need[] = {"http://127.0.0.1:8998", "http://localhost:8998",
                        "if voice not in allowed_voices:", "> 8000", "if self.lock.locked():"};
  for (size_t i = 0; i < G_N_ELEMENTS(need); i++)
    EXPECT(contains(r.out, r.out_len, need[i]));
  char *lock = r.out ? strstr(r.out, "if self.lock.locked():") : NULL;
  char *ws = r.out ? strstr(r.out, "ws = web.WebSocketResponse()") : NULL;
  EXPECT(lock && ws && lock < ws);
  EXPECT(count_substr(r.out, r.out_len, "int(request.query[\"seed\"])") == 2);
  proc_clear(&r);
  g_free(input);
  end_case();
}

static void test_patch_drift(void) {
  begin("server/root drift and duplicates fail closed without clobbering the source file");
  char *sources[4];
  sources[0] = g_strdup("changed");
  sources[1] = g_strconcat(handler, handler, NULL);
  sources[2] = g_strdup(handler);
  sources[3] = g_strconcat(handler, "\n", web_root, "\n", web_root, NULL);
  for (size_t i = 0; i < 4; i++) {
    char *dir = temp_dir("personaplex-tools-XXXXXX");
    char *file = g_build_filename(dir, "server.py", NULL);
    EXPECT(write_file(file, sources[i], strlen(sources[i]), 0644));
    char *argv[] = {patch_bin, file, "/guard.js", NULL};
    Proc r = invoke(patch_bin, argv, NULL, 0, NULL, NULL, 5000);
    EXPECT(r.status == 1);
    char *data = NULL;
    size_t n = 0;
    EXPECT(read_file(file, &data, &n));
    EXPECT(n == strlen(sources[i]) && !memcmp(data, sources[i], n));
    EXPECT(contains(r.err, r.err_len, "changed"));
    g_free(data);
    proc_clear(&r);
    g_free(file);
    rm_rf(dir);
    g_free(dir);
    g_free(sources[i]);
  }
  end_case();
}

static void test_browser_quote(void) {
  begin("browser injection quotes Unicode, spaces, backslashes and quote characters, preserving source symlinks and modes");
  char *dir = temp_dir("personaplex-tools-XXXXXX");
  char *file = g_build_filename(dir, "server.py", NULL);
  char *link = g_build_filename(dir, "link.py", NULL);
  char *source = g_strconcat(handler, "\n", web_root, NULL);
  EXPECT(write_file(file, source, strlen(source), 0640));
  EXPECT(symlink(file, link) == 0);
  const char *guard = "/path with spaces/雪\\\".js";
  char *argv[] = {patch_bin, link, (char *)guard, NULL};
  Proc r = invoke(patch_bin, argv, NULL, 0, NULL, NULL, 5000);
  EXPECT(r.status == 0);
  char buf[4096];
  ssize_t n = readlink(link, buf, sizeof buf - 1);
  EXPECT(n > 0);
  if (n > 0) {
    buf[n] = 0;
    EXPECT(!strcmp(buf, file));
  }
  struct stat st;
  EXPECT(stat(file, &st) == 0 && (st.st_mode & 0777) == 0640);
  char *out = NULL;
  size_t len = 0;
  EXPECT(read_file(file, &out, &len));
  char *at = out ? strstr(out, "+ Path(") : NULL;
  EXPECT(at);
  if (at) {
    char *q = strchr(at, '"');
    EXPECT(q);
    if (q) {
      size_t i = 1;
      while (q[i] && !(q[i] == '"' && q[i - 1] != '\\'))
        i++;
      yyjson_doc *doc = yyjson_read(q, i + 1, 0);
      yyjson_val *val = doc ? yyjson_doc_get_root(doc) : NULL;
      EXPECT(val && yyjson_is_str(val) && yyjson_get_len(val) == strlen(guard) &&
             !memcmp(yyjson_get_str(val), guard, strlen(guard)));
      yyjson_doc_free(doc);
    }
  }
  EXPECT(out && contains(out, len, "content_type=\"text/html\""));
  g_free(out);
  proc_clear(&r);
  g_free(source);
  g_free(file);
  g_free(link);
  rm_rf(dir);
  g_free(dir);
  end_case();
}

int main(void) {
  umask(022);
  signal(SIGPIPE, SIG_IGN);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  fixture_bin = sibling("PERSONAPLEX_FIXTURE", "personaplex-fixture");
  models_bin = sibling("PERSONAPLEX_MODELS_BIN", "personaplex-models");
  patch_bin = sibling("PERSONAPLEX_PATCH_BIN", "personaplex-patch");
  if (!fixture_bin || !models_bin || !patch_bin || access(fixture_bin, X_OK) ||
      access(models_bin, X_OK) || access(patch_bin, X_OK)) {
    fprintf(stderr, "Set PERSONAPLEX_FIXTURE, PERSONAPLEX_MODELS_BIN and PERSONAPLEX_PATCH_BIN\n");
    return 1;
  }
  if (!load_ssl()) {
    fprintf(stderr, "libssl unavailable\n");
    return 1;
  }
  tls_dir = temp_dir("personaplex-tls-XXXXXX");
  key_path = g_build_filename(tls_dir, "key.pem", NULL);
  cert_path = g_build_filename(tls_dir, "cert.pem", NULL);
  if (!make_cert(key_path, cert_path, "/CN=localhost",
                 "subjectAltName=DNS:localhost,IP:127.0.0.1"))
    return 1;
  test_assets();
  test_cli();
  test_credentials();
  test_whitespace();
  test_bad_credentials();
  test_cached();
  test_terms();
  test_install();
  test_failures();
  test_framing();
  test_same_redirect();
  test_cross_redirect();
  test_case_redirect();
  test_idle();
  test_bad_redirects();
  test_stall_trust();
  test_wrong_host();
  test_safe_tar();
  test_unsafe_members();
  test_escape_links();
  test_per_entry();
  test_patch_guards();
  test_patch_drift();
  test_browser_quote();
  rm_rf(tls_dir);
  g_free(tls_dir);
  g_free(key_path);
  g_free(cert_path);
  g_free(fixture_bin);
  g_free(models_bin);
  g_free(patch_bin);
  curl_global_cleanup();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("ok");
  return 0;
}
