#include "agenda.h"
#include <stdio.h>
#include <string.h>
GTimeZone *zone(const char *name) {
  icaltimezone_set_builtin_tzdata(0);
  if (!strcmp(name, "local"))
    return g_time_zone_new_identifier("/etc/localtime");
  icaltimezone *z = icaltimezone_get_builtin_timezone(name);
  if (!z && strcmp(name, "UTC") && strcmp(name, "Etc/UTC") &&
      strcmp(name, "GMT"))
    return NULL;
  char *path = g_build_filename(AGENDA_ZONEINFO, name, NULL);
  GTimeZone *out = g_time_zone_new_identifier(path);
  g_free(path);
  return out;
}
GTimeZone *local_zone(char **name) {
  const char *raw = g_getenv("TZ");
  if (raw) {
    while (*raw == ':')
      raw++;
    if (*raw) {
      GTimeZone *z = zone(raw);
      if (z && strcmp(raw, "local")) {
        *name = g_strdup(raw);
        return z;
      }
      if (z)
        g_time_zone_unref(z);
    }
  }
  *name = g_strdup("local");
  GTimeZone *out = g_time_zone_new_identifier("/etc/localtime");
  if (!out)
    out = g_time_zone_new_identifier(NULL);
  return out ? out : g_time_zone_new_utc();
}
GDateTime *parse_day(const char *value, GTimeZone *tz) {
  if (!value)
    return NULL;
  size_t len = strlen(value);
  int y = 0, m = 0, d = 0;
  if (strchr(value, 'W')) {
    bool syntax =
        (len == 7 && value[4] == 'W') || (len == 8 && value[4] == 'W') ||
        (len == 8 && value[4] == '-' && value[5] == 'W') ||
        (len == 10 && value[4] == '-' && value[5] == 'W' && value[8] == '-');
    if (!syntax)
      return NULL;
    char compact[9] = {0};
    size_t n = 0;
    for (const char *p = value; *p; p++)
      if (*p != '-') {
        if (n >= 8)
          return NULL;
        compact[n++] = *p;
      }
    if ((n != 7 && n != 8) || compact[4] != 'W')
      return NULL;
    for (size_t i = 0; i < n; i++)
      if (i != 4 && !g_ascii_isdigit(compact[i]))
        return NULL;
    y = (compact[0] - '0') * 1000 + (compact[1] - '0') * 100 +
        (compact[2] - '0') * 10 + compact[3] - '0';
    int week = (compact[5] - '0') * 10 + compact[6] - '0',
        weekday = n == 8 ? compact[7] - '0' : 1;
    if (week < 1 || week > 53 || weekday < 1 || weekday > 7)
      return NULL;
    GDateTime *jan = g_date_time_new_utc(y, 1, 4, 0, 0, 0);
    if (!jan)
      return NULL;
    GDateTime *day = g_date_time_add_days(
        jan, (week - 1) * 7 + weekday - g_date_time_get_day_of_week(jan));
    g_date_time_unref(jan);
    if (g_date_time_get_week_numbering_year(day) != y ||
        g_date_time_get_week_of_year(day) != week) {
      g_date_time_unref(day);
      return NULL;
    }
    m = g_date_time_get_month(day);
    d = g_date_time_get_day_of_month(day);
    y = g_date_time_get_year(day);
    g_date_time_unref(day);
  } else {
    if (len != 8 && len != 10)
      return NULL;
    for (size_t i = 0; i < len; i++)
      if (len == 10 && (i == 4 || i == 7)) {
        if (value[i] != '-')
          return NULL;
      } else if (!g_ascii_isdigit(value[i]))
        return NULL;
    if (len == 10) {
      if (sscanf(value, "%4d-%2d-%2d", &y, &m, &d) != 3)
        return NULL;
    } else if (sscanf(value, "%4d%2d%2d", &y, &m, &d) != 3)
      return NULL;
  }
  return g_date_time_new(tz, y, m, d, 0, 0, 0);
}
GDateTime *parse_stamp(const char *value) {
  if (!value || !*value || strlen(value) > 64)
    return NULL;
  const char *time = strchr(value, 'T');
  if (!time)
    time = strchr(value, ' ');
  if (!time)
    return NULL;
  const char *p = time + 1;
  bool aware = strchr(p, 'Z') || strchr(p, '+') || strchr(p, '-');
  if (!aware)
    return NULL;
  return g_date_time_new_from_iso8601(value, NULL);
}
char *iso_day(GDateTime *t) { return g_date_time_format(t, "%Y-%m-%d"); }
char *iso_stamp(GDateTime *t) {
  char *base = g_date_time_format(t, "%Y-%m-%dT%H:%M:%S");
  GString *out = g_string_new(base);
  g_free(base);
  int micro = g_date_time_get_microsecond(t);
  if (micro)
    g_string_append_printf(out, ".%06d", micro);
  gint64 offset = g_date_time_get_utc_offset(t) / G_TIME_SPAN_SECOND;
  gint64 magnitude = offset < 0 ? -offset : offset;
  g_string_append_printf(out, "%c%02d:%02d", offset < 0 ? '-' : '+',
                         (int)(magnitude / 3600), (int)(magnitude / 60 % 60));
  if (magnitude % 60)
    g_string_append_printf(out, ":%02d", (int)(magnitude % 60));
  return g_string_free(out, false);
}
char *event_time(yyjson_val *event, GTimeZone *tz) {
  yyjson_val *start = field(event, "start");
  if (!yyjson_is_obj(start)) {
    bad("invalid event start");
    return NULL;
  }
  yyjson_val *day = field(start, "date");
  if (truth(day)) {
    const char *s = text(day);
    GTimeZone *utc = g_time_zone_new_utc();
    GDateTime *t = s ? parse_day(s, utc) : NULL;
    g_time_zone_unref(utc);
    if (!t) {
      bad("invalid event time");
      return NULL;
    }
    g_date_time_unref(t);
    return g_strdup("All day");
  }
  GDateTime *t = parse_stamp(text(field(start, "dateTime")));
  if (!t) {
    bad("invalid event timestamp");
    return NULL;
  }
  GDateTime *local = g_date_time_to_timezone(t, tz);
  char *out = g_date_time_format(local, "%H:%M");
  g_date_time_unref(t);
  g_date_time_unref(local);
  return out;
}
static bool validate_end(yyjson_val *event, GTimeZone *tz) {
  yyjson_val *end = field(event, "end");
  if (!end || yyjson_is_null(end))
    return true;
  if (!yyjson_is_obj(end))
    return bad("invalid event time");
  yyjson_val *start = field(event, "start");
  bool allday = field(end, "date") != NULL;
  const char *key = allday ? "date" : "dateTime";
  if (!field(start, key) || !field(end, key))
    return bad("invalid event time");
  (void)tz;
  GTimeZone *utc = g_time_zone_new_utc();
  GDateTime *a = allday ? parse_day(text(field(start, key)), utc)
                        : parse_stamp(text(field(start, key)));
  GDateTime *b = allday ? parse_day(text(field(end, key)), utc)
                        : parse_stamp(text(field(end, key)));
  g_time_zone_unref(utc);
  bool ok =
      a && b &&
      (allday ? g_date_time_compare(a, b) < 0 : g_date_time_compare(a, b) <= 0);
  if (a)
    g_date_time_unref(a);
  if (b)
    g_date_time_unref(b);
  return ok ? true : bad("invalid event time");
}
char *safe_link(const char *value) {
  char *s = clean(value, 2000), *lower = g_utf8_casefold(s, -1);
  bool safe = g_str_has_prefix(s, "https://calendar.google.com/") &&
              !strstr(lower, "private-") && !strstr(lower, "basic.ics") &&
              !strstr(lower, "/ical/");
  g_free(lower);
  if (!safe)
    *s = 0;
  return s;
}
yyjson_mut_val *normalize(yyjson_mut_doc *doc, yyjson_val *event, GTimeZone *tz,
                          GPtrArray *secrets) {
  if (!yyjson_is_obj(event)) {
    bad("invalid event");
    return NULL;
  }
  yyjson_val *summary = field(event, "summary");
  if (summary && !yyjson_is_str(summary)) {
    bad("invalid event");
    return NULL;
  }
  yyjson_val *start = field(event, "start");
  if (!yyjson_is_obj(start)) {
    bad("invalid event");
    return NULL;
  }
  yyjson_mut_val *out = yyjson_mut_obj(doc);
  char *summary_text =
      summary ? clean_value(summary, 2000) : g_strdup("(Untitled event)");
  char *s = redact(summary_text, secrets, 2000);
  g_free(summary_text);
  yyjson_mut_obj_add_strcpy(doc, out, "summary", s);
  g_free(s);
  const char *key = field(start, "date") ? "date" : "dateTime";
  yyjson_mut_val *time = yyjson_mut_obj(doc),
                 *v = yyjson_val_mut_copy(doc, field(start, key));
  yyjson_mut_obj_add_val(doc, time, key, v ? v : yyjson_mut_null(doc));
  yyjson_mut_obj_add_val(doc, out, "start", time);
  yyjson_val *calendar = field(event, "calendar");
  if (calendar) {
    if (!yyjson_is_str(calendar) || !yyjson_get_len(calendar) ||
        string_length(calendar) > 40) {
      bad("invalid calendar name");
      return NULL;
    }
    char *name = clean_value(calendar, 2000);
    s = redact(name, secrets, 2000);
    g_free(name);
    yyjson_mut_obj_add_strcpy(doc, out, "calendar", s);
    g_free(s);
  }
  yyjson_val *end = field(event, "end");
  if (end && !yyjson_is_null(end)) {
    if (!yyjson_is_obj(end) ||
        (!field(end, "date") && !field(end, "dateTime"))) {
      bad("invalid event time");
      return NULL;
    }
    key = field(end, "date") ? "date" : "dateTime";
    time = yyjson_mut_obj(doc);
    v = yyjson_val_mut_copy(doc, field(end, key));
    yyjson_mut_obj_add_val(doc, time, key, v ? v : yyjson_mut_null(doc));
    yyjson_mut_obj_add_val(doc, out, "end", time);
  }
  s = redact(text(field(event, "htmlLink")), secrets, 2000);
  char *link = safe_link(s);
  if (*link)
    yyjson_mut_obj_add_strcpy(doc, out, "htmlLink", link);
  g_free(s);
  g_free(link);
  yyjson_doc *check = yyjson_mut_val_imut_copy(out, NULL);
  yyjson_val *root = yyjson_doc_get_root(check);
  s = event_time(root, tz);
  bool ok = s && validate_end(root, tz);
  g_free(s);
  yyjson_doc_free(check);
  return ok ? out : NULL;
}
