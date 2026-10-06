#include "usage.h"
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static bool mkdirs(const char *path) {
  char *s = strdup(path);
  for (char *p = s + 1; *p; p++)
    if (*p == '/') {
      *p = 0;
      if (mkdir(s, 0777) && errno != EEXIST) {
        free(s);
        return false;
      }
      *p = '/';
    }
  bool ok = !mkdir(s, 0777) || errno == EEXIST;
  free(s);
  return ok;
}
bool write_auth(const char *path, Val *auth) {
  char *parent = strdup(path), *slash = strrchr(parent, '/');
  if (slash)
    *slash = 0;
  else {
    free(parent);
    parent = strdup(".");
  }
  if (!mkdirs(parent)) {
    free(parent);
    return false;
  }
  char *temporary = join(parent, ".auth-XXXXXX"), *data = encode(auth);
  free(parent);
  int fd = mkstemp(temporary);
  bool ok = false;
  if (fd >= 0 && data) {
    FILE *f = fdopen(fd, "w");
    if (f) {
      ok = fputs(data, f) >= 0 && fputc('\n', f) != EOF && fflush(f) == 0 &&
           fchmod(fd, 0600) == 0;
      if (fclose(f))
        ok = false;
    } else
      close(fd);
    if (ok && rename(temporary, path))
      ok = false;
  } else if (fd >= 0)
    close(fd);
  if (!ok)
    unlink(temporary);
  free(data);
  free(temporary);
  return ok;
}
static double monotonic_ms(void);
typedef struct {
  char *data;
  size_t len;
  double activity;
  curl_off_t downloaded, uploaded;
  long response_status;
} Buffer;
static int progress(void *raw, curl_off_t total_down, curl_off_t down,
                    curl_off_t total_up, curl_off_t up) {
  (void)total_down;
  (void)total_up;
  Buffer *b = raw;
  if (down != b->downloaded || up != b->uploaded)
    b->activity = monotonic_ms();
  b->downloaded = down;
  b->uploaded = up;
  return monotonic_ms() - b->activity > 15000;
}
static size_t response_header(char *data, size_t size, size_t count,
                              void *raw) {
  (void)data;
  Buffer *b = raw;
  b->activity = monotonic_ms();
  if (count && size > SIZE_MAX / count)
    return 0;
  size_t n = size * count;
  if (n >= 5 && !memcmp(data, "HTTP/", 5)) {
    char *space = memchr(data, ' ', n);
    if (space && (size_t)(data + n - space) >= 4 &&
        isdigit((unsigned char)space[1]) && isdigit((unsigned char)space[2]) &&
        isdigit((unsigned char)space[3]))
      b->response_status =
          (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
  }
  // urllib reports HTTP errors at the headers, without waiting for an error
  // body. Preserve that distinction even if the body would stall or reset.
  if (n == 2 && data[0] == '\r' && data[1] == '\n' && b->response_status >= 300)
    return 0;
  return n;
}
static size_t receive(char *data, size_t size, size_t count, void *raw) {
  Buffer *b = raw;
  if (count && size > SIZE_MAX / count)
    return 0;
  size_t n = size * count;
  if (b->len == SIZE_MAX || n > SIZE_MAX - b->len - 1)
    return 0;
  b->activity = monotonic_ms();
  char *p = realloc(b->data, b->len + n + 1);
  if (!p)
    return 0;
  b->data = p;
  memcpy(p + b->len, data, n);
  b->len += n;
  p[b->len] = 0;
  return n;
}
static bool safe_header(const char *value) {
  for (const unsigned char *p = (const unsigned char *)value; *p; p++)
    if (*p < 32 || *p == 127)
      return false;
  return true;
}
Reply http_request(Doc *d, const char *url, const char *token,
                   const char *extra_headers, const char *form) {
  Reply r = {.failed = true};
  if (token && !safe_header(token))
    return r;
  CURL *curl = curl_easy_init();
  if (!curl)
    return r;
  struct curl_slist *headers = NULL;
  Buffer b = {.activity = monotonic_ms()};
  headers = curl_slist_append(headers, "Accept: application/json");
  if (token) {
    size_t n = strlen(token) + 32;
    char *h = allocate(n);
    snprintf(h, n, "Authorization: Bearer %s", token);
    headers = curl_slist_append(headers, h);
    free(h);
  }
  if (extra_headers && *extra_headers) {
    char *s = strdup(extra_headers), *save = NULL;
    for (char *h = strtok_r(s, "\n", &save); h;
         h = strtok_r(NULL, "\n", &save)) {
      if (!safe_header(h)) {
        free(s);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return r;
      }
      headers = curl_slist_append(headers, h);
    }
    free(s);
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  // Preserve the socket timeout without cutting off a longer response that
  // continues to make progress. Header and body activity restart the timer.
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &b);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, response_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &b);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
  const char *ca = getenv("SSL_CERT_FILE");
  if (ca && *ca)
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
  if (form) {
    headers = curl_slist_append(
        headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form);
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  CURLcode code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
  if (r.status >= 300)
    r.failed = false;
  else if (code == CURLE_OK) {
    r.failed = false;
    if (r.status >= 200 && r.status < 300) {
      r.body = parse_bytes(d, b.data ? b.data : "", b.len);
      if (!yyjson_mut_is_obj(r.body))
        r.failed = true;
    }
  }
  free(b.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return r;
}
const char *oauth_access(Doc *d, Context *ctx, const char *provider,
                         const char *url, const char *client_id,
                         const char **failure) {
  char *dir = agent_dir(), *path = join(dir, "auth.json");
  free(dir);
  Val *auth = load(d, path), *entry = get(auth, provider);
  *failure = "Waiting for auth";
  if (!yyjson_mut_is_obj(entry) ||
      (!truth(get(entry, "refresh")) && !truth(get(entry, "access")))) {
    free(path);
    return "";
  }
  int64_t now = (int64_t)(context_now(ctx) * 1000);
  *failure = "";
  if (token_current(entry, now, 300000)) {
    free(path);
    return text(get(entry, "access"));
  }
  *failure = "Sign-in expired";
  const char *refresh = text(get(entry, "refresh"));
  if (!*refresh) {
    free(path);
    return "";
  }
  CURL *curl = curl_easy_init();
  if (!curl) {
    free(path);
    return "";
  }
  char *escaped = curl_easy_escape(curl, refresh, 0),
       *client = curl_easy_escape(curl, client_id, 0);
  if (!escaped || !client) {
    curl_free(escaped);
    curl_free(client);
    curl_easy_cleanup(curl);
    free(path);
    return "";
  }
  size_t n = strlen(escaped) + strlen(client) + 80;
  char *form = allocate(n);
  snprintf(form, n, "grant_type=refresh_token&refresh_token=%s&client_id=%s",
           escaped, client);
  Reply reply = ctx->transport(d, url, NULL, NULL, form);
  curl_free(escaped);
  curl_free(client);
  curl_easy_cleanup(curl);
  free(form);
  if (reply.failed || reply.status < 200 || reply.status >= 300 ||
      !truth(get(reply.body, "access_token"))) {
    free(path);
    if (token_current(entry, now, 0)) {
      *failure = "";
      return text(get(entry, "access"));
    }
    return "";
  }
  Val *updated = merge_oauth(d, entry, reply.body, now);
  put(d, auth, provider, updated);
  (void)write_auth(path, auth);
  free(path);
  *failure = "";
  return text(get(updated, "access"));
}
double context_now(Context *ctx) {
  if (!ctx->real_time)
    return ctx->now;
  struct timespec t;
  if (clock_gettime(CLOCK_REALTIME, &t))
    return ctx->now;
  return t.tv_sec + t.tv_nsec / 1000000000.0;
}
static double monotonic_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
char *command_key(const char *raw, int timeout_ms) {
  char *value = strip_text(strdup(raw));
  if (*value != '!')
    return value;
  int pipes[2];
  if (pipe(pipes)) {
    free(value);
    return strdup("");
  }
  pid_t pid = fork();
  if (pid == 0) {
    setpgid(0, 0);
    close(pipes[0]);
    dup2(pipes[1], STDOUT_FILENO);
    close(pipes[1]);
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDERR_FILENO);
      close(null);
    }
    execl("/bin/sh", "sh", "-c", value + 1, (char *)NULL);
    _exit(127);
  }
  free(value);
  close(pipes[1]);
  if (pid < 0) {
    close(pipes[0]);
    return strdup("");
  }
  setpgid(pid, pid);
  fcntl(pipes[0], F_SETFL, O_NONBLOCK);
  Buffer b = {0};
  double deadline = monotonic_ms() + timeout_ms;
  int status = 0;
  bool done = false, eof = false, failed = false;
  while (!done || !eof) {
    char data[4096];
    ssize_t n;
    while ((n = read(pipes[0], data, sizeof data)) > 0) {
      if (!receive(data, 1, (size_t)n, &b)) {
        failed = true;
        break;
      }
    }
    if (n == 0)
      eof = true;
    if (!done) {
      pid_t result = waitpid(pid, &status, WNOHANG);
      if (result == pid)
        done = true;
      else if (result < 0 && errno != EINTR) {
        done = true;
        failed = true;
      }
    }
    if (done && eof)
      break;
    if (failed || monotonic_ms() >= deadline) {
      failed = true;
      break;
    }
    struct pollfd pollfd = {.fd = pipes[0], .events = POLLIN | POLLHUP};
    poll(&pollfd, 1, 20);
  }
  if (failed) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
  }
  if (!done)
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  close(pipes[0]);
  if (failed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    free(b.data);
    return strdup("");
  }
  return strip_text(b.data ? b.data : strdup(""));
}
char *opencode_key(Doc *d) {
  const char *env = getenv("OPENCODE_GO_API_KEY");
  if (env && *env)
    return strdup(env);
  char *dir = agent_dir(), *path = join(dir, "models.json");
  Val *models = load(d, path),
      *entry = get(get(models, "providers"), "opencode-go");
  free(path);
  Val *raw = get(entry, "apiKey");
  if (!truth(raw))
    raw = get(entry, "key");
  if (!yyjson_mut_is_str(raw) || !truth(raw)) {
    path = join(dir, "auth.json");
    Val *auth = load(d, path), *stored = get(auth, "opencode-go");
    free(path);
    raw = yyjson_mut_is_str(stored) ? stored : get(stored, "key");
    if (!truth(raw))
      raw = get(stored, "access");
  }
  free(dir);
  return command_key(text(raw), 20000);
}
Val *jwt_claims(Doc *d, const char *token) {
  const char *start = strchr(token, '.');
  if (!start)
    return yyjson_mut_obj(d);
  start++;
  const char *end = strchr(start, '.');
  if (!end)
    end = start + strlen(start);
  char *decoded = allocate((size_t)(end - start) + 1);
  size_t n = 0;
  unsigned acc = 0, bits = 0;
  bool valid = true;
  for (const char *p = start; p < end && *p != '='; p++) {
    const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char ch = *p == '+' ? '-' : *p == '/' ? '_' : *p;
    const char *c = strchr(alphabet, ch);
    if (!c) {
      valid = false;
      break;
    }
    acc = (acc << 6) | (unsigned)(c - alphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded[n++] = (char)((acc >> bits) & 255);
    }
  }
  Val *claims = valid ? parse_bytes(d, decoded, n) : NULL;
  free(decoded);
  return yyjson_mut_is_obj(claims) ? claims : yyjson_mut_obj(d);
}
static bool header_value(const char *value) { return safe_header(value); }
Val *collect(Doc *d, Context *ctx, const char *id) {
  bool codex = !strcmp(id, "codex"), grok = !strcmp(id, "grok");
  const char *name = codex ? "Codex" : grok ? "Grok" : "OpenCode";
  const char *provider = codex ? "openai-codex" : grok ? "xai" : "opencode-go";
  const char *auth_help =
      codex  ? "Run /login openai-codex in Pi."
      : grok ? "Run /login xai in Pi."
             : "Connect OpenCode Go in Pi (`/login opencode-go`).";
  const char *raw = getenv("PI_AGENT_DIR");
  char *root = (raw && *raw) ? strdup(raw) : join(home_dir(), ".pi/agent"),
       *sessions = join(root, "sessions");
  Val *stats = scan_pi(d, sessions, provider, context_now(ctx));
  free(sessions);
  const char *failure = "";
  char *key = NULL;
  const char *token;
  if (codex || grok)
    token = oauth_access(d, ctx, provider,
                         codex ? "https://auth.openai.com/oauth/token"
                               : "https://auth.x.ai/oauth2/token",
                         codex ? "app_EMoamEEZ73f0CkXaXp7hrann"
                               : "b1a00492-073a-47ea-816f-4c329264a828",
                         &failure);
  else {
    key = opencode_key(d);
    token = key;
    if (!*key)
      failure = "Waiting for auth";
  }
  Val *claims = grok ? jwt_claims(d, token) : NULL;
  Allowance a = {.limits = yyjson_mut_arr(d),
                 .tier = codex  ? ""
                         : grok ? (*token ? grok_tier(d, claims) : "")
                                : "Go",
                 .status = failure};
  const char *help = *failure ? auth_help : "";
  bool retry = false;
  if (*token) {
    char *extra = NULL;
    bool valid = true;
    if (codex) {
      char *path = join(root, "auth.json");
      Val *auth = load(d, path);
      free(path);
      const char *account = text(get(get(auth, "openai-codex"), "accountId"));
      valid = header_value(account);
      if (*account) {
        extra = allocate(strlen(account) + 32);
        sprintf(extra, "ChatGPT-Account-Id: %s", account);
      }
    } else if (grok) {
      Val *user = get(claims, "principal_id");
      if (!truth(user))
        user = get(claims, "sub");
      const char *uid = text(user);
      valid = header_value(uid);
      extra = allocate(strlen(uid) + 80);
      sprintf(extra, "x-xai-token-auth: xai-grok-cli\nx-userid: %s", uid);
    } else
      extra = strdup("User-Agent: hypr-agent-usage");
    Reply reply = valid
                      ? ctx->transport(
                            d,
                            codex ? "https://chatgpt.com/backend-api/wham/usage"
                            : grok ? "https://cli-chat-proxy.grok.com/v1/"
                                     "billing?format=credits"
                                   : "https://opencode.ai/zen/go/v1/usage",
                            token, extra, NULL)
                      : (Reply){.failed = true};
    free(extra);
    a.status = codex  ? "Codex limits unavailable"
               : grok ? "Grok limits unavailable"
                      : "OpenCode limits unavailable";
    if (reply.failed) {
      help =
          codex ? "Couldn't reach Codex usage. Local Pi stats are still shown."
          : grok
              ? "Couldn't reach Grok billing. Local Pi stats are still shown."
              : "Couldn't reach OpenCode Go usage.";
      retry = true;
    } else if (reply.status < 200 || reply.status >= 300) {
      char note[128];
      snprintf(note, sizeof note,
               codex  ? "Codex usage returned status %ld."
               : grok ? "Grok billing returned status %ld."
                      : "OpenCode Go usage returned status %ld.",
               reply.status);
      help = yyjson_mut_get_str(yyjson_mut_strcpy(d, note));
      if (!codex && !grok && reply.status == 403)
        help = "This OpenCode key has no Go subscription.";
      if (!codex && !grok && reply.status == 401)
        help = auth_help;
    } else {
      if (codex)
        a = parse_codex(d, reply.body, context_now(ctx));
      else if (grok) {
        a.limits = parse_grok(d, reply.body);
        a.status = "";
      } else
        a = parse_opencode(d, reply.body);
      if (!codex && !grok && !*a.tier)
        a.tier = "Go";
      help = "";
      if (!yyjson_mut_arr_size(a.limits) && !*a.status) {
        a.status = codex  ? "Codex limits unavailable"
                   : grok ? "Grok limits unavailable"
                          : "OpenCode limits unavailable";
        help = codex  ? "Codex usage returned no allowance."
               : grok ? "Grok billing returned no allowance."
                      : "OpenCode Go usage returned no allowance.";
      }
    }
  }
  Val *out = record(d, id, name, stats, a, help, retry, context_now(ctx));
  free(key);
  free(root);
  return out;
}
