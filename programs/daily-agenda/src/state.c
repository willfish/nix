#include "agenda.h"
#include <string.h>
static char *visible_input(const char *raw, size_t len) {
  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < len; i++) {
    if (!raw[i])
      g_string_append(s, "\356\200\200");
    else
      g_string_append_c(s, raw[i]);
  }
  return g_string_free(s, false);
}
static GPtrArray *split_lines(const char *s) {
  GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
  const char *start = s, *p = s;
  while (*p) {
    gunichar c = g_utf8_get_char(p);
    if (c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
        (c >= 0x1c && c <= 0x1e) || c == 0x85 || c == 0x2028 || c == 0x2029) {
      g_ptr_array_add(lines, g_strndup(start, (size_t)(p - start)));
      p = g_utf8_next_char(p);
      if (c == '\r' && *p == '\n')
        p++;
      start = p;
    } else
      p = g_utf8_next_char(p);
  }
  if (*start)
    g_ptr_array_add(lines, g_strdup(start));
  return lines;
}
GPtrArray *reminders(const char *path, const char **warning) {
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    *warning = "Today's notes file is missing.";
    return rows;
  }
  GBytes *bytes = NULL;
  if (!read_bytes(path, 4ULL * (MAX_BYTES + 1), &bytes)) {
    g_ptr_array_free(rows, true);
    return NULL;
  }
  gsize len;
  const char *raw = g_bytes_get_data(bytes, &len);
  if (!g_utf8_validate(raw, (gssize)len, NULL) && !memchr(raw, 0, len)) {
    g_bytes_unref(bytes);
    g_ptr_array_free(rows, true);
    bad("invalid notes text");
    return NULL;
  }
  char *value = visible_input(raw, len);
  g_bytes_unref(bytes);
  if (!g_utf8_validate(value, -1, NULL) ||
      g_utf8_strlen(value, -1) > MAX_BYTES) {
    g_free(value);
    g_ptr_array_free(rows, true);
    bad("notes too large");
    return NULL;
  }
  GPtrArray *lines = split_lines(value);
  g_free(value);
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  GRegex *bullet = g_regex_new(
             "\\A[\\s\\x{1c}-\\x{1f}]*[-*+][\\s\\x{1c}-\\x{1f}]+(.*)\\z",
             G_REGEX_DOTALL, 0, NULL),
         *checkbox = g_regex_new("\\A\\[([ xX])\\][\\s\\x{1c}-\\x{1f}]*(.*)\\z",
                                 G_REGEX_DOTALL, 0, NULL);
  bool section = false, fence = false;
  for (size_t i = 0; i < lines->len; i++) {
    const char *line = lines->pdata[i];
    char *trimmed = strip(line);
    if (g_str_has_prefix(trimmed, "```") || g_str_has_prefix(trimmed, "~~~")) {
      fence = !fence;
      g_free(trimmed);
      continue;
    }
    if (fence) {
      g_free(trimmed);
      continue;
    }
    GMatchInfo *match = NULL, *box = NULL;
    bool is_bullet = g_regex_match(bullet, line, 0, &match);
    char *body = is_bullet ? g_match_info_fetch(match, 1) : NULL;
    bool is_box = body && g_regex_match(checkbox, body, 0, &box);
    char *row = NULL;
    if (is_box) {
      char *mark = g_match_info_fetch(box, 1),
           *content = g_match_info_fetch(box, 2), *nonempty = strip(content);
      if (*mark == ' ' && *nonempty)
        row = clean(content, 2000);
      g_free(mark);
      g_free(content);
      g_free(nonempty);
    } else if (is_bullet && section)
      row = clean(body, 2000);
    else if (*trimmed) {
      char *p = trimmed;
      while (*p == '#')
        p++;
      char *heading = strip(p);
      p = heading;
      while (*p == '*' || *p == ':')
        p++;
      char *last = p + strlen(p);
      while (last > p && (last[-1] == '*' || last[-1] == ':'))
        *--last = 0;
      char *name = g_utf8_casefold(p, -1);
      section = !strcmp(name, "reminders") ||
                !strcmp(name, "what i plan to do today") ||
                !strcmp(name, "today") || !strcmp(name, "todos") ||
                !strcmp(name, "to do");
      g_free(name);
      g_free(heading);
    }
    if (row) {
      if (!g_hash_table_contains(seen, row)) {
        g_ptr_array_add(rows, row);
        g_hash_table_add(seen, row);
      } else
        g_free(row);
    }
    g_free(body);
    if (match)
      g_match_info_free(match);
    if (box)
      g_match_info_free(box);
    g_free(trimmed);
  }
  g_regex_unref(bullet);
  g_regex_unref(checkbox);
  g_hash_table_destroy(seen);
  g_ptr_array_free(lines, true);
  return rows;
}
yyjson_mut_doc *cached_events(const char *path, GDateTime *now, GTimeZone *tz,
                              char **status) {
  yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *array = yyjson_mut_arr(out);
  yyjson_mut_doc_set_root(out, array);
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    *status = g_strdup("Calendar not cached yet.");
    return out;
  }
  yyjson_doc *input = load_json(path);
  if (!input) {
    yyjson_mut_doc_free(out);
    return NULL;
  }
  yyjson_val *root = yyjson_doc_get_root(input);
  char *day = iso_day(now);
  const char *saved = text(field(root, "day"));
  if (!yyjson_is_obj(root) || !saved || strcmp(saved, day)) {
    g_free(day);
    *status = g_strdup("No calendar cache for today.");
    yyjson_doc_free(input);
    return out;
  }
  g_free(day);
  GDateTime *fetched = parse_stamp(text(field(root, "fetched_at")));
  yyjson_val *events = field(root, "events");
  bool ok = fetched && yyjson_is_arr(events) &&
            yyjson_arr_size(events) <= MAX_OCCURRENCES;
  if (ok) {
    yyjson_val *event;
    size_t i, max;
    yyjson_arr_foreach(events, i, max, event) {
      yyjson_val *summary = field(event, "summary");
      if (!yyjson_is_obj(event) ||
          (summary &&
           (!yyjson_is_str(summary) || string_length(summary) > 4000))) {
        ok = false;
        break;
      }
      yyjson_mut_val *value = normalize(out, event, tz, NULL);
      if (!value) {
        ok = false;
        break;
      }
      yyjson_mut_arr_add_val(array, value);
    }
  }
  if (ok) {
    GDateTime *local = g_date_time_to_timezone(fetched, tz);
    char *when = g_date_time_format(local, "%H:%M %Z");
    gint64 age = g_date_time_difference(now, fetched);
    *status = g_strconcat(age > 1800 * G_TIME_SPAN_SECOND || age < 0
                              ? "Calendar cache is stale. "
                              : "",
                          "Last refreshed ", when, ".", NULL);
    g_free(when);
    g_date_time_unref(local);
  }
  if (fetched)
    g_date_time_unref(fetched);
  yyjson_doc_free(input);
  if (!ok) {
    yyjson_mut_doc_free(out);
    bad("invalid cache");
    return NULL;
  }
  return out;
}
#define WORD "[\\p{L}\\p{N}_]"
#define LETTER "[\\p{L}\\p{Nl}\\p{No}_]"
#define PUNCT "[\\p{L}\\p{N}_!\"'&.,?]"
void print_wrapped(const char *s, bool pretty) {
  const char *pattern =
      "( +|(?<=" PUNCT ")-{2,}(?=" WORD ")|[^ ]+?(?:-(?:(?<=" LETTER
      "{2}-)|(?<=" LETTER "-" LETTER "-))(?=" LETTER "-?" LETTER
      ")|(?= |\\z)|(?<=" PUNCT ")(?=-{2,}" WORD ")))";
  GRegex *regex = g_regex_new(pattern, 0, 0, NULL);
  char **split = g_regex_split(regex, s, 0);
  GPtrArray *chunks = g_ptr_array_new_with_free_func(g_free);
  for (size_t i = 0; split[i]; i++)
    if (*split[i])
      g_ptr_array_add(chunks, g_strdup(split[i]));
  g_strfreev(split);
  g_regex_unref(regex);
  size_t index = 0, lines = 0;
  while (index < chunks->len) {
    const char *indent = lines ? "    " : (pretty ? "  ◦ " : "  [Notes] ");
    size_t width = 70 - (size_t)g_utf8_strlen(indent, -1), used = 0;
    GPtrArray *line = g_ptr_array_new_with_free_func(g_free);
    char *trimmed = strip(chunks->pdata[index]);
    if (lines && !*trimmed)
      index++;
    g_free(trimmed);
    while (index < chunks->len) {
      char *chunk = chunks->pdata[index];
      size_t length = (size_t)g_utf8_strlen(chunk, -1);
      if (used + length > width)
        break;
      g_ptr_array_add(line, g_strdup(chunk));
      used += length;
      index++;
    }
    if (index < chunks->len &&
        (size_t)g_utf8_strlen(chunks->pdata[index], -1) > width &&
        used < width) {
      char *chunk = chunks->pdata[index],
           *end = g_utf8_offset_to_pointer(chunk, (glong)(width - used)),
           *hyphen = NULL;
      for (char *p = chunk; p < end; p = g_utf8_next_char(p))
        if (*p == '-')
          hyphen = p;
      if (hyphen && hyphen > chunk) {
        bool not_hyphen = false;
        for (char *p = chunk; p < hyphen; p++)
          if (*p != '-')
            not_hyphen = true;
        if (not_hyphen)
          end = hyphen + 1;
      }
      g_ptr_array_add(line, g_strndup(chunk, (size_t)(end - chunk)));
      char *remaining = g_strdup(end);
      g_free(chunks->pdata[index]);
      chunks->pdata[index] = remaining;
    }
    if (line->len) {
      trimmed = strip(line->pdata[line->len - 1]);
      if (!*trimmed)
        g_ptr_array_remove_index(line, line->len - 1);
      g_free(trimmed);
    }
    if (line->len) {
      fputs(indent, stdout);
      for (size_t i = 0; i < line->len; i++)
        fputs(line->pdata[i], stdout);
      putchar('\n');
      lines++;
    }
    g_ptr_array_free(line, true);
  }
  if (!lines)
    putchar('\n');
  g_ptr_array_free(chunks, true);
}
