#define _DEFAULT_SOURCE
#include "agenda.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
  icalcomponent *component;
  struct icaltimetype start, end, rid;
  gint64 duration;
  const char *uid;
  bool cancelled, future;
  int sequence;
} Event;
typedef struct {
  Event *master;
  GPtrArray *overrides;
} Series;
typedef struct {
  struct icaltimetype time;
  gint64 duration;
  int kind; // RDATE, DTSTART, then RRULE, matching legacy union precedence.
  bool period;
} Candidate;
static bool valid_time(struct icaltimetype t) {
  if (icaltime_is_null_time(t) || !icaltime_is_valid_time(t))
    return false;
  GDateTime *d =
      g_date_time_new_utc(t.year, t.month, t.day, t.hour, t.minute, t.second);
  if (d)
    g_date_time_unref(d);
  return d != NULL;
}
static gint64 wall(struct icaltimetype t) {
  GDateTime *d =
      g_date_time_new_utc(t.year, t.month, t.day, t.hour, t.minute, t.second);
  if (!d)
    return 0;
  gint64 n = g_date_time_to_unix(d);
  g_date_time_unref(d);
  return n;
}
static struct icaltimetype shift(struct icaltimetype t, gint64 seconds) {
  if (t.is_date) {
    icaltime_adjust(&t, (int)(seconds / 86400), 0, 0, 0);
    return t;
  }
  while (seconds > 1000000000) {
    icaltime_adjust(&t, 0, 0, 0, 1000000000);
    seconds -= 1000000000;
  }
  while (seconds < -1000000000) {
    icaltime_adjust(&t, 0, 0, 0, -1000000000);
    seconds += 1000000000;
  }
  icaltime_adjust(&t, 0, 0, 0, (int)seconds);
  return t;
}
static GDateTime *wall_time(struct icaltimetype t, GTimeZone *tz) {
  gint64 w = wall(t);
  int before = g_time_zone_get_offset(
          tz, g_time_zone_find_interval(tz, G_TIME_TYPE_UNIVERSAL, w - 86400)),
      after = g_time_zone_get_offset(
          tz, g_time_zone_find_interval(tz, G_TIME_TYPE_UNIVERSAL, w + 86400));
  bool a = g_time_zone_get_offset(
               tz, g_time_zone_find_interval(tz, G_TIME_TYPE_UNIVERSAL,
                                             w - before)) == before,
       b = g_time_zone_get_offset(
               tz, g_time_zone_find_interval(tz, G_TIME_TYPE_UNIVERSAL,
                                             w - after)) == after;
  int offset =
      a && b ? MAX(before, after) : (a ? before : (b ? after : before));
  GTimeZone *fixed = g_time_zone_new_offset(offset);
  GDateTime *out = g_date_time_new(fixed, t.year, t.month, t.day, t.hour,
                                   t.minute, t.second);
  g_time_zone_unref(fixed);
  return out;
}
static GDateTime *date_time(struct icaltimetype t, GTimeZone *attach,
                            GTimeZone *host) {
  if (t.is_date)
    return g_date_time_new(host, t.year, t.month, t.day, 0, 0, 0);
  if (icaltime_is_utc(t))
    return g_date_time_new_utc(t.year, t.month, t.day, t.hour, t.minute,
                               t.second);
  if (t.zone) {
    const char *location = icaltimezone_get_location((icaltimezone *)t.zone);
    GTimeZone *z = location ? zone(location) : NULL;
    if (z) {
      GDateTime *out = wall_time(t, z);
      g_time_zone_unref(z);
      return out;
    }
    int daylight = 0,
        offset =
            icaltimezone_get_utc_offset((icaltimezone *)t.zone, &t, &daylight);
    if (offset <= -86400 || offset >= 86400)
      return NULL;
    GTimeZone *fixed = g_time_zone_new_offset(offset);
    GDateTime *out = g_date_time_new(fixed, t.year, t.month, t.day, t.hour,
                                     t.minute, t.second);
    g_time_zone_unref(fixed);
    return out;
  }
  return wall_time(t, attach);
}
static gint64 utc_alias(struct icaltimetype t, GTimeZone *attach,
                        GTimeZone *host) {
  GDateTime *d = !t.is_date && t.zone ? date_time(t, attach, host) : NULL;
  gint64 n = d ? g_date_time_to_unix(d) : wall(t);
  if (d)
    g_date_time_unref(d);
  return n;
}
static gint64 move_seconds(Event *e, GTimeZone *attach, GTimeZone *host) {
  return e->start.zone && e->rid.zone && e->start.zone != e->rid.zone
             ? utc_alias(e->start, attach, host) -
                   utc_alias(e->rid, attach, host)
             : wall(e->start) - wall(e->rid);
}
static bool recurrence_same(struct icaltimetype a, struct icaltimetype b,
                            GTimeZone *attach, GTimeZone *host) {
  // Legacy recurrence IDs expose both the UTC and wall-clock aliases.
  gint64 ak = wall(a), bk = wall(b), au = ak, bu = bk;
  GDateTime *at = !a.is_date && a.zone ? date_time(a, attach, host) : NULL,
            *bt = !b.is_date && b.zone ? date_time(b, attach, host) : NULL;
  if (at) {
    au = g_date_time_to_unix(at);
    g_date_time_unref(at);
  }
  if (bt) {
    bu = g_date_time_to_unix(bt);
    g_date_time_unref(bt);
  }
  return ak == bk || ak == bu || au == bk || au == bu;
}
static bool excluded(Event *master, struct icaltimetype t, GTimeZone *attach,
                     GTimeZone *host, bool date_wildcard) {
  for (icalproperty *p = icalcomponent_get_first_property(master->component,
                                                          ICAL_EXDATE_PROPERTY);
       p; p = icalcomponent_get_next_property(master->component,
                                              ICAL_EXDATE_PROPERTY)) {
    struct icaltimetype x =
        icalproperty_get_datetime_with_component(p, master->component);
    if ((date_wildcard && x.is_date && x.year == t.year && x.month == t.month &&
         x.day == t.day) ||
        recurrence_same(x, t, attach, host))
      return true;
  }
  return false;
}
static const char *property_text(icalcomponent *c, icalproperty_kind kind) {
  icalproperty *p = icalcomponent_get_first_property(c, kind);
  return p ? icalvalue_get_text(icalproperty_get_value(p)) : "";
}
static char *property_display(icalcomponent *c, icalproperty_kind kind) {
  int count = icalcomponent_count_properties(c, kind);
  if (count <= 1)
    return g_strdup(property_text(c, kind));
  GString *out = g_string_new("[");
  for (icalproperty *p = icalcomponent_get_first_property(c, kind); p;
       p = icalcomponent_get_next_property(c, kind)) {
    if (out->len > 1)
      g_string_append(out, ", ");
    const char *raw = icalvalue_get_text(icalproperty_get_value(p));
    if (!raw)
      raw = "";
    bool bytes = kind != ICAL_URL_PROPERTY;
    GString *encoded = g_string_new(NULL);
    for (const char *s = raw; *s; s++) {
      if (bytes && (*s == '\\' || *s == ',' || *s == ';'))
        g_string_append_c(encoded, '\\');
      if (bytes && *s == '\n')
        g_string_append(encoded, "\\n");
      else
        g_string_append_c(encoded, *s);
    }
    char quote =
        strchr(encoded->str, '\'') && !strchr(encoded->str, '\"') ? '\"' : '\'';
    if (bytes)
      g_string_append(out, "vText(b");
    g_string_append_c(out, quote);
    for (const unsigned char *s = (const unsigned char *)encoded->str; *s;
         s++) {
      if (*s == quote || *s == '\\') {
        g_string_append_c(out, '\\');
        g_string_append_c(out, (char)*s);
      } else if (*s == '\n')
        g_string_append(out, "\\n");
      else if (*s == '\r')
        g_string_append(out, "\\r");
      else if (*s == '\t')
        g_string_append(out, "\\t");
      else if (*s < 32 || *s >= 127)
        g_string_append_printf(out, "\\x%02x", *s);
      else
        g_string_append_c(out, (char)*s);
    }
    g_string_append_c(out, quote);
    if (bytes)
      g_string_append_c(out, ')');
    g_string_free(encoded, true);
  }
  g_string_append_c(out, ']');
  return g_string_free(out, false);
}
static struct icaltimetype property_zone(struct icaltimetype t, icalproperty *p,
                                         icalcomponent *c) {
  if (icaltime_is_utc(t))
    return t;
  icalparameter *parameter =
      icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER);
  if (!parameter)
    return t;
  const char *name = icalparameter_get_tzid(parameter);
  icaltimezone *tz = NULL;
  for (icalcomponent *at = c; at && !tz; at = icalcomponent_get_parent(at))
    tz = icalcomponent_get_timezone(at, name);
  if (!tz)
    tz = icaltimezone_get_builtin_timezone_from_tzid(name);
  if (!tz)
    tz = icaltimezone_get_builtin_timezone(name);
  return tz ? icaltime_set_timezone(&t, tz) : t;
}
static struct icaltimetype prop_time(icalcomponent *c, icalproperty_kind kind) {
  icalproperty *p = icalcomponent_get_first_property(c, kind);
  return p ? icalproperty_get_datetime_with_component(p, c)
           : icaltime_null_time();
}
static gint64 duration_seconds(struct icaldurationtype d) {
  gint64 n = (((gint64)d.weeks * 7 + d.days) * 24 + d.hours) * 3600 +
             (gint64)d.minutes * 60 + d.seconds;
  return d.is_neg ? -n : n;
}
static void series_free(void *p) {
  Series *s = p;
  g_ptr_array_free(s->overrides, true);
  g_free(s);
}
static Event *event_new(icalcomponent *c) {
  if (icalcomponent_count_errors(c) ||
      icalcomponent_count_properties(c, ICAL_DTSTART_PROPERTY) != 1 ||
      icalcomponent_count_properties(c, ICAL_DTEND_PROPERTY) > 1 ||
      icalcomponent_count_properties(c, ICAL_DURATION_PROPERTY) > 1 ||
      icalcomponent_count_properties(c, ICAL_RRULE_PROPERTY) > 1 ||
      icalcomponent_count_properties(c, ICAL_UID_PROPERTY) > 1) {
    bad("invalid recurrence");
    return NULL;
  }
  Event *e = g_new0(Event, 1);
  e->component = c;
  e->start = prop_time(c, ICAL_DTSTART_PROPERTY);
  e->end = prop_time(c, ICAL_DTEND_PROPERTY);
  e->rid = prop_time(c, ICAL_RECURRENCEID_PROPERTY);
  if (!valid_time(e->start) ||
      (!icaltime_is_null_time(e->rid) && !valid_time(e->rid))) {
    g_free(e);
    bad("invalid event time");
    return NULL;
  }
  e->uid = icalcomponent_get_uid(c);
  if (!e->uid)
    e->uid = "";
  e->cancelled = icalcomponent_count_properties(c, ICAL_STATUS_PROPERTY) == 1 &&
                 icalcomponent_get_status(c) == ICAL_STATUS_CANCELLED;
  e->sequence = icalcomponent_get_first_property(c, ICAL_SEQUENCE_PROPERTY)
                    ? icalcomponent_get_sequence(c)
                    : -1;
  icalproperty *rid =
      icalcomponent_get_first_property(c, ICAL_RECURRENCEID_PROPERTY);
  icalparameter *range =
      rid ? icalproperty_get_first_parameter(rid, ICAL_RANGE_PARAMETER) : NULL;
  e->future =
      range && icalparameter_get_range(range) == ICAL_RANGE_THISANDFUTURE;
  if (!icaltime_is_null_time(e->end)) {
    if (!valid_time(e->end) || e->start.is_date != e->end.is_date) {
      g_free(e);
      bad("invalid event time");
      return NULL;
    }
    gint64 duration;
    bool start_aware = e->start.zone || icaltime_is_utc(e->start),
         end_aware = e->end.zone || icaltime_is_utc(e->end);
    if (start_aware && end_aware &&
        (e->start.zone != e->end.zone ||
         icaltime_is_utc(e->start) != icaltime_is_utc(e->end))) {
      GTimeZone *utc = g_time_zone_new_utc();
      GDateTime *a = date_time(e->start, utc, utc),
                *b = date_time(e->end, utc, utc);
      g_time_zone_unref(utc);
      if (!a || !b) {
        if (a)
          g_date_time_unref(a);
        if (b)
          g_date_time_unref(b);
        g_free(e);
        bad("invalid event time");
        return NULL;
      }
      duration = g_date_time_difference(b, a) / G_TIME_SPAN_SECOND;
      g_date_time_unref(a);
      g_date_time_unref(b);
    } else
      duration = wall(e->end) - wall(e->start);
    if (duration < 0 || (e->start.is_date && !duration)) {
      g_free(e);
      bad("invalid event time");
      return NULL;
    }
    e->duration = duration;
  } else {
    icalproperty *p =
        icalcomponent_get_first_property(c, ICAL_DURATION_PROPERTY);
    e->duration = p ? duration_seconds(icalproperty_get_duration(p))
                    : (e->start.is_date ? 86400 : 0);
    if (e->duration < 0) {
      g_free(e);
      bad("invalid event time");
      return NULL;
    }
  }
  for (icalproperty *p =
           icalcomponent_get_first_property(c, ICAL_RRULE_PROPERTY);
       p; p = icalcomponent_get_next_property(c, ICAL_RRULE_PROPERTY)) {
    struct icalrecurrencetype r = icalproperty_get_rrule(p);
    if (r.freq == ICAL_NO_RECURRENCE || r.interval < 1) {
      g_free(e);
      bad("invalid recurrence");
      return NULL;
    }
  }
  return e;
}
static const icaltimezone *recurrence_zone(Event *master) {
  if (!master->start.is_date && master->start.zone)
    return master->start.zone;
  if (!master->end.is_date && master->end.zone)
    return master->end.zone;
  for (icalproperty *p = icalcomponent_get_first_property(master->component,
                                                          ICAL_EXDATE_PROPERTY);
       p; p = icalcomponent_get_next_property(master->component,
                                              ICAL_EXDATE_PROPERTY)) {
    struct icaltimetype t =
        icalproperty_get_datetime_with_component(p, master->component);
    if (!t.is_date && t.zone)
      return t.zone;
  }
  for (icalproperty *p = icalcomponent_get_first_property(master->component,
                                                          ICAL_RDATE_PROPERTY);
       p; p = icalcomponent_get_next_property(master->component,
                                              ICAL_RDATE_PROPERTY)) {
    struct icaldatetimeperiodtype r = icalproperty_get_rdate(p);
    struct icaltimetype t =
        property_zone(icaltime_is_null_time(r.time) ? r.period.start : r.time,
                      p, master->component);
    if (!t.is_date && t.zone)
      return t.zone;
  }
  return NULL;
}
static struct icaltimetype recurrence_time(struct icaltimetype t, Event *master,
                                           const icaltimezone *tz) {
  if (!t.zone)
    t.zone = tz;
  t.is_date = master->start.is_date;
  if (t.is_date)
    t.hour = t.minute = t.second = 0;
  return t;
}
static int candidate_compare(const void *a, const void *b, void *data) {
  const Candidate *x = *(Candidate *const *)a, *y = *(Candidate *const *)b;
  GTimeZone **zones = data;
  if ((x->kind == 2) != (y->kind == 2))
    return x->kind == 2 ? 1 : -1;
  gint64 one = utc_alias(x->time, zones[0], zones[1]),
         two = utc_alias(y->time, zones[0], zones[1]);
  return one < two ? -1 : one > two ? 1 : x->kind - y->kind;
}
static bool overlaps(Event *e, struct icaltimetype start, gint64 duration,
                     GDateTime *low, GDateTime *high, GTimeZone *attach,
                     GTimeZone *host) {
  GDateTime *a = date_time(start, attach, host),
            *b = date_time(shift(start, duration), attach, host);
  bool yes = a && b && g_date_time_compare(a, high) < 0 &&
             (g_date_time_compare(b, low) > 0 ||
              (duration == 0 && g_date_time_compare(a, low) >= 0));
  if (a)
    g_date_time_unref(a);
  if (b)
    g_date_time_unref(b);
  (void)e;
  return yes;
}
static yyjson_mut_val *record(yyjson_mut_doc *doc, Event *e,
                              struct icaltimetype start, gint64 duration,
                              GTimeZone *attach, GTimeZone *host,
                              bool records) {
  GDateTime *a = date_time(start, attach, host),
            *b = date_time(shift(start, duration), attach, host);
  if (!a || !b) {
    if (a)
      g_date_time_unref(a);
    if (b)
      g_date_time_unref(b);
    bad("invalid event time");
    return NULL;
  }
  struct icaltimetype finish = shift(start, duration);
  char *first = start.is_date ? g_strdup_printf("%04d-%02d-%02d", start.year,
                                                start.month, start.day)
                              : iso_stamp(a),
       *last = start.is_date ? g_strdup_printf("%04d-%02d-%02d", finish.year,
                                               finish.month, finish.day)
                             : iso_stamp(b);
  yyjson_mut_val *out = yyjson_mut_obj(doc);
  if (records) {
    char *uid = clean(e->uid, 200);
    yyjson_mut_obj_add_strcpy(doc, out, "uid", uid);
    g_free(uid);
  }
  char *raw = property_display(e->component, ICAL_SUMMARY_PROPERTY);
  char *summary = clean(raw && *raw ? raw : "(Untitled event)", 2000);
  yyjson_mut_obj_add_strcpy(doc, out, "summary", summary);
  g_free(summary);
  g_free(raw);
  yyjson_mut_val *value = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, value, start.is_date ? "date" : "dateTime",
                            first);
  yyjson_mut_obj_add_val(doc, out, "start", value);
  value = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, value, start.is_date ? "date" : "dateTime",
                            last);
  yyjson_mut_val *ending = value;
  if (records) {
    const icalproperty_kind kinds[] = {
        ICAL_LOCATION_PROPERTY, ICAL_DESCRIPTION_PROPERTY, ICAL_URL_PROPERTY};
    const char *names[] = {"location", "description", "url"};
    for (size_t i = 0; i < 3; i++) {
      char *raw_value = property_display(e->component, kinds[i]);
      char *s = clean(raw_value, i == 1 ? 2000 : 500);
      g_free(raw_value);
      yyjson_mut_obj_add_strcpy(doc, out, names[i], s);
      g_free(s);
    }
    yyjson_mut_obj_add_val(doc, out, "end", ending);
  } else {
    yyjson_mut_obj_add_val(doc, out, "end", ending);
    char *url = property_display(e->component, ICAL_URL_PROPERTY);
    char *link = safe_link(url);
    g_free(url);
    if (*link)
      yyjson_mut_obj_add_strcpy(doc, out, "htmlLink", link);
    g_free(link);
  }
  g_free(first);
  g_free(last);
  g_date_time_unref(a);
  g_date_time_unref(b);
  return out;
}
static char *identity(yyjson_mut_val *event, const char *uid, bool records) {
  if (records)
    uid = yyjson_mut_get_str(yyjson_mut_obj_get(event, "uid"));
  yyjson_mut_val *start = yyjson_mut_obj_get(event, "start"),
                 *v = yyjson_mut_obj_get(start, "date");
  if (!v)
    v = yyjson_mut_obj_get(start, "dateTime");
  return g_strconcat(uid, "\037", yyjson_mut_get_str(v), NULL);
}
static int record_compare(const void *a, const void *b) {
  yyjson_mut_val *x = *(yyjson_mut_val *const *)a,
                 *y = *(yyjson_mut_val *const *)b;
  yyjson_mut_val *xs = yyjson_mut_obj_get(x, "start"),
                 *ys = yyjson_mut_obj_get(y, "start");
  bool xd = yyjson_mut_obj_get(xs, "date") != NULL,
       yd = yyjson_mut_obj_get(ys, "date") != NULL;
  if (xd != yd)
    return xd ? -1 : 1;
  int cmp = strcmp(
      yyjson_mut_get_str(yyjson_mut_obj_get(xs, xd ? "date" : "dateTime")),
      yyjson_mut_get_str(yyjson_mut_obj_get(ys, yd ? "date" : "dateTime")));
  if (cmp)
    return cmp;
  return strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(x, "summary")),
                yyjson_mut_get_str(yyjson_mut_obj_get(y, "summary")));
}
static bool append_event(yyjson_mut_doc *doc, GPtrArray *out, GHashTable *seen,
                         Event *e, struct icaltimetype t, gint64 duration,
                         GDateTime *low, GDateTime *high, GTimeZone *attach,
                         GTimeZone *host, bool records) {
  if (e->cancelled || !overlaps(e, t, duration, low, high, attach, host))
    return true;
  yyjson_mut_val *r = record(doc, e, t, duration, attach, host, records);
  if (!r)
    return false;
  char *key = identity(r, e->uid, records);
  if (g_hash_table_contains(seen, key)) {
    g_free(key);
    return true;
  }
  g_hash_table_add(seen, key);
  g_ptr_array_add(out, r);
  return out->len <= (records ? BAR_MAX_EVENTS : MAX_OCCURRENCES)
             ? true
             : bad("too many events");
}
static bool expand_series(Series *s, yyjson_mut_doc *doc, GPtrArray *out,
                          GHashTable *seen, GDateTime *low, GDateTime *high,
                          GTimeZone *attach, GTimeZone *host, bool records) {
  Event *master = s->master;
  if (!master) {
    for (size_t i = 0; i < s->overrides->len; i++) {
      Event *e = s->overrides->pdata[i];
      if (!append_event(doc, out, seen, e, e->start, e->duration, low, high,
                        attach, host, records))
        return false;
    }
    return true;
  }
  GPtrArray *candidates = g_ptr_array_new_with_free_func(g_free);
  Candidate *initial = g_new(Candidate, 1);
  const icaltimezone *recurrence_tz = recurrence_zone(master);
  *initial = (Candidate){recurrence_time(master->start, master, recurrence_tz),
                         master->duration, 1, false};
  g_ptr_array_add(candidates, initial);
  gint64 before = master->duration, after = 0;
  for (size_t i = 0; i < s->overrides->len; i++) {
    Event *e = s->overrides->pdata[i];
    if (e->future) {
      gint64 move = move_seconds(e, attach, host);
      before = MAX(before, e->duration + MAX(move, 0));
      after = MAX(after, -move);
    }
  }
  gint64 nominal_low = g_date_time_to_unix(low) - before,
         nominal_high = g_date_time_to_unix(high) + after;
  gint64 high_wall = nominal_high + 172800;
  for (size_t i = 0; i < s->overrides->len; i++) {
    Event *e = s->overrides->pdata[i];
    if (e->future) {
      gint64 offset = wall(e->start) - wall(e->rid);
      high_wall = MAX(high_wall, g_date_time_to_unix(high) + 172800 - offset);
    }
  }
  for (icalproperty *p = icalcomponent_get_first_property(master->component,
                                                          ICAL_RRULE_PROPERTY);
       p; p = icalcomponent_get_next_property(master->component,
                                              ICAL_RRULE_PROPERTY)) {
    struct icalrecurrencetype rule = icalproperty_get_rrule(p);
    icalrecur_iterator *iter = icalrecur_iterator_new(rule, initial->time);
    if (!iter) {
      g_ptr_array_free(candidates, true);
      return bad("invalid recurrence");
    }
    struct icaltimetype t;
    while (!icaltime_is_null_time(t = icalrecur_iterator_next(iter))) {
      if (wall(t) > high_wall)
        break;
      Candidate *c = g_new(Candidate, 1);
      *c = (Candidate){t, master->duration, 2, false};
      g_ptr_array_add(candidates, c);
    }
    icalrecur_iterator_free(iter);
  }
  for (icalproperty *p = icalcomponent_get_first_property(master->component,
                                                          ICAL_RDATE_PROPERTY);
       p; p = icalcomponent_get_next_property(master->component,
                                              ICAL_RDATE_PROPERTY)) {
    struct icaldatetimeperiodtype r = icalproperty_get_rdate(p);
    struct icaltimetype t =
        property_zone(icaltime_is_null_time(r.time) ? r.period.start : r.time,
                      p, master->component);
    gint64 duration = master->duration;
    if (icaltime_is_null_time(r.time)) {
      struct icaltimetype last =
          property_zone(r.period.end, p, master->component);
      duration = icaltime_is_null_time(last)
                     ? duration_seconds(r.period.duration)
                     : (t.zone && last.zone && t.zone != last.zone
                            ? utc_alias(last, attach, host) -
                                  utc_alias(t, attach, host)
                            : wall(last) - wall(t));
    }
    if (!valid_time(t)) {
      g_ptr_array_free(candidates, true);
      return bad("invalid recurrence");
    }
    Candidate *c = g_new(Candidate, 1);
    *c = (Candidate){recurrence_time(t, master, recurrence_tz), duration, 0,
                     icaltime_is_null_time(r.time)};
    g_ptr_array_add(candidates, c);
  }
  GTimeZone *zones[] = {attach, host};
  g_ptr_array_sort_with_data(candidates, candidate_compare, zones);
  GHashTable *starts =
      g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
  bool ok = true;
  for (size_t i = 0; i < candidates->len && ok; i++) {
    Candidate *c = candidates->pdata[i];
    Event *use = master, *future = NULL;
    Event *replacement = NULL;
    for (size_t j = 0; j < s->overrides->len; j++) {
      Event *e = s->overrides->pdata[j];
      if (recurrence_same(e->rid, c->time, attach, host)) {
        replacement = e;
        break;
      }
      if (e->future &&
          utc_alias(e->rid, attach, host) <= utc_alias(c->time, attach, host) &&
          (!future || utc_alias(e->rid, attach, host) >=
                          utc_alias(future->rid, attach, host)))
        future = e;
    }
    if (excluded(master, c->time, attach, host, true))
      continue;
    if (replacement) {
      GDateTime *nominal = date_time(c->time, attach, host);
      gint64 moment = nominal ? g_date_time_to_unix(nominal) : 0;
      if (nominal)
        g_date_time_unref(nominal);
      if (moment >= nominal_low && moment <= nominal_high)
        ok = append_event(doc, out, seen, replacement, replacement->start,
                          replacement->duration, low, high, attach, host,
                          records);
      continue;
    }
    gint64 moment = utc_alias(c->time, attach, host);
    if (g_hash_table_contains(starts, &moment))
      continue;
    gint64 *key = g_new(gint64, 1);
    *key = moment;
    g_hash_table_add(starts, key);
    struct icaltimetype t = c->time;
    gint64 duration = c->duration;
    if (future) {
      use = future;
      t = shift(t, move_seconds(future, attach, host));
      duration = c->period ? c->duration : future->duration;
    }
    ok = append_event(doc, out, seen, use, t, duration, low, high, attach, host,
                      records);
  }
  g_hash_table_destroy(starts);
  g_ptr_array_free(candidates, true);
  // Detached moves whose original occurrence was outside the query follow the
  // core occurrences. This ordering determines UID/start collision precedence.
  for (size_t i = 0; i < s->overrides->len && ok; i++) {
    Event *e = s->overrides->pdata[i];
    if (!excluded(master, e->rid, attach, host, false))
      ok = append_event(doc, out, seen, e, e->start, e->duration, low, high,
                        attach, host, records);
  }
  return ok;
}
yyjson_mut_doc *parse_calendar(GBytes *raw, GDateTime *start, GDateTime *end,
                               GTimeZone *host, const char *zone_name,
                               bool records) {
  (void)zone_name;
  gsize size;
  const char *bytes = g_bytes_get_data(raw, &size);
  if (size > MAX_BYTES) {
    bad("response too large");
    return NULL;
  }
  GString *visible = g_string_new(NULL);
  for (size_t i = 0; i < size; i++) {
    if (bytes[i])
      g_string_append_c(visible, bytes[i]);
    else
      g_string_append(visible, "\356\200\200");
  }
  char *input = g_utf8_make_valid(visible->str, (gssize)visible->len),
       *upper = g_ascii_strup(input, -1);
  g_string_free(visible, true);
  size_t count = 0;
  for (const char *p = upper; (p = strstr(p, "BEGIN:VEVENT")); p += 12)
    count++;
  bool valid = count <= MAX_COMPONENTS && strstr(upper, "BEGIN:VCALENDAR");
  g_free(upper);
  if (!valid) {
    g_free(input);
    bad("invalid calendar or too many events");
    return NULL;
  }
  icalcomponent *calendar = icalparser_parse_string(input);
  g_free(input);
  if (!calendar || icalcomponent_isa(calendar) != ICAL_VCALENDAR_COMPONENT) {
    if (calendar)
      icalcomponent_free(calendar);
    bad("invalid calendar");
    return NULL;
  }
  GTimeZone *attach = g_time_zone_ref(host);
  for (icalproperty *p =
           icalcomponent_get_first_property(calendar, ICAL_X_PROPERTY);
       p; p = icalcomponent_get_next_property(calendar, ICAL_X_PROPERTY))
    if (!g_ascii_strcasecmp(icalproperty_get_x_name(p), "X-WR-TIMEZONE")) {
      char *name = strip(icalproperty_get_x(p));
      if (*name) {
        GTimeZone *z = strcmp(name, "local") ? zone(name) : NULL;
        if (!z) {
          g_free(name);
          g_time_zone_unref(attach);
          icalcomponent_free(calendar);
          bad("invalid calendar timezone");
          return NULL;
        }
        g_time_zone_unref(attach);
        attach = z;
      }
      g_free(name);
      break;
    }
  GHashTable *map =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *groups = g_ptr_array_new_with_free_func(series_free),
            *events = g_ptr_array_new_with_free_func(g_free);
  bool ok = true;
  size_t index = 0;
  for (icalcomponent *c =
           icalcomponent_get_first_component(calendar, ICAL_VEVENT_COMPONENT);
       c;
       c = icalcomponent_get_next_component(calendar, ICAL_VEVENT_COMPONENT)) {
    Event *e = event_new(c);
    if (!e) {
      ok = false;
      break;
    }
    g_ptr_array_add(events, e);
    char *key =
        *e->uid ? g_strdup(e->uid) : g_strdup_printf("\037%zu", index++);
    Series *series = g_hash_table_lookup(map, key);
    if (!series) {
      series = g_new0(Series, 1);
      series->overrides = g_ptr_array_new();
      g_hash_table_insert(map, key, series);
      g_ptr_array_add(groups, series);
    } else
      g_free(key);
    if (icaltime_is_null_time(e->rid)) {
      if (!series->master || e->sequence > series->master->sequence)
        series->master = e;
    } else {
      bool found = false;
      for (size_t j = 0; j < series->overrides->len; j++) {
        Event *old = series->overrides->pdata[j];
        if (recurrence_same(old->rid, e->rid, attach, host)) {
          if (e->sequence > old->sequence)
            series->overrides->pdata[j] = e;
          found = true;
          break;
        }
      }
      if (!found)
        g_ptr_array_add(series->overrides, e);
    }
  }
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  GPtrArray *out = g_ptr_array_new();
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (size_t i = 0; i < groups->len && ok; i++)
    ok = expand_series(groups->pdata[i], doc, out, seen, start, end, attach,
                       host, records);
  if (!records)
    g_ptr_array_sort(out, record_compare);
  yyjson_mut_val *array = yyjson_mut_arr(doc);
  yyjson_mut_doc_set_root(doc, array);
  for (size_t i = 0; i < out->len; i++)
    yyjson_mut_arr_add_val(array, out->pdata[i]);
  g_hash_table_destroy(seen);
  g_ptr_array_free(out, true);
  g_hash_table_destroy(map);
  g_ptr_array_free(groups, true);
  g_ptr_array_free(events, true);
  g_time_zone_unref(attach);
  icalcomponent_free(calendar);
  if (!ok) {
    yyjson_mut_doc_free(doc);
    return NULL;
  }
  return doc;
}
int parse_cli(int argc, char **argv, bool range) {
  if (argc != (range ? 3 : 2))
    return 1;
  const char *requested = argv[range ? 2 : 1];
  GTimeZone *z = NULL;
  if (!strcmp(requested, "local")) {
    char *name = NULL;
    z = local_zone(&name);
    bool valid = !strcmp(name, "local");
    g_free(name);
    if (!valid) {
      if (z)
        g_time_zone_unref(z);
      return 1;
    }
  } else
    z = zone(requested);
  if (!z)
    return 1;
  GDateTime *start = parse_day(argv[0], z),
            *end = range ? parse_day(argv[1], z)
                         : (start ? g_date_time_add_days(start, 1) : NULL);
  bool ok = start && end && g_date_time_compare(end, start) > 0;
  gint64 span = ok ? g_date_time_difference(end, start) : 0;
  GDateTime *a = start
                     ? g_date_time_new_utc(g_date_time_get_year(start),
                                           g_date_time_get_month(start),
                                           g_date_time_get_day_of_month(start),
                                           0, 0, 0)
                     : NULL,
            *b = end ? g_date_time_new_utc(g_date_time_get_year(end),
                                           g_date_time_get_month(end),
                                           g_date_time_get_day_of_month(end), 0,
                                           0, 0)
                     : NULL;
  gint64 days = a && b ? g_date_time_difference(b, a) / G_TIME_SPAN_DAY : 0;
  if (a)
    g_date_time_unref(a);
  if (b)
    g_date_time_unref(b);
  if (range && days > 90)
    ok = false;
  if ((!range && (span <= 0 || span > 36 * G_TIME_SPAN_HOUR)) ||
      (range && (span > 91 * G_TIME_SPAN_DAY)))
    ok = false;
  GByteArray *raw = g_byte_array_new();
  if (ok) {
    unsigned char buf[8192];
    size_t n;
    while (
        (n = fread(buf, 1, MIN(sizeof buf, MAX_BYTES + 1 - raw->len), stdin))) {
      g_byte_array_append(raw, buf, (guint)n);
      if (raw->len > MAX_BYTES)
        break;
    }
    ok = !ferror(stdin) && raw->len <= MAX_BYTES;
  }
  GBytes *bytes = g_byte_array_free_to_bytes(raw);
  yyjson_mut_doc *doc =
      ok ? parse_calendar(bytes, start, end, z, argv[range ? 2 : 1], range)
         : NULL;
  if (doc) {
    char *json = json_string(doc, true);
    if (!json)
      ok = false;
    else {
      fputs(json, stdout);
      g_free(json);
    }
    yyjson_mut_doc_free(doc);
  } else
    ok = false;
  g_bytes_unref(bytes);
  if (start)
    g_date_time_unref(start);
  if (end)
    g_date_time_unref(end);
  g_time_zone_unref(z);
  return ok ? 0 : 1;
}
