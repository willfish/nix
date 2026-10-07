#define _DEFAULT_SOURCE
#include "models.h"
#include <curl/curl.h>
#include <errno.h>
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  FILE *file;
  GChecksum *digest;
  uint64_t received, expected;
  gint64 activity;
  curl_off_t downloaded, uploaded;
  long idle_ms, status;
  bool oversized, write_failed, location, chunked;
} Transfer;
int matches(const char *path, const Asset *asset) {
  struct stat st;
  if (stat(path, &st) != 0)
    return errno == ENOENT || errno == ENOTDIR || errno == ELOOP ||
                   errno == EBADF
               ? 0
               : -1;
  if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size != asset->bytes)
    return 0;
  FILE *file = fopen(path, "rb");
  if (!file)
    return -1;
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  unsigned char data[65536];
  size_t n;
  while ((n = fread(data, 1, sizeof(data), file)))
    g_checksum_update(sum, data, (gssize)n);
  bool ok = !ferror(file);
  bool valid = ok && !strcmp(g_checksum_get_string(sum), asset->sha256);
  if (fclose(file))
    ok = false;
  g_checksum_free(sum);
  return ok ? (valid ? 1 : 0) : -1;
}
static size_t receive(char *data, size_t size, size_t count, void *raw) {
  Transfer *t = raw;
  if (count && size > SIZE_MAX / count)
    return 0;
  size_t n = size * count;
  t->activity = g_get_monotonic_time();
  if (t->received > t->expected || n > t->expected - t->received) {
    t->oversized = true;
    return 0;
  }
  t->received += n;
  g_checksum_update(t->digest, (const guchar *)data, (gssize)n);
  if (fwrite(data, 1, n, t->file) != n) {
    t->write_failed = true;
    return 0;
  }
  return n;
}
static size_t header(char *data, size_t size, size_t count, void *raw) {
  Transfer *t = raw;
  t->activity = g_get_monotonic_time();
  if (count && size > SIZE_MAX / count)
    return 0;
  size_t n = size * count;
  if (n >= 5 && !memcmp(data, "HTTP/", 5)) {
    char *space = memchr(data, ' ', n);
    if (space && (size_t)(data + n - space) >= 4) {
      t->status =
          (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
      t->location = false;
      t->chunked = false;
    }
  } else if (n >= 9 && !g_ascii_strncasecmp(data, "Location:", 9)) {
    for (size_t i = 9; i < n; i++)
      if (!g_ascii_isspace(data[i]))
        t->location = true;
  }
  if (n >= 18 && !g_ascii_strncasecmp(data, "Transfer-Encoding:", 18)) {
    char *line = g_ascii_strdown(data, (gssize)n);
    t->chunked = strstr(line + 18, "chunked") != NULL;
    g_free(line);
  }
  bool redirect = t->status == 301 || t->status == 302 || t->status == 303 ||
                  t->status == 307 || t->status == 308;
  // urllib raises unhandled HTTP errors at the headers, before reading a body.
  if (n == 2 && !memcmp(data, "\r\n", 2) && t->status >= 300 &&
      (!redirect || !t->location))
    return 0;
  return n;
}
static int progress(void *raw, curl_off_t total_down, curl_off_t down,
                    curl_off_t total_up, curl_off_t up) {
  (void)total_down;
  (void)total_up;
  Transfer *t = raw;
  if (down != t->downloaded || up != t->uploaded)
    t->activity = g_get_monotonic_time();
  t->downloaded = down;
  t->uploaded = up;
  return g_get_monotonic_time() - t->activity > (gint64)t->idle_ms * 1000;
}
static CURLcode download(const Asset *asset, Transfer *t, long *status) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return CURLE_FAILED_INIT;
  curl_easy_setopt(curl, CURLOPT_URL, asset->url);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,ftp");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https,ftp");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Connection: close");
  headers = curl_slist_append(headers, "Accept-Encoding: identity");
  headers = curl_slist_append(headers, "Accept:");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "pi-voice-model-setup");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, t->idle_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  const char *ca = getenv("SSL_CERT_FILE");
  if (ca)
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, t);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, t);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, t);
  CURLcode code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return code;
}
static char *commas(uint64_t value) {
  char plain[32];
  snprintf(plain, sizeof(plain), "%" PRIu64, value);
  size_t len = strlen(plain);
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < len; i++) {
    g_string_append_c(out, plain[i]);
    if (i + 1 < len && (len - i - 1) % 3 == 0)
      g_string_append_c(out, ',');
  }
  return g_string_free(out, false);
}
int install_model(const char *root, const Asset *asset, bool check_only,
                  const DownloadPolicy *policy) {
  char *destination = g_build_filename(root, asset->path, NULL);
  int matched = matches(destination, asset);
  if (matched == 1) {
    printf("Verified %s\n", destination);
    fflush(stdout);
    g_free(destination);
    return 0;
  }
  if (matched == -1) {
    fprintf(stderr, "Could not read model: %s\n", destination);
    g_free(destination);
    return 1;
  }
  if (check_only) {
    fprintf(stderr, "Missing or invalid model: %s\n", destination);
    g_free(destination);
    return 1;
  }
  char *parent = g_path_get_dirname(destination);
  if (g_mkdir_with_parents(parent, 0777)) {
    fprintf(stderr, "Could not create model directory: %s\n", parent);
    g_free(parent);
    g_free(destination);
    return 1;
  }
  char *size = commas(asset->bytes);
  int result = 1;
  for (unsigned attempt = 0; attempt < 3; attempt++) {
    printf("Downloading %s (%s bytes)\n", asset->path, size);
    fflush(stdout);
    char *temporary = g_build_filename(parent, "tmpXXXXXX.partial", NULL);
    int fd = mkstemps(temporary, 8);
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    bool retry = true;
    if (!file) {
      if (fd >= 0)
        close(fd);
      if (attempt == 2)
        fputs("Could not create temporary model file\n", stderr);
    } else {
      Transfer t = {.file = file,
                    .digest = g_checksum_new(G_CHECKSUM_SHA256),
                    .expected = asset->bytes,
                    .activity = g_get_monotonic_time(),
                    .idle_ms = policy->idle_ms};
      long status = 0;
      CURLcode code = download(asset, &t, &status);
      bool truncated_chunks = code == CURLE_PARTIAL_FILE && t.chunked;
      bool complete =
          code == CURLE_OK || (code == CURLE_PARTIAL_FILE && !t.chunked);
      bool integrity = t.received == asset->bytes &&
                       !strcmp(g_checksum_get_string(t.digest), asset->sha256);
      bool synced = fflush(file) == 0 && fsync(fd) == 0;
      bool closed = fclose(file) == 0;
      g_checksum_free(t.digest);
      if (t.oversized) {
        fprintf(stderr, "Download exceeds expected size: %s\n", asset->path);
        retry = false;
      } else if (truncated_chunks) {
        fputs("Model download failed: incomplete chunked response\n", stderr);
        retry = false;
      } else if (complete && !t.write_failed && synced && closed &&
                 !integrity) {
        fprintf(stderr, "Model integrity check failed: %s\n", asset->path);
        retry = false;
      } else if (complete && !t.write_failed && synced && closed && integrity &&
                 rename(temporary, destination) == 0) {
        printf("Installed %s\n", destination);
        fflush(stdout);
        result = 0;
        retry = false;
      } else if (attempt == 2) {
        if (status >= 300)
          fprintf(stderr, "Model download failed (HTTP %ld)\n", status);
        else if (code != CURLE_OK)
          fprintf(stderr, "Model download failed: %s\n",
                  curl_easy_strerror(code));
        else
          fputs("Could not write or install model\n", stderr);
      }
    }
    if (fd >= 0 && unlink(temporary) != 0 && errno != ENOENT) {
      fputs("Could not remove temporary model file\n", stderr);
      result = 1;
      retry = false;
    }
    g_free(temporary);
    if (!retry)
      break;
    if (attempt < 2)
      g_usleep((gulong)(attempt + 1) * policy->backoff_ms * 1000);
  }
  g_free(size);
  g_free(parent);
  g_free(destination);
  return result;
}
