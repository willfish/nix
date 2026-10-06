#pragma once
#include <gio/gio.h>
#include <libical/ical.h>
#include <stdbool.h>
#include <yyjson.h>
#define MAX_BYTES 2000000
#define MAX_COMPONENTS 10000
#define MAX_OCCURRENCES 500
#define BAR_MAX_EVENTS 1500
#define PARSE_TIMEOUT 60
#define FETCH_TIMEOUT 20
extern const char *agenda_error;
bool bad(const char *message);
yyjson_val *field(yyjson_val *object, const char *key);
const char *text(yyjson_val *value);
bool truth(yyjson_val *value);
GPtrArray *keys(yyjson_val *object);
char *clean(const char *text, size_t limit);
char *clean_value(yyjson_val *value, size_t limit);
size_t string_length(yyjson_val *value);
char *strip(const char *text);
char *redact(const char *text, GPtrArray *secrets, size_t limit);
char *path_home(void);
char *env_path(const char *variable, const char *suffix);
bool read_bytes(const char *path, size_t limit, GBytes **bytes);
yyjson_doc *load_json(const char *path);
char *json_string(yyjson_mut_doc *doc, bool ascii);
bool save_cache(const char *path, yyjson_mut_doc *doc);
GTimeZone *zone(const char *name);
GTimeZone *local_zone(char **name);
GDateTime *parse_stamp(const char *value);
GDateTime *parse_day(const char *value, GTimeZone *zone);
char *iso_stamp(GDateTime *time);
char *iso_day(GDateTime *time);
char *event_time(yyjson_val *event, GTimeZone *zone);
yyjson_mut_val *normalize(yyjson_mut_doc *doc, yyjson_val *event,
                          GTimeZone *zone, GPtrArray *secrets);
char *safe_link(const char *value);
yyjson_mut_doc *parse_calendar(GBytes *raw, GDateTime *start, GDateTime *end,
                               GTimeZone *host, const char *zone_name,
                               bool records);
int parse_cli(int argc, char **argv, bool range);
yyjson_mut_doc *expand(GBytes *raw, const char *program, GDateTime *start,
                       GDateTime *end, const char *zone_name, bool range);
yyjson_mut_doc *feeds(const char *path);
bool feed_url(const char *value, char **token);
GPtrArray *feed_secrets(const char *url);
bool open_feed(const char *url, GBytes **bytes);
yyjson_mut_doc *fetch_calendars(yyjson_mut_doc *feeds, GDateTime *day,
                                GTimeZone *zone, const char *zone_name,
                                const char *program);
bool export_bar(yyjson_mut_doc *feeds, GDateTime *now, GTimeZone *zone,
                const char *zone_name, const char *program);
yyjson_mut_doc *bar_rows(yyjson_val *records, const char *name, GTimeZone *zone,
                         GPtrArray *secrets);
GPtrArray *reminders(const char *path, const char **warning);
yyjson_mut_doc *cached_events(const char *path, GDateTime *now, GTimeZone *zone,
                              char **status);
void print_wrapped(const char *text, bool pretty);
int agenda_main(int argc, char **argv);
int agenda_run(int argc, char **argv, const char *child_program);
