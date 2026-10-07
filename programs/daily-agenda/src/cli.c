#include "agenda.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
static char *credentials_path(void) {
  const char *direct = g_getenv("DAILY_CALENDAR_CREDENTIALS");
  if (direct)
    return g_strdup(*direct ? direct : ".");
  const char *sops = g_getenv("SOPS_NIX_SECRETS_DIR");
  if (sops)
    return g_build_filename(sops, "GOOGLE_CALENDAR_ICAL", NULL);
  char *config = env_path("XDG_CONFIG_HOME", ".config"),
       *path = g_build_filename(config, "sops-nix/secrets/GOOGLE_CALENDAR_ICAL",
                                NULL);
  g_free(config);
  return path;
}
static void usage(FILE *out) {
  fputs("usage: daily-agenda [-h] [--refresh] [--status]\n\nRead-only notes "
        "and a private Google Calendar iCal feed.\n",
        out);
}
static char *resolve_program(const char *name) {
#ifdef __linux__
  char *actual = g_file_read_link("/proc/self/exe", NULL);
  if (actual)
    return actual;
#endif
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(NULL, &size);
  char *actual = g_malloc(size);
  if (!_NSGetExecutablePath(actual, &size))
    return actual;
  g_free(actual);
#endif
  char *found = g_find_program_in_path(name);
  return found ? found : g_canonicalize_filename(name, NULL);
}
int agenda_main(int argc, char **argv) { return agenda_run(argc, argv, NULL); }
int agenda_run(int argc, char **argv, const char *child_program) {
  if (argc > 1 && !strcmp(argv[1], "--parse-feed"))
    return parse_cli(argc - 2, argv + 2, false);
  if (argc > 1 && !strcmp(argv[1], "--parse-range"))
    return parse_cli(argc - 2, argv + 2, true);
  bool refresh = false, status_only = false, stopped = false;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (!stopped && !strcmp(arg, "--")) {
      stopped = true;
      continue;
    }
    if (!stopped &&
        (!strcmp(arg, "-h") || !strcmp(arg, "--help") || !strcmp(arg, "--he") ||
         !strcmp(arg, "--hel") || !strcmp(arg, "--h"))) {
      usage(stdout);
      return 0;
    }
    size_t len = strlen(arg);
    if (!stopped && len >= 3 && len <= 9 && !strncmp(arg, "--refresh", len))
      refresh = true;
    else if (!stopped && len >= 3 && len <= 8 && !strncmp(arg, "--status", len))
      status_only = true;
    else {
      usage(stderr);
      return 2;
    }
  }
  char *zone_name = NULL;
  GTimeZone *tz = local_zone(&zone_name);
  if (!tz) {
    g_free(zone_name);
    return 1;
  }
  GDateTime *now = g_date_time_new_now(tz);
  char *day = iso_day(now), *cache_root = env_path("XDG_CACHE_HOME", ".cache"),
       *cache =
           g_build_filename(cache_root, "daily-agenda/calendar.json", NULL),
       *secret = credentials_path();
  bool configured = g_file_test(secret, G_FILE_TEST_EXISTS);
  int result = 0;
  if (status_only) {
    if (!configured)
      puts("Calendar feed: missing");
    else {
      yyjson_mut_doc *fs = feeds(secret);
      if (!fs)
        puts("Calendar feed: unusable");
      else {
        puts("Calendar feed: present");
        fputs("Calendars: ", stdout);
        yyjson_mut_val *k, *v;
        yyjson_mut_obj_iter iter =
            yyjson_mut_obj_iter_with(yyjson_mut_doc_get_root(fs));
        bool first = true;
        while ((k = yyjson_mut_obj_iter_next(&iter))) {
          v = yyjson_mut_obj_iter_get_val(k);
          (void)v;
          if (!first)
            fputs(", ", stdout);
          fputs(yyjson_mut_get_str(k), stdout);
          first = false;
        }
        putchar('\n');
        yyjson_mut_doc_free(fs);
      }
    }
    char *status = NULL;
    yyjson_mut_doc *saved = cached_events(cache, now, tz, &status);
    if (saved) {
      puts(status);
      yyjson_mut_doc_free(saved);
      g_free(status);
    } else
      puts("Calendar cache unavailable or invalid.");
    goto done;
  }
  GPtrArray *warnings = g_ptr_array_new();
  if (refresh) {
    yyjson_mut_doc *fs = feeds(secret);
    char *program =
        child_program ? g_strdup(child_program) : resolve_program(argv[0]);
    GDateTime *midnight = parse_day(day, tz);
    yyjson_mut_doc *events =
        fs ? fetch_calendars(fs, midnight, tz, zone_name, program) : NULL;
    bool ok = events != NULL;
    if (ok) {
      yyjson_mut_doc *saved = yyjson_mut_doc_new(NULL);
      yyjson_mut_val *root = yyjson_mut_obj(saved);
      yyjson_mut_doc_set_root(saved, root);
      yyjson_mut_obj_add_strcpy(saved, root, "day", day);
      char *stamp = iso_stamp(now);
      yyjson_mut_obj_add_strcpy(saved, root, "fetched_at", stamp);
      g_free(stamp);
      yyjson_mut_obj_add_val(
          saved, root, "events",
          yyjson_mut_val_mut_copy(saved, yyjson_mut_doc_get_root(events)));
      ok = save_cache(cache, saved);
      yyjson_mut_doc_free(saved);
      if (ok && !export_bar(fs, now, tz, zone_name, program))
        fputs("Warning: Calendar bar export failed; the previous rail calendar "
              "was kept.\n",
              stderr);
    }
    if (!ok)
      g_ptr_array_add(warnings,
                      "Calendar refresh failed; check access, credentials and "
                      "network. Using cache if available.");
    if (events)
      yyjson_mut_doc_free(events);
    if (fs)
      yyjson_mut_doc_free(fs);
    g_free(program);
    g_date_time_unref(midnight);
  }
  if (!configured)
    g_ptr_array_add(warnings, "Google Calendar access is not configured.");
  bool pretty = isatty(STDOUT_FILENO);
  if (pretty) {
    char *date = g_date_time_format(now, "%A, %-d %B");
    printf("\n  \033[1;36mTODAY\033[0m  %s\n\n", date);
    g_free(date);
    puts("  \033[1mREMINDERS\033[0m\n");
  } else {
    printf("Today's agenda: %s\n", day);
    puts("\nNotes");
  }
  char *home = path_home(),
       *notes = g_build_filename(home, "Notes", day, "today.md", NULL);
  const char *warning = NULL;
  GPtrArray *rows = reminders(notes, &warning);
  if (rows) {
    if (warning)
      g_ptr_array_add(warnings, (void *)warning);
    for (size_t i = 0; i < rows->len; i++)
      print_wrapped(rows->pdata[i], pretty);
    if (!rows->len)
      puts(pretty ? "  Nothing on your list."
                  : "  No unfinished reminders found.");
    g_ptr_array_free(rows, true);
  } else
    g_ptr_array_add(warnings, "Today's notes could not be read.");
  g_free(home);
  g_free(notes);
  puts(pretty ? "\n  \033[1mSCHEDULE\033[0m\n" : "\nGoogle Calendar");
  char *status = NULL;
  yyjson_mut_doc *saved = cached_events(cache, now, tz, &status);
  if (saved) {
    if (!pretty)
      printf("  %s\n", status);
    yyjson_doc *input = yyjson_mut_doc_imut_copy(saved, NULL);
    yyjson_val *events = yyjson_doc_get_root(input), *event;
    size_t i, max;
    yyjson_arr_foreach(events, i, max, event) {
      char *time = event_time(event, tz),
           *summary = clean(text(field(event, "summary")), 2000);
      const char *calendar = text(field(event, "calendar"));
      if (pretty)
        printf("  \033[36m%s\033[0m  %s", time, summary);
      else
        printf("  [Calendar] %s %s", time, summary);
      if (calendar && *calendar)
        printf("  · %s", calendar);
      putchar('\n');
      char *link = safe_link(text(field(event, "htmlLink")));
      if (*link && !pretty)
        printf("    %s\n", link);
      g_free(time);
      g_free(summary);
      g_free(link);
    }
    if (!yyjson_arr_size(events) && g_file_test(cache, G_FILE_TEST_EXISTS) &&
        g_str_has_prefix(status, "Last refreshed"))
      puts("  No events today.");
    yyjson_doc_free(input);
    yyjson_mut_doc_free(saved);
  } else
    g_ptr_array_add(warnings, "Calendar cache unavailable or invalid.");
  if (pretty) {
    bool calendar_warning = false;
    for (size_t i = 0; i < warnings->len; i++) {
      const char *w = warnings->pdata[i];
      if (strstr(w, "Calendar") || strstr(w, "calendar"))
        calendar_warning = true;
    }
    printf("\n  \033[2m%s\033[0m\n",
           calendar_warning
               ? "Calendar temporarily unavailable · showing saved events"
               : status);
  } else
    for (size_t i = 0; i < warnings->len; i++)
      printf("\nWarning: %s\n", (const char *)warnings->pdata[i]);
  result = refresh && warnings->len ? 1 : 0;
  g_free(status);
  g_ptr_array_free(warnings, true);
done:
  g_date_time_unref(now);
  g_time_zone_unref(tz);
  g_free(zone_name);
  g_free(day);
  g_free(cache_root);
  g_free(cache);
  g_free(secret);
  return result;
}
