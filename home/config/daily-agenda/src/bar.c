#include "agenda.h"
#include <stdlib.h>
#include <string.h>
static char *public_https(const char *value, GPtrArray *secrets) {
  char *link = redact(value, secrets, 2000);
  bool ok = g_str_has_prefix(link, "https://") &&
            g_utf8_strlen(link, -1) <= 500 && !strpbrk(link, "\"'<>");
  for (const char *p = link; ok && *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isspace(c) || c == 0x85 || (c >= 0x1c && c <= 0x1f))
      ok = false;
  }
  GUri *uri = ok ? g_uri_parse(link, G_URI_FLAGS_PARSE_RELAXED, NULL) : NULL;
  ok = uri && !strcmp(g_uri_get_scheme(uri), "https") && g_uri_get_host(uri) &&
       *g_uri_get_host(uri) && !g_uri_get_userinfo(uri) &&
       (g_uri_get_port(uri) == -1 || g_uri_get_port(uri) == 443);
  if (uri)
    g_uri_unref(uri);
  if (!ok)
    *link = 0;
  return link;
}
static char *meeting(yyjson_val *record, GPtrArray *secrets) {
  const char *fields[] = {"location", "description", "url"};
  GRegex *urls = g_regex_new("(*UCP)https://[^\\s<>'\"]+", 0, 0, NULL),
         *hosts = g_regex_new("\\A(?:meet\\.google\\.com|teams\\.microsoft\\."
                              "com|(?:[a-z0-9-]+\\.)?zoom\\.us)\\z",
                              G_REGEX_CASELESS, 0, NULL);
  char *out = g_strdup("");
  for (size_t i = 0; i < 3 && !*out; i++) {
    const char *value = text(field(record, fields[i]));
    if (!value)
      continue;
    GMatchInfo *matches = NULL;
    g_regex_match(urls, value, 0, &matches);
    while (g_match_info_matches(matches)) {
      char *match = g_match_info_fetch(matches, 0);
      size_t n = strlen(match);
      while (n && strchr(").,;", match[n - 1]))
        match[--n] = 0;
      char *url = public_https(match, secrets);
      GUri *uri =
          *url ? g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED, NULL) : NULL;
      const char *host = uri ? g_uri_get_host(uri) : NULL;
      if (host && g_regex_match(hosts, host, 0, NULL)) {
        g_free(out);
        out = g_strdup(url);
      }
      if (uri)
        g_uri_unref(uri);
      g_free(url);
      g_free(match);
      if (*out)
        break;
      g_match_info_next(matches, NULL);
    }
    g_match_info_free(matches);
  }
  g_regex_unref(urls);
  g_regex_unref(hosts);
  return out;
}
static const char *calendar_color(const char *name) {
  static const char *colors[] = {"#4285f4", "#0b8043", "#f6bf26", "#f83a22",
                                 "#8e24aa", "#039be5", "#e67c73", "#616161"};
  GChecksum *s = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(s, (const guchar *)name, (gssize)strlen(name));
  unsigned char digest[32];
  gsize len = sizeof digest;
  g_checksum_get_digest(s, digest, &len);
  g_checksum_free(s);
  return colors[digest[0] % 8];
}
yyjson_mut_doc *bar_rows(yyjson_val *records, const char *name, GTimeZone *tz,
                         GPtrArray *secrets) {
  if (!yyjson_is_arr(records)) {
    bad("invalid calendar");
    return NULL;
  }
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *rows = yyjson_mut_arr(doc);
  yyjson_mut_doc_set_root(doc, rows);
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  bool ok = true;
  yyjson_val *record;
  size_t i, max;
  yyjson_arr_foreach(records, i, max, record) {
    yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *event = yyjson_mut_obj(tmp);
    yyjson_mut_doc_set_root(tmp, event);
    const char *names[] = {"summary", "start", "end"};
    for (size_t j = 0; j < 3; j++) {
      yyjson_val *v = field(record, names[j]);
      yyjson_mut_obj_add_val(
          tmp, event, names[j],
          v ? yyjson_val_mut_copy(tmp, v)
            : (j == 0 ? yyjson_mut_str(tmp, "") : yyjson_mut_null(tmp)));
    }
    yyjson_mut_obj_add_strcpy(tmp, event, "calendar", name);
    yyjson_doc *raw = yyjson_mut_doc_imut_copy(tmp, NULL);
    yyjson_mut_val *normalized =
        normalize(doc, yyjson_doc_get_root(raw), tz, secrets);
    yyjson_doc_free(raw);
    yyjson_mut_doc_free(tmp);
    if (!normalized) {
      ok = false;
      break;
    }
    yyjson_mut_val *start = yyjson_mut_obj_get(normalized, "start"),
                   *end = yyjson_mut_obj_get(normalized, "end");
    bool all_day = yyjson_mut_obj_get(start, "date") != NULL;
    const char *key = all_day ? "date" : "dateTime",
               *first = yyjson_mut_get_str(yyjson_mut_obj_get(start, key)),
               *last = end ? yyjson_mut_get_str(yyjson_mut_obj_get(end, key))
                           : NULL;
    if (!last)
      last = first;
    GDateTime *a = all_day ? parse_day(first, tz) : parse_stamp(first),
              *b = all_day ? parse_day(last, tz) : parse_stamp(last);
    if (a && !all_day) {
      GDateTime *c = g_date_time_to_timezone(a, tz);
      g_date_time_unref(a);
      a = c;
    }
    if (b && !all_day) {
      GDateTime *c = g_date_time_to_timezone(b, tz);
      g_date_time_unref(b);
      b = c;
    }
    if (!a || !b) {
      if (a)
        g_date_time_unref(a);
      if (b)
        g_date_time_unref(b);
      ok = bad("invalid event time");
      break;
    }
    if (all_day) {
      if (!end) {
        g_date_time_unref(b);
        b = g_date_time_add_days(a, 1);
      }
      GDateTime *c = g_date_time_add_days(b, -1);
      g_date_time_unref(b);
      b = c;
    } else if (g_date_time_get_hour(b) == 0 && g_date_time_get_minute(b) == 0 &&
               g_date_time_get_second(b) == 0 &&
               g_date_time_get_microsecond(b) == 0 &&
               g_date_time_compare(b, a) > 0) {
      GDateTime *c = g_date_time_add_days(b, -1);
      g_date_time_unref(b);
      b = c;
    }
    char *a_day = iso_day(a), *b_day = iso_day(b);
    GTimeZone *utc = g_time_zone_new_utc();
    GDateTime *begin = parse_day(a_day, utc), *finish = parse_day(b_day, utc);
    g_time_zone_unref(utc);
    if (g_date_time_compare(finish, begin) < 0 && !all_day) {
      g_date_time_unref(finish);
      finish = g_date_time_ref(begin);
    }
    g_free(a_day);
    g_free(b_day);
    g_date_time_unref(a);
    g_date_time_unref(b);
    if (g_date_time_compare(finish, begin) < 0 ||
        g_date_time_difference(finish, begin) > 90 * G_TIME_SPAN_DAY) {
      g_date_time_unref(begin);
      g_date_time_unref(finish);
      ok = bad("invalid event time");
      break;
    }
    yyjson_mut_doc *start_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(start_doc,
                            yyjson_mut_val_mut_copy(start_doc, start));
    char *serialized = json_string(start_doc, true);
    yyjson_mut_doc_free(start_doc);
    const char *uid = text(field(record, "uid"));
    if (!uid)
      uid = "";
    GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(hash, (const guchar *)uid, (gssize)strlen(uid));
    g_checksum_update(hash, (const guchar *)"", 1);
    g_checksum_update(hash, (const guchar *)serialized,
                      (gssize)strlen(serialized));
    char *id = g_strndup(g_checksum_get_string(hash), 20);
    g_checksum_free(hash);
    g_free(serialized);
    char *location = redact(text(field(record, "location")), secrets, 500),
         *link = meeting(record, secrets),
         *page = public_https(text(field(record, "url")), secrets);
    GDateTime *day = g_date_time_ref(begin);
    while (g_date_time_compare(day, finish) <= 0) {
      char *date_key = iso_day(day),
           *seen_key = g_strconcat(id, "\037", date_key, NULL);
      if (!g_hash_table_contains(seen, seen_key)) {
        g_hash_table_add(seen, seen_key);
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, row, "id", id);
        yyjson_mut_obj_add_strcpy(doc, row, "calendarId", name);
        yyjson_mut_obj_add_strcpy(doc, row, "calendarName", name);
        yyjson_mut_obj_add_str(doc, row, "color", calendar_color(name));
        yyjson_mut_obj_add_strcpy(doc, row, "dateKey", date_key);
        yyjson_mut_obj_add_strcpy(doc, row, "start", first);
        yyjson_mut_obj_add_strcpy(doc, row, "end", last);
        yyjson_mut_obj_add_bool(doc, row, "allDay", all_day);
        yyjson_mut_obj_add_strcpy(
            doc, row, "title",
            yyjson_mut_get_str(yyjson_mut_obj_get(normalized, "summary")));
        yyjson_mut_obj_add_strcpy(doc, row, "location", location);
        if (*link)
          yyjson_mut_obj_add_strcpy(doc, row, "meetingUrl", link);
        if (*page && strcmp(page, link))
          yyjson_mut_obj_add_strcpy(doc, row, "eventUrl", page);
        yyjson_mut_arr_add_val(rows, row);
      } else
        g_free(seen_key);
      g_free(date_key);
      GDateTime *next = g_date_time_add_days(day, 1);
      g_date_time_unref(day);
      day = next;
      if (yyjson_mut_arr_size(rows) > BAR_MAX_EVENTS) {
        ok = bad("too many events");
        break;
      }
    }
    g_date_time_unref(day);
    g_date_time_unref(begin);
    g_date_time_unref(finish);
    g_free(id);
    g_free(location);
    g_free(link);
    g_free(page);
    if (!ok)
      break;
  }
  g_hash_table_destroy(seen);
  if (!ok) {
    yyjson_mut_doc_free(doc);
    return NULL;
  }
  return doc;
}
static int row_compare(const void *a, const void *b) {
  yyjson_mut_val *x = *(yyjson_mut_val *const *)a,
                 *y = *(yyjson_mut_val *const *)b;
  const char *names[] = {"dateKey", "start", "title"};
  int cmp = strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(x, names[0])),
                   yyjson_mut_get_str(yyjson_mut_obj_get(y, names[0])));
  if (cmp)
    return cmp;
  bool xd = yyjson_mut_get_bool(yyjson_mut_obj_get(x, "allDay")),
       yd = yyjson_mut_get_bool(yyjson_mut_obj_get(y, "allDay"));
  if (xd != yd)
    return xd ? 1 : -1;
  for (size_t i = 1; i < 3; i++) {
    cmp = strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(x, names[i])),
                 yyjson_mut_get_str(yyjson_mut_obj_get(y, names[i])));
    if (cmp)
      return cmp;
  }
  return 0;
}
bool export_bar(yyjson_mut_doc *fs, GDateTime *now, GTimeZone *tz,
                const char *zone_name, const char *program) {
  GDateTime *start = g_date_time_add_days(now, -7),
            *end = g_date_time_add_days(now, 61);
  yyjson_doc *input = yyjson_mut_doc_imut_copy(fs, NULL);
  yyjson_val *root = yyjson_doc_get_root(input);
  GPtrArray *names = keys(root), *all = g_ptr_array_new();
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  bool ok = names != NULL;
  for (size_t i = 0; names && i < names->len && ok; i++) {
    const char *name = names->pdata[i], *url = text(field(root, name));
    GBytes *bytes = NULL;
    yyjson_mut_doc *expanded =
        open_feed(url, &bytes)
            ? expand(bytes, program, start, end, zone_name, true)
            : NULL;
    if (bytes)
      g_bytes_unref(bytes);
    GPtrArray *secrets = feed_secrets(url);
    yyjson_doc *records =
        expanded ? yyjson_mut_doc_imut_copy(expanded, NULL) : NULL;
    yyjson_mut_doc *rows =
        records ? bar_rows(yyjson_doc_get_root(records), name, tz, secrets)
                : NULL;
    if (!rows)
      ok = false;
    else {
      yyjson_mut_val *row;
      size_t j, max;
      yyjson_mut_arr_foreach(yyjson_mut_doc_get_root(rows), j, max, row) {
        g_ptr_array_add(all, yyjson_mut_val_mut_copy(doc, row));
        if (all->len > BAR_MAX_EVENTS) {
          ok = bad("too many events");
          break;
        }
      }
      yyjson_mut_doc_free(rows);
    }
    if (records)
      yyjson_doc_free(records);
    if (expanded)
      yyjson_mut_doc_free(expanded);
    if (secrets)
      g_ptr_array_free(secrets, true);
  }
  if (ok) {
    g_ptr_array_sort(all, row_compare);
    yyjson_mut_val *value = yyjson_mut_obj(doc), *events = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, value);
    yyjson_mut_obj_add_uint(doc, value, "version", 1);
    char *stamp = iso_stamp(now);
    yyjson_mut_obj_add_strcpy(doc, value, "syncedAt", stamp);
    g_free(stamp);
    yyjson_mut_obj_add_str(doc, value, "source", "google-ical");
    for (size_t i = 0; i < all->len; i++)
      yyjson_mut_arr_add_val(events, all->pdata[i]);
    yyjson_mut_obj_add_val(doc, value, "events", events);
    char *state = env_path("XDG_STATE_HOME", ".local/state"),
         *path = g_build_filename(state, "omarchy/calendar-events.json", NULL);
    ok = save_cache(path, doc);
    g_free(state);
    g_free(path);
  }
  g_ptr_array_free(all, true);
  if (names)
    g_ptr_array_free(names, true);
  yyjson_mut_doc_free(doc);
  yyjson_doc_free(input);
  g_date_time_unref(start);
  g_date_time_unref(end);
  return ok;
}
