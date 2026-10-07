#define _POSIX_C_SOURCE 200809L
#include "tools.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  FILE *file;
  uint64_t received, expected;
  gint64 activity;
  curl_off_t downloaded, uploaded;
  long idle_ms, status;
  char *location;
  bool oversized, chunked;
} Transfer;
static size_t receive(char *data, size_t size, size_t count, void *raw) {
  Transfer *t = raw;
  if (count && size > SIZE_MAX / count)
    return 0;
  size_t n = size * count;
  t->activity = g_get_monotonic_time();
  if (n > t->expected - t->received) {
    t->oversized = true;
    return 0;
  }
  t->received += n;
  return fwrite(data, 1, n, t->file);
}
static size_t header(char *data, size_t size, size_t count, void *raw) {
  Transfer *t = raw;
  if (count && size > SIZE_MAX / count)
    return 0;
  size_t n = size * count;
  t->activity = g_get_monotonic_time();
  if (n >= 5 && !memcmp(data, "HTTP/", 5)) {
    char *space = memchr(data, ' ', n);
    if (space && (size_t)(data + n - space) >= 4) {
      t->status =
          (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
      g_clear_pointer(&t->location, g_free);
      t->chunked = false;
    }
  } else if (n >= 9 && !g_ascii_strncasecmp(data, "Location:", 9)) {
    char *value = g_strndup(data + 9, n - 9);
    g_strstrip(value);
    g_free(t->location);
    t->location = value;
  } else if (n >= 18 && !g_ascii_strncasecmp(data, "Transfer-Encoding:", 18)) {
    char *line = g_ascii_strdown(data, (gssize)n);
    t->chunked = strstr(line + 18, "chunked") != NULL;
    g_free(line);
  }
  if (n == 2 && !memcmp(data, "\r\n", 2) && t->status >= 300)
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
static char *authority(const char *url) {
  const char *start = strstr(url, "://");
  if (!start)
    return NULL;
  start += 3;
  return g_strndup(start, strcspn(start, "/?#"));
}
bool pp_download(const char *url, const char *token, int fd, const Asset *asset,
                 long idle_ms, char **error) {
  FILE *file = fdopen(fd, "wb");
  if (!file) {
    close(fd);
    return pp_error(error, "Could not write model temporary file");
  }
  char *current = g_strdup(url);
  bool authenticated = true, ok = false;
  GHashTable *visited =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (unsigned hop = 0; hop <= 10; hop++) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      pp_error(error, "Could not initialize model download");
      break;
    }
    Transfer t = {.file = file,
                  .expected = asset->bytes,
                  .activity = g_get_monotonic_time(),
                  .idle_ms = idle_ms};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Connection: close");
    headers = curl_slist_append(headers, "Accept-Encoding: identity");
    headers = curl_slist_append(headers, "Accept:");
    if (authenticated) {
      char *auth = g_strconcat("Authorization: Bearer ", token, NULL);
      headers = curl_slist_append(headers, auth);
      g_free(auth);
    }
    curl_easy_setopt(curl, CURLOPT_URL, current);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Python-urllib/3.13");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    const char *ca = g_getenv("SSL_CERT_FILE");
    if (ca)
      curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, idle_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    CURLcode code = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    bool redirect = t.status == 301 || t.status == 302 || t.status == 303 ||
                    t.status == 307 || t.status == 308;
    if (redirect && t.location) {
      GError *failure = NULL;
      char *next = g_uri_resolve_relative(current, t.location,
                                          G_URI_FLAGS_ENCODED, &failure);
      g_clear_error(&failure);
      GUri *uri = next ? g_uri_parse(next, G_URI_FLAGS_ENCODED, NULL) : NULL;
      if (!uri || g_ascii_strcasecmp(g_uri_get_scheme(uri), "https")) {
        pp_error(error, "Refusing non-HTTPS model redirect");
        g_free(next);
        if (uri)
          g_uri_unref(uri);
        g_free(t.location);
        break;
      }
      g_uri_unref(uri);
      unsigned repeats = GPOINTER_TO_UINT(g_hash_table_lookup(visited, next));
      if (repeats >= 4 || g_hash_table_size(visited) >= 10) {
        pp_error(error,
                 "Model download failed (HTTP %ld); check gated model access",
                 t.status);
        g_free(next);
        g_free(t.location);
        break;
      }
      g_hash_table_replace(visited, g_strdup(next),
                           GUINT_TO_POINTER(repeats + 1));
      char *old_host = authority(current), *new_host = authority(next);
      if (g_strcmp0(old_host, new_host))
        authenticated = false;
      g_free(old_host);
      g_free(new_host);
      g_free(current);
      current = next;
      g_free(t.location);
      continue;
    }
    g_free(t.location);
    if (t.status >= 300)
      pp_error(error,
               "Model download failed (HTTP %ld); check gated model access",
               t.status);
    else if (t.oversized)
      pp_error(error, "Unexpected download size for %s", asset->name);
    else if (code != CURLE_OK && !(code == CURLE_PARTIAL_FILE && !t.chunked))
      pp_error(error, "Model download failed; check the network and retry");
    else
      ok = true;
    break;
  }
  g_hash_table_destroy(visited);
  g_free(current);
  if (fclose(file)) {
    ok = false;
    pp_error(error, "Could not write model temporary file");
  }
  return ok;
}
