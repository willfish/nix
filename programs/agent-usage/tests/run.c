#include "run.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static int failures;
static const char *current = "check";
void check_begin(const char *name) { current = name; }
void check_fail(const char *fmt, ...) {
  failures++;
  fprintf(stderr, "FAIL %s: ", current);
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fputc('\n', stderr);
}
int check_finish(void) {
  if (!failures)
    fprintf(stderr, "ok %s\n", current);
  return failures ? 1 : 0;
}
char *check_exe_dir(void) {
  char raw[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", raw, sizeof raw - 1);
  if (n < 0)
    return NULL;
  raw[n] = 0;
  char *slash = strrchr(raw, '/');
  if (!slash)
    return NULL;
  *slash = 0;
  return strdup(raw);
}
char *check_join(const char *a, const char *b) {
  size_t na = strlen(a), nb = strlen(b);
  int slash = na && a[na - 1] != '/';
  char *out = malloc(na + nb + (slash ? 2 : 1));
  if (!out)
    return NULL;
  memcpy(out, a, na);
  size_t at = na;
  if (slash)
    out[at++] = '/';
  memcpy(out + at, b, nb + 1);
  return out;
}
char *check_temp(const char *prefix) {
  char tmpl[128];
  int wrote = snprintf(tmpl, sizeof tmpl, "/tmp/%sXXXXXX", prefix);
  if (wrote < 0 || (size_t)wrote >= sizeof tmpl)
    return NULL;
  char *path = strdup(tmpl);
  if (!path || !mkdtemp(path)) {
    free(path);
    return NULL;
  }
  char *real = realpath(path, NULL);
  free(path);
  return real;
}
int check_mkdir(const char *path) {
  return mkdir(path, 0777) && errno != EEXIST ? -1 : 0;
}
int check_mkdir_p(const char *path) {
  char *copy = strdup(path);
  if (!copy)
    return -1;
  for (char *p = copy + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    if (check_mkdir(copy)) {
      free(copy);
      return -1;
    }
    *p = '/';
  }
  int rc = check_mkdir(copy);
  free(copy);
  return rc;
}
int check_write(const char *path, const void *data, size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;
  const unsigned char *p = data;
  size_t at = 0;
  while (at < len) {
    ssize_t n = write(fd, p + at, len - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      return -1;
    }
    at += (size_t)n;
  }
  return close(fd);
}
unsigned char *check_read(const char *path, size_t *len) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END)) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET)) {
    fclose(file);
    return NULL;
  }
  unsigned char *data = malloc((size_t)size + 1);
  if (!data) {
    fclose(file);
    return NULL;
  }
  if (size && fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return NULL;
  }
  data[size] = 0;
  if (len)
    *len = (size_t)size;
  fclose(file);
  return data;
}
int check_mode(const char *path) {
  struct stat st;
  if (lstat(path, &st))
    return -1;
  return (int)(st.st_mode & 0777);
}
static void rm_one(const char *path) {
  struct stat st;
  if (lstat(path, &st))
    return;
  if (S_ISDIR(st.st_mode)) {
    chmod(path, 0700);
    DIR *dir = opendir(path);
    if (dir) {
      struct dirent *entry;
      while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
          continue;
        char *child = check_join(path, entry->d_name);
        if (child)
          rm_one(child);
        free(child);
      }
      closedir(dir);
    }
    rmdir(path);
  } else
    unlink(path);
}
void check_rm_rf(const char *path) {
  if (path)
    rm_one(path);
}
static char **merge_env(const char *const *overrides) {
  size_t base = 0, extra = 0;
  while (environ[base])
    base++;
  if (overrides)
    while (overrides[extra])
      extra++;
  char **out = calloc(base + extra + 1, sizeof *out);
  if (!out)
    return NULL;
  for (size_t i = 0; i < base; i++) {
    out[i] = strdup(environ[i]);
    if (!out[i])
      return NULL;
  }
  for (size_t i = 0; i < extra; i++) {
    const char *eq = strchr(overrides[i], '=');
    size_t key = eq ? (size_t)(eq - overrides[i]) : strlen(overrides[i]);
    size_t at = 0;
    for (; at < base; at++)
      if (!strncmp(out[at], overrides[i], key) && out[at][key] == '=')
        break;
    char *copy = strdup(overrides[i]);
    if (!copy)
      return NULL;
    if (at < base) {
      free(out[at]);
      out[at] = copy;
    } else
      out[base++] = copy;
  }
  return out;
}
static int append_bytes(unsigned char **data, size_t *len, size_t *cap, int fd) {
  unsigned char buf[4096];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n < 0 && errno == EINTR)
    return 1;
  if (n <= 0)
    return n < 0 ? -1 : 0;
  if (*len > SIZE_MAX - (size_t)n - 1)
    return -1;
  if (*len + (size_t)n + 1 > *cap) {
    size_t next = *cap ? *cap : 256;
    while (next < *len + (size_t)n + 1) {
      if (next > SIZE_MAX / 2)
        return -1;
      next *= 2;
    }
    unsigned char *grown = realloc(*data, next);
    if (!grown)
      return -1;
    *data = grown;
    *cap = next;
  }
  memcpy(*data + *len, buf, (size_t)n);
  *len += (size_t)n;
  (*data)[*len] = 0;
  return 1;
}
Proc check_run(const char *program, const char *const *args, const void *input,
               size_t input_len, const char *const *env, int timeout_sec) {
  Proc proc = {.status = -1};
  int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
  if (pipe(out_pipe) || pipe(err_pipe) || (input && pipe(in_pipe)))
    return proc;
  pid_t pid = fork();
  if (pid < 0)
    return proc;
  if (!pid) {
    setpgid(0, 0);
    if (input)
      dup2(in_pipe[0], STDIN_FILENO);
    else {
      int null = open("/dev/null", O_RDONLY);
      if (null >= 0)
        dup2(null, STDIN_FILENO);
    }
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    int max = in_pipe[1] > err_pipe[1] ? in_pipe[1] : err_pipe[1];
    for (int fd = 3; fd <= max; fd++)
      close(fd);
    char **merged = merge_env(env);
    if (!merged)
      _exit(127);
    execve(program, (char *const *)args, merged);
    _exit(127);
  }
  setpgid(pid, pid);
  close(out_pipe[1]);
  close(err_pipe[1]);
  if (input) {
    close(in_pipe[0]);
    const unsigned char *p = input;
    size_t at = 0;
    while (at < input_len) {
      ssize_t n = write(in_pipe[1], p + at, input_len - at);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      at += (size_t)n;
    }
    close(in_pipe[1]);
  }
  fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
  size_t out_cap = 0, err_cap = 0;
  int out_open = 1, err_open = 1, status = 0, reaped = 0;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (out_open || err_open || !reaped) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (double)(now.tv_sec - start.tv_sec) +
                     (double)(now.tv_nsec - start.tv_nsec) / 1e9;
    if (elapsed > timeout_sec) {
      kill(-pid, SIGKILL);
      break;
    }
    if (out_open) {
      int rc = 1;
      while (rc > 0)
        rc = append_bytes(&proc.out, &proc.out_len, &out_cap, out_pipe[0]);
      if (rc == 0)
        out_open = 0;
      else if (rc < 0 && errno != EAGAIN)
        out_open = 0;
    }
    if (err_open) {
      int rc = 1;
      while (rc > 0)
        rc = append_bytes(&proc.err, &proc.err_len, &err_cap, err_pipe[0]);
      if (rc == 0)
        err_open = 0;
      else if (rc < 0 && errno != EAGAIN)
        err_open = 0;
    }
    if (!reaped && waitpid(pid, &status, WNOHANG) == pid)
      reaped = 1;
    if (reaped && !out_open && !err_open)
      break;
    struct timespec pause = {.tv_nsec = 20 * 1000 * 1000};
    nanosleep(&pause, NULL);
  }
  close(out_pipe[0]);
  close(err_pipe[0]);
  if (!reaped && waitpid(pid, &status, 0) < 0)
    return proc;
  proc.exited = WIFEXITED(status);
  proc.status = proc.exited ? WEXITSTATUS(status) : -1;
  if (!proc.out) {
    proc.out = calloc(1, 1);
    proc.out_len = 0;
  }
  if (!proc.err) {
    proc.err = calloc(1, 1);
    proc.err_len = 0;
  }
  return proc;
}
void proc_free(Proc *proc) {
  free(proc->out);
  free(proc->err);
  *proc = (Proc){0};
}
bool contains_text(const unsigned char *data, size_t len, const char *text) {
  size_t n = strlen(text);
  if (n > len)
    return false;
  for (size_t i = 0; i + n <= len; i++)
    if (!memcmp(data + i, text, n))
      return true;
  return false;
}
bool ends_with(const unsigned char *data, size_t len, const char *text) {
  size_t n = strlen(text);
  return n <= len && !memcmp(data + len - n, text, n);
}
yyjson_doc *parse_json(const void *data, size_t len) {
  return yyjson_read(data, len, YYJSON_READ_ALLOW_INVALID_UNICODE |
                                    YYJSON_READ_ALLOW_TRAILING_COMMAS);
}
yyjson_val *jget(yyjson_val *value, const char *key) {
  return yyjson_is_obj(value) ? yyjson_obj_get(value, key) : NULL;
}
const char *jstr(yyjson_val *value, size_t *len) {
  if (!yyjson_is_str(value))
    return NULL;
  if (len)
    *len = yyjson_get_len(value);
  return yyjson_get_str(value);
}
static bool number_equal(yyjson_val *a, yyjson_val *b) {
  if (!yyjson_is_num(a) || !yyjson_is_num(b))
    return false;
  double left = yyjson_get_num(a), right = yyjson_get_num(b);
  return left == right;
}
bool json_equal(yyjson_val *a, yyjson_val *b) {
  if (!a || !b)
    return a == b;
  if (yyjson_is_num(a) || yyjson_is_num(b))
    return number_equal(a, b);
  if (yyjson_get_type(a) != yyjson_get_type(b))
    return false;
  if (yyjson_is_str(a))
    return yyjson_get_len(a) == yyjson_get_len(b) &&
           !memcmp(yyjson_get_str(a), yyjson_get_str(b), yyjson_get_len(a));
  if (yyjson_is_arr(a)) {
    if (yyjson_arr_size(a) != yyjson_arr_size(b))
      return false;
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(a, i, n, item) if (!json_equal(item, yyjson_arr_get(b, i)))
      return false;
    return true;
  }
  if (yyjson_is_obj(a)) {
    if (yyjson_obj_size(a) != yyjson_obj_size(b))
      return false;
    size_t i, n;
    yyjson_val *key, *item;
    yyjson_obj_foreach(a, i, n, key, item) {
      yyjson_val *other = yyjson_obj_getn(b, yyjson_get_str(key), yyjson_get_len(key));
      if (!json_equal(item, other))
        return false;
    }
    return true;
  }
  return yyjson_is_true(a) == yyjson_is_true(b) ||
         (yyjson_is_null(a) && yyjson_is_null(b)) ||
         (yyjson_is_false(a) && yyjson_is_false(b));
}
char *env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return strdup(value && *value ? value : fallback);
}
