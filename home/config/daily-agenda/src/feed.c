#define _DEFAULT_SOURCE
#include "agenda.h"
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
bool feed_url(const char *value, char **token) {
  if (!value)
    return bad("invalid calendar url");
  GRegex *regex = g_regex_new("\\Ahttps://calendar\\.google\\.com/calendar/"
                              "ical/((?:[A-Za-z0-9._+-]|%[0-9A-Fa-f]{2})+)/"
                              "private-([A-Za-z0-9_-]{16,256})/basic\\.ics\\z",
                              0, 0, NULL);
  GMatchInfo *match = NULL;
  bool ok = g_regex_match(regex, value, 0, &match);
  if (ok) {
    char *id = g_match_info_fetch(match, 1),
         *decoded = g_uri_unescape_string(id, NULL);
    ok = decoded &&
         g_regex_match_simple("\\A[A-Za-z0-9][A-Za-z0-9._+@#-]*\\z", decoded, 0,
                              0) &&
         !strchr(decoded, '%');
    if (ok && token)
      *token = g_match_info_fetch(match, 2);
    g_free(id);
    g_free(decoded);
  }
  g_match_info_free(match);
  g_regex_unref(regex);
  return ok ? true : bad("invalid calendar url");
}
GPtrArray *feed_secrets(const char *url) {
  char *token = NULL;
  if (!feed_url(url, &token))
    return NULL;
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(out, g_strdup(url));
  g_ptr_array_add(out, token);
  g_ptr_array_add(out, g_strdup(token));
  return out;
}
yyjson_mut_doc *feeds(const char *path) {
  struct stat st;
  if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || (st.st_mode & 077) ||
      st.st_size > 4096) {
    bad("credentials must be private");
    return NULL;
  }
  GBytes *bytes;
  if (!read_bytes(path, 4096, &bytes))
    return NULL;
  gsize len;
  const char *raw = g_bytes_get_data(bytes, &len);
  if (!g_utf8_validate(raw, (gssize)len, NULL) || memchr(raw, 0, len)) {
    g_bytes_unref(bytes);
    bad("invalid credentials");
    return NULL;
  }
  char *all = g_strndup(raw, len), *cursor = all;
  while (g_str_has_prefix(cursor, "\357\273\277"))
    cursor += 3;
  char *s = strip(cursor);
  g_free(all);
  g_bytes_unref(bytes);
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  bool ok = true;
  if (*s != '{') {
    ok = feed_url(s, NULL);
    if (ok)
      yyjson_mut_obj_add_strcpy(doc, root, "William", s);
  } else {
    yyjson_doc *input =
        yyjson_read(s, strlen(s), YYJSON_READ_ALLOW_INF_AND_NAN);
    GPtrArray *names = input && yyjson_is_obj(yyjson_doc_get_root(input))
                           ? keys(yyjson_doc_get_root(input))
                           : NULL;
    ok = names && names->len >= 1 && names->len <= 8;
    if (ok)
      for (size_t i = 0; i < names->len; i++) {
        const char *name = names->pdata[i],
                   *url = text(field(yyjson_doc_get_root(input), name));
        char *trimmed = strip(name), *safe = clean(name, 2000);
        ok = *trimmed && g_utf8_strlen(name, -1) <= 40 && !strcmp(safe, name) &&
             feed_url(url, NULL);
        g_free(trimmed);
        g_free(safe);
        if (!ok)
          break;
        yyjson_mut_obj_add(root, yyjson_mut_strcpy(doc, name),
                           yyjson_mut_strcpy(doc, url));
      }
    if (names)
      g_ptr_array_free(names, true);
    if (input)
      yyjson_doc_free(input);
  }
  g_free(s);
  if (!ok) {
    yyjson_mut_doc_free(doc);
    bad("invalid credentials");
    return NULL;
  }
  return doc;
}
typedef struct {
  GByteArray *body;
  bool header_ok, declared, chunked;
  gint64 progress_at;
  curl_off_t last;
  long status;
} Fetch;
static size_t body(void *data, size_t size, size_t count, void *user) {
  Fetch *f = user;
  size_t n = size * count;
  f->progress_at = g_get_monotonic_time();
  if (n > MAX_BYTES + 1 - f->body->len)
    return 0;
  g_byte_array_append(f->body, data, (guint)n);
  return f->body->len <= MAX_BYTES ? n : 0;
}
static size_t header(void *data, size_t size, size_t count, void *user) {
  Fetch *f = user;
  size_t n = size * count;
  f->progress_at = g_get_monotonic_time();
  char *line = g_strndup(data, n);
  if (g_str_has_prefix(line, "HTTP/")) {
    char *space = strchr(line, ' ');
    f->status = space ? strtol(space + 1, NULL, 10) : 0;
    f->header_ok = true;
    f->declared = f->chunked = false;
  }
  if (!g_ascii_strncasecmp(line, "Transfer-Encoding:", 18)) {
    char *lower = g_ascii_strdown(line + 18, -1);
    f->chunked = strstr(lower, "chunked") != NULL;
    g_free(lower);
  }
  if (!g_ascii_strncasecmp(line, "Content-Length:", 15)) {
    char *s = g_strstrip(line + 15), *end;
    errno = 0;
    long long length = strtoll(s, &end, 10);
    if (!*s || *end || errno || length < 0 || length > MAX_BYTES)
      f->header_ok = false;
    else
      f->declared = true;
  }
  bool stop = (!strcmp(line, "\r\n") || !strcmp(line, "\n")) &&
              (!f->header_ok || f->status != 200);
  g_free(line);
  return stop ? 0 : n;
}
static int progress(void *user, curl_off_t total, curl_off_t now,
                    curl_off_t up_total, curl_off_t up_now) {
  Fetch *f = user;
  (void)total;
  (void)up_total;
  (void)up_now;
  gint64 at = g_get_monotonic_time();
  if (now != f->last) {
    f->progress_at = at;
    f->last = now;
  }
  return at - f->progress_at > FETCH_TIMEOUT * G_TIME_SPAN_SECOND;
}
bool open_feed(const char *url, GBytes **bytes) {
  if (!feed_url(url, NULL))
    return false;
  CURL *curl = curl_easy_init();
  if (!curl)
    return bad("calendar request failed");
  Fetch f = {.body = g_byte_array_new(),
             .header_ok = true,
             .progress_at = g_get_monotonic_time()};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Accept: text/calendar, text/plain");
  headers = curl_slist_append(headers, "Accept-Encoding: identity");
  headers = curl_slist_append(headers, "Connection: close");
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Python-urllib/3.13");
  curl_easy_setopt(curl, CURLOPT_HTTP_CONTENT_DECODING, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)FETCH_TIMEOUT);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &f);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &f);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &f);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  const char *trust = g_getenv("SSL_CERT_FILE");
  if (trust)
    curl_easy_setopt(curl, CURLOPT_CAINFO, trust);
  CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  // urllib's bounded read accepts an EOF-short Content-Length body. Incomplete
  // chunk framing still fails; the child must validate the whole iCalendar.
  bool framed = code == CURLE_OK ||
                (code == CURLE_PARTIAL_FILE && f.declared && !f.chunked);
  bool ok = framed && status == 200 && f.header_ok && f.body->len <= MAX_BYTES;
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (ok)
    *bytes = g_byte_array_free_to_bytes(f.body);
  else
    g_byte_array_unref(f.body);
  return ok ? true : bad("calendar request failed");
}
static int calendar_compare(const void *a, const void *b) {
  yyjson_mut_val *x = *(yyjson_mut_val *const *)a,
                 *y = *(yyjson_mut_val *const *)b;
  yyjson_mut_val *xs = yyjson_mut_obj_get(x, "start"),
                 *ys = yyjson_mut_obj_get(y, "start");
  bool xd = yyjson_mut_obj_get(xs, "date") != NULL,
       yd = yyjson_mut_obj_get(ys, "date") != NULL;
  if (xd != yd)
    return xd ? -1 : 1;
  const char *xx = yyjson_mut_get_str(
                 yyjson_mut_obj_get(xs, xd ? "date" : "dateTime")),
             *yy = yyjson_mut_get_str(
                 yyjson_mut_obj_get(ys, yd ? "date" : "dateTime"));
  if (xd)
    return strcmp(xx, yy);
  GDateTime *one = parse_stamp(xx), *two = parse_stamp(yy);
  int cmp = g_date_time_compare(one, two);
  g_date_time_unref(one);
  g_date_time_unref(two);
  return cmp;
}
yyjson_mut_doc *fetch_calendars(yyjson_mut_doc *fs, GDateTime *day,
                                GTimeZone *tz, const char *name,
                                const char *program) {
  yyjson_doc *input = yyjson_mut_doc_imut_copy(fs, NULL);
  yyjson_val *root = yyjson_doc_get_root(input);
  GPtrArray *names = keys(root), *all = g_ptr_array_new();
  yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
  bool ok = names != NULL;
  for (size_t i = 0; names && i < names->len && ok; i++) {
    const char *calendar = names->pdata[i], *url = text(field(root, calendar));
    GBytes *raw = NULL;
    GDateTime *end = g_date_time_add_days(day, 1);
    yyjson_mut_doc *expanded = open_feed(url, &raw)
                                   ? expand(raw, program, day, end, name, false)
                                   : NULL;
    g_date_time_unref(end);
    if (raw)
      g_bytes_unref(raw);
    GPtrArray *secrets = feed_secrets(url);
    if (!expanded || !secrets) {
      if (expanded)
        yyjson_mut_doc_free(expanded);
      if (secrets)
        g_ptr_array_free(secrets, true);
      ok = false;
      break;
    }
    yyjson_doc *items = yyjson_mut_doc_imut_copy(expanded, NULL);
    yyjson_val *event;
    size_t j, max;
    yyjson_arr_foreach(yyjson_doc_get_root(items), j, max, event) {
      yyjson_mut_val *normalized = normalize(out, event, tz, secrets);
      if (!normalized) {
        ok = false;
        break;
      }
      yyjson_mut_obj_add_strcpy(out, normalized, "calendar", calendar);
      g_ptr_array_add(all, normalized);
      if (all->len > MAX_OCCURRENCES) {
        ok = bad("too many events");
        break;
      }
    }
    yyjson_doc_free(items);
    yyjson_mut_doc_free(expanded);
    g_ptr_array_free(secrets, true);
  }
  if (ok) {
    g_ptr_array_sort(all, calendar_compare);
    yyjson_mut_val *array = yyjson_mut_arr(out);
    yyjson_mut_doc_set_root(out, array);
    for (size_t i = 0; i < all->len; i++)
      yyjson_mut_arr_add_val(array, all->pdata[i]);
  }
  if (names)
    g_ptr_array_free(names, true);
  g_ptr_array_free(all, true);
  yyjson_doc_free(input);
  if (!ok) {
    yyjson_mut_doc_free(out);
    return NULL;
  }
  return out;
}
