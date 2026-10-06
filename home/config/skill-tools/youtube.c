#include "common.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef yyjson_mut_val M;
typedef yyjson_mut_doc D;
typedef struct {
  double start, end;
  GString *text;
} Cue;
static void string_free(gpointer p) { g_string_free(p, TRUE); }
static void cue_free(gpointer p) {
  Cue *cue = p;
  string_free(cue->text);
  g_free(cue);
}
static GString *trimmed(const char *s, size_t n) {
  const char *end = s + n;
  while (s < end && unicode_space(g_utf8_get_char(s)))
    s = g_utf8_next_char(s);
  while (end > s) {
    const char *p = g_utf8_find_prev_char(s, end);
    if (!unicode_space(g_utf8_get_char(p)))
      break;
    end = p;
  }
  return g_string_new_len(s, end - s);
}
static char *capture(const char *pattern, const char *s, int group) {
  GRegex *re = g_regex_new(pattern, 0, 0, NULL);
  GMatchInfo *match = NULL;
  g_regex_match(re, s, 0, &match);
  char *out =
      g_match_info_matches(match) ? g_match_info_fetch(match, group) : NULL;
  g_match_info_free(match);
  g_regex_unref(re);
  return out;
}
static bool offset(const char *s, double *value) {
  char *trim = strip(s), *ascii = decimal_text(trim);
  g_free(trim);
  bool ok = false;
  if (g_regex_match_simple("\\A[0-9]+(?:\\.[0-9]+)?\\z", ascii, 0, 0)) {
    *value = g_ascii_strtod(ascii, NULL);
    ok = true;
  } else {
    GRegex *re =
        g_regex_new("\\A(?:(\\d+)h)?(?:(\\d+)m)?(?:(\\d+(?:\\.\\d+)?)s)?\\z",
                    G_REGEX_CASELESS, 0, NULL);
    GMatchInfo *match = NULL;
    g_regex_match(re, ascii, 0, &match);
    if (*ascii && g_match_info_matches(match)) {
      *value = 0;
      const double scale[] = {3600, 60, 1};
      for (int i = 1; i <= 3; i++) {
        char *part = g_match_info_fetch(match, i);
        *value += g_ascii_strtod(part, NULL) * scale[i - 1];
        g_free(part);
      }
      ok = true;
    }
    g_match_info_free(match);
    g_regex_unref(re);
  }
  g_free(ascii);
  return ok;
}
static bool clock_value(GString *raw, double *value) {
  GString *token = trimmed(raw->str, raw->len);
  size_t length = 0;
  while (length < token->len &&
         !unicode_space(g_utf8_get_char(token->str + length)))
    length = (size_t)(g_utf8_next_char(token->str + length) - token->str);
  g_string_truncate(token, length);
  if (strlen(token->str) != token->len) {
    string_free(token);
    return false;
  }
  char *ascii = decimal_text(token->str);
  string_free(token);
  GRegex *re = g_regex_new(
      "\\A(?:(\\d{1,2}):)?(\\d{1,2}):(\\d{2})(?:[.,](\\d{1,3}))?\\z", 0, 0,
      NULL);
  GMatchInfo *match = NULL;
  g_regex_match(re, ascii, 0, &match);
  bool ok = g_match_info_matches(match);
  if (ok) {
    *value = 0;
    const double scale[] = {3600, 60, 1};
    for (int i = 1; i <= 3; i++) {
      char *part = g_match_info_fetch(match, i);
      *value += g_ascii_strtod(part, NULL) * scale[i - 1];
      g_free(part);
    }
    char *fraction = g_match_info_fetch(match, 4);
    if (*fraction)
      *value += g_ascii_strtod(fraction, NULL) / pow(10, strlen(fraction));
    g_free(fraction);
  }
  g_match_info_free(match);
  g_regex_unref(re);
  g_free(ascii);
  return ok;
}
static gssize arrow(GString *s) {
  for (size_t i = 0; i + 3 <= s->len; i++)
    if (!memcmp(s->str + i, "-->", 3))
      return (gssize)i;
  return -1;
}
static GString *clean_line(GString *line) {
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < line->len;) {
    if (line->str[i] == '<') {
      char *end = memchr(line->str + i + 1, '>', line->len - i - 1);
      if (end && end > line->str + i + 1) {
        i = (size_t)(end - line->str) + 1;
        continue;
      }
    }
    if (i + 6 <= line->len && !memcmp(line->str + i, "&nbsp;", 6)) {
      g_string_append_c(out, ' ');
      i += 6;
    } else
      g_string_append_c(out, line->str[i++]);
  }
  GString *trim = trimmed(out->str, out->len);
  string_free(out);
  return trim;
}
static GString *captions_file(const char *path) {
  char *data = NULL;
  gsize n;
  if (!g_file_get_contents(path, &data, &n, NULL))
    return NULL;
  GString *valid = g_string_new(NULL);
  for (size_t at = 0; at < n;) {
    char *end = memchr(data + at, 0, n - at);
    size_t size = end ? (size_t)(end - data - at) : n - at;
    char *part = g_utf8_make_valid(data + at, (gssize)size);
    g_string_append(valid, part);
    g_free(part);
    at += size;
    if (at < n) {
      g_string_append_c(valid, 0);
      at++;
    }
  }
  g_free(data);
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < valid->len; i++) {
    char c = valid->str[i];
    if (c == '\r') {
      c = '\n';
      if (i + 1 < valid->len && valid->str[i + 1] == '\n')
        i++;
    }
    g_string_append_c(out, c);
  }
  string_free(valid);
  return out;
}
static GPtrArray *parse_cues(GString *text) {
  GPtrArray *lines = g_ptr_array_new_with_free_func(string_free),
            *cues = g_ptr_array_new_with_free_func(cue_free);
  size_t start = 0;
  for (size_t i = 0; i <= text->len; i++)
    if (i == text->len || text->str[i] == '\n') {
      g_ptr_array_add(lines, trimmed(text->str + start, i - start));
      start = i + 1;
    }
  for (size_t index = 0; index < lines->len;) {
    GString *line = lines->pdata[index];
    if (!line->len ||
        g_regex_match_simple(
            "(*UCP)^(WEBVTT|NOTE|STYLE|Kind:|Language:|REGION)\\b", line->str,
            G_REGEX_CASELESS, 0)) {
      index++;
      continue;
    }
    gssize at = arrow(line);
    if (at < 0) {
      if (index + 1 < lines->len && arrow(lines->pdata[index + 1]) >= 0) {
        line = lines->pdata[++index];
        at = arrow(line);
      } else {
        index++;
        continue;
      }
    }
    GString *a = g_string_new_len(line->str, at),
            *b = g_string_new_len(line->str + at + 3,
                                  (gssize)line->len - at - 3);
    Cue *cue = g_new0(Cue, 1);
    bool ok = clock_value(a, &cue->start) && clock_value(b, &cue->end);
    string_free(a);
    string_free(b);
    if (!ok) {
      g_free(cue);
      g_ptr_array_free(lines, TRUE);
      g_ptr_array_free(cues, TRUE);
      return NULL;
    }
    cue->text = g_string_new(NULL);
    index++;
    while (index < lines->len && ((GString *)lines->pdata[index])->len) {
      GString *clean = clean_line(lines->pdata[index++]);
      if (clean->len) {
        if (cue->text->len)
          g_string_append_c(cue->text, ' ');
        g_string_append_len(cue->text, clean->str, (gssize)clean->len);
      }
      string_free(clean);
    }
    if (!cue->text->len) {
      cue_free(cue);
      continue;
    }
    Cue *previous = cues->len ? cues->pdata[cues->len - 1] : NULL;
    if (previous && !memcmp(previous->text->str, cue->text->str,
                            MIN(previous->text->len, cue->text->len))) {
      previous->end = fmax(previous->end, cue->end);
      if (cue->text->len > previous->text->len) {
        string_free(previous->text);
        previous->text = cue->text;
        cue->text = g_string_new(NULL);
      }
      cue_free(cue);
    } else
      g_ptr_array_add(cues, cue);
  }
  g_ptr_array_free(lines, TRUE);
  return cues;
}
static bool hit(Cue *cue, const char *query) {
  char *needle = g_utf8_casefold(query, -1);
  bool found = false;
  for (size_t at = 0; at < cue->text->len && !found;) {
    char *end = memchr(cue->text->str + at, 0, cue->text->len - at);
    size_t n = end ? (size_t)(end - cue->text->str - at) : cue->text->len - at;
    char *fold = g_utf8_casefold(cue->text->str + at, (gssize)n);
    found = strstr(fold, needle) != NULL;
    g_free(fold);
    at += n + 1;
  }
  g_free(needle);
  return found;
}
static bool in_window(Cue *cue, double around, double window) {
  return cue->end >= fmax(0, around - window) && cue->start <= around + window;
}
static char *clock_string(double value) {
  if (!isfinite(value))
    return NULL;
  value = fmax(0, trunc(value));
  double hours = floor(value / 3600), minutes = floor(fmod(value, 3600) / 60),
         seconds = fmod(value, 60);
  return hours > 0
             ? g_strdup_printf("%.0f:%02.0f:%02.0f", hours, minutes, seconds)
             : g_strdup_printf("%.0f:%02.0f", minutes, seconds);
}
static bool truth(Val *v) {
  if (!v || yyjson_is_null(v))
    return false;
  if (yyjson_is_bool(v))
    return yyjson_get_bool(v);
  if (yyjson_is_str(v))
    return yyjson_get_len(v) > 0;
  if (yyjson_is_arr(v))
    return yyjson_arr_size(v) > 0;
  if (yyjson_is_obj(v))
    return yyjson_obj_size(v) > 0;
  if (yyjson_is_num(v))
    return yyjson_get_num(v) != 0;
  return true;
}
static bool numeric(Val *v, double *out) {
  if (yyjson_is_num(v)) {
    *out = yyjson_get_num(v);
    return true;
  }
  if (yyjson_is_bool(v)) {
    *out = yyjson_get_bool(v) ? 1 : 0;
    return true;
  }
  return yyjson_is_str(v) && number_arg(yyjson_get_str(v), out);
}
static M *copy_default(D *d, Val *v, const char *fallback) {
  return truth(v) ? yyjson_val_mut_copy(d, v) : yyjson_mut_strcpy(d, fallback);
}
static D *metadata(Val *info, const char *url) {
  if (!yyjson_is_obj(info))
    return NULL;
  D *d = yyjson_mut_doc_new(NULL);
  M *root = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, root);
  Val *id = field(info, "id"), *duration = field(info, "duration");
  yyjson_mut_obj_add_val(d, root, "id",
                         id ? yyjson_val_mut_copy(d, id) : yyjson_mut_null(d));
  yyjson_mut_obj_add_val(d, root, "title",
                         copy_default(d, field(info, "title"), ""));
  Val *channel = field(info, "channel");
  if (!truth(channel))
    channel = field(info, "uploader");
  yyjson_mut_obj_add_val(d, root, "channel", copy_default(d, channel, ""));
  yyjson_mut_obj_add_val(d, root, "duration",
                         duration ? yyjson_val_mut_copy(d, duration)
                                  : yyjson_mut_null(d));
  Val *duration_string = field(info, "duration_string");
  char *clock = NULL;
  if (!truth(duration_string) && truth(duration)) {
    double n;
    if (!numeric(duration, &n) || (clock = clock_string(n)) == NULL)
      goto fail;
  }
  yyjson_mut_obj_add_val(d, root, "duration_string",
                         copy_default(d, duration_string, clock ? clock : ""));
  g_free(clock);
  clock = NULL;
  yyjson_mut_obj_add_val(d, root, "upload_date",
                         copy_default(d, field(info, "upload_date"), ""));
  yyjson_mut_obj_add_val(d, root, "url",
                         copy_default(d, field(info, "webpage_url"), url));
  yyjson_mut_obj_add_val(d, root, "description",
                         copy_default(d, field(info, "description"), ""));
  M *chapters = yyjson_mut_arr(d);
  Val *list = field(info, "chapters");
  if (truth(list) && !yyjson_is_arr(list))
    goto fail;
  Val *chapter;
  size_t i, n;
  yyjson_arr_foreach(list, i, n, chapter) {
    if (!yyjson_is_obj(chapter))
      goto fail;
    Val *start = field(chapter, "start_time");
    double seconds = 0;
    if (truth(start) && !numeric(start, &seconds))
      goto fail;
    clock = clock_string(seconds);
    if (!clock)
      goto fail;
    M *item = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, item, "start", seconds);
    yyjson_mut_obj_add_val(d, item, "title",
                           copy_default(d, field(chapter, "title"), ""));
    yyjson_mut_obj_add_strcpy(d, item, "clock", clock);
    g_free(clock);
    clock = NULL;
    yyjson_mut_arr_add_val(chapters, item);
  }
  yyjson_mut_obj_add_val(d, root, "chapters", chapters);
  return d;
fail:
  g_free(clock);
  yyjson_mut_doc_free(d);
  return NULL;
}
static void add_cue(D *d, M *array, Cue *cue) {
  M *item = yyjson_mut_obj(d);
  yyjson_mut_obj_add_real(d, item, "start", cue->start);
  yyjson_mut_obj_add_real(d, item, "end", cue->end);
  yyjson_mut_obj_add_strncpy(d, item, "text", cue->text->str, cue->text->len);
  yyjson_mut_arr_add_val(array, item);
}
static bool json_output(D *d, GPtrArray *cues, bool has_around, double around,
                        double window, const char *query) {
  M *root = yyjson_mut_doc_get_root(d), *selected = yyjson_mut_arr(d),
    *matches = yyjson_mut_arr(d);
  yyjson_mut_obj_add_val(d, root, "timestamp",
                         has_around ? yyjson_mut_real(d, around)
                                    : yyjson_mut_null(d));
  yyjson_mut_obj_add_real(d, root, "window", window);
  yyjson_mut_obj_add_val(d, root, "query",
                         query ? yyjson_mut_strcpy(d, query)
                               : yyjson_mut_null(d));
  yyjson_mut_obj_add_uint(d, root, "cue_count", cues->len);
  for (size_t i = 0; i < cues->len; i++) {
    Cue *cue = cues->pdata[i];
    if (has_around && in_window(cue, around, window))
      add_cue(d, selected, cue);
    if (query && *query && hit(cue, query))
      add_cue(d, matches, cue);
  }
  yyjson_mut_obj_add_val(d, root, "around", selected);
  yyjson_mut_obj_add_val(d, root, "matches", matches);
  char *json = yyjson_mut_write(
      d, YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_ALLOW_INF_AND_NAN, NULL);
  bool ok = json && puts(json) != EOF;
  free(json);
  return ok;
}
static void value_text(GString *out, M *v, const char *fallback) {
  if (yyjson_mut_is_str(v)) {
    if (yyjson_mut_get_len(v))
      g_string_append_len(out, yyjson_mut_get_str(v),
                          (gssize)yyjson_mut_get_len(v));
    else
      g_string_append(out, fallback);
  } else if (!v || yyjson_mut_is_null(v))
    g_string_append(out, fallback);
  else if (yyjson_mut_is_bool(v))
    g_string_append(out, yyjson_mut_get_bool(v) ? "True" : fallback);
  else {
    char *s = yyjson_mut_val_write(v, YYJSON_WRITE_ALLOW_INF_AND_NAN, NULL);
    if (s)
      g_string_append(out, s);
    free(s);
  }
}
static char *repr(const char *text) {
  char quote = strchr(text, '\'') && !strchr(text, '"') ? '"' : '\'';
  GString *out = g_string_new(NULL);
  g_string_append_c(out, quote);
  for (const char *p = text; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (c == (gunichar)quote || c == '\\') {
      g_string_append_c(out, '\\');
      g_string_append_unichar(out, c);
    } else if (c == '\n')
      g_string_append(out, "\\n");
    else if (c == '\r')
      g_string_append(out, "\\r");
    else if (c == '\t')
      g_string_append(out, "\\t");
    else if (!g_unichar_isprint(c) || (unicode_space(c) && c != ' ')) {
      if (c <= 255)
        g_string_append_printf(out, "\\x%02x", c);
      else if (c <= 65535)
        g_string_append_printf(out, "\\u%04x", c);
      else
        g_string_append_printf(out, "\\U%08x", c);
    } else
      g_string_append_unichar(out, c);
  }
  g_string_append_c(out, quote);
  return g_string_free(out, FALSE);
}
static void cue_text(GString *out, Cue *cue) {
  char *clock = clock_string(cue->start);
  g_string_append_printf(out, "[%s] ", clock);
  g_string_append_len(out, cue->text->str, (gssize)cue->text->len);
  g_string_append_c(out, '\n');
  g_free(clock);
}
static bool render(D *d, GPtrArray *cues, bool has_around, double around,
                   double window, const char *query) {
  M *meta = yyjson_mut_doc_get_root(d);
  GString *out = g_string_new(NULL);
  const char *keys[] = {"title", "channel", "duration_string", "url"},
             *labels[] = {"Title: ", "Channel: ", "Duration: ", "URL: "};
  for (size_t i = 0; i < 4; i++) {
    g_string_append(out, labels[i]);
    value_text(out, yyjson_mut_obj_get(meta, keys[i]),
               i == 3 ? "" : "(unknown)");
    g_string_append_c(out, '\n');
  }
  char *clock = has_around ? clock_string(around) : NULL;
  if (has_around && !clock) {
    string_free(out);
    return false;
  }
  if (clock)
    g_string_append_printf(out, "Timestamp: %s (%.0fs)\n", clock, around);
  g_string_append_c(out, '\n');
  M *description = yyjson_mut_obj_get(meta, "description");
  if (!yyjson_mut_is_str(description)) {
    g_free(clock);
    string_free(out);
    return false;
  }
  GString *desc =
      trimmed(yyjson_mut_get_str(description), yyjson_mut_get_len(description));
  if (desc->len) {
    g_string_append(out, "Description:\n");
    g_string_append_len(out, desc->str, (gssize)desc->len);
    g_string_append(out, "\n\n");
  }
  string_free(desc);
  M *chapters = yyjson_mut_obj_get(meta, "chapters");
  if (yyjson_mut_arr_size(chapters)) {
    g_string_append(out, "Chapters:\n");
    M *chapter;
    size_t i, n;
    yyjson_mut_arr_foreach(chapters, i, n, chapter) {
      g_string_append_printf(
          out, "- %s ",
          yyjson_mut_get_str(yyjson_mut_obj_get(chapter, "clock")));
      value_text(out, yyjson_mut_obj_get(chapter, "title"), "");
      g_string_append_c(out, '\n');
    }
    g_string_append_c(out, '\n');
  }
  if (has_around && cues->len) {
    g_string_append_printf(out, "Captions around %s (±%.0fs):\n", clock,
                           window);
    for (size_t i = 0; i < cues->len; i++)
      if (in_window(cues->pdata[i], around, window))
        cue_text(out, cues->pdata[i]);
    g_string_append_c(out, '\n');
  }
  g_free(clock);
  if (query && *query) {
    size_t hits = 0;
    for (size_t i = 0; i < cues->len; i++)
      if (hit(cues->pdata[i], query))
        hits++;
    char *quoted = repr(query);
    g_string_append_printf(out, "Query matches (%s): %zu\n", quoted, hits);
    g_free(quoted);
    for (size_t i = 0; i < cues->len; i++)
      if (hit(cues->pdata[i], query))
        cue_text(out, cues->pdata[i]);
    g_string_append_c(out, '\n');
  }
  if (!has_around && (!query || !*query)) {
    if (cues->len) {
      g_string_append_printf(
          out,
          "Captions: %u cues. Pass --around or --query; do not dump the full "
          "transcript into chat.\nSample:\n",
          cues->len);
      for (size_t i = 0; i < MIN(12u, cues->len); i++)
        cue_text(out, cues->pdata[i]);
    } else
      g_string_append(out, "Captions: none\n");
  }
  while (out->len) {
    const char *last = g_utf8_find_prev_char(out->str, out->str + out->len);
    if (!unicode_space(g_utf8_get_char(last)))
      break;
    g_string_truncate(out, (size_t)(last - out->str));
  }
  g_string_append_c(out, '\n');
  bool ok = fwrite(out->str, 1, out->len, stdout) == out->len;
  string_free(out);
  return ok;
}
static gint file_order(gconstpointer a, gconstpointer b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static bool fetch(const char *url, const char *dir, yyjson_doc **info,
                  GString **subs) {
  char *command = g_find_program_in_path("yt-dlp"),
       *nix = command ? NULL : g_find_program_in_path("nix");
  if (!command && !nix) {
    fputs("yt-dlp is not installed and nix is unavailable\n", stderr);
    return false;
  }
  GPtrArray *args = g_ptr_array_new();
  if (command)
    g_ptr_array_add(args, command);
  else {
    const char *prefix[] = {"nix", "shell", "nixpkgs#yt-dlp", "-c", "yt-dlp"};
    for (size_t i = 0; i < 5; i++)
      g_ptr_array_add(args, (gpointer)prefix[i]);
  }
  const char *flags[] = {"--skip-download",
                         "--no-playlist",
                         "--no-warnings",
                         "--write-info-json",
                         "--write-subs",
                         "--write-auto-subs",
                         "--sub-langs",
                         "en.*,en",
                         "--sub-format",
                         "vtt/best",
                         "-o"};
  for (size_t i = 0; i < G_N_ELEMENTS(flags); i++)
    g_ptr_array_add(args, (gpointer)flags[i]);
  char *output = g_build_filename(dir, "%(id)s", NULL);
  g_ptr_array_add(args, output);
  g_ptr_array_add(args, (gpointer)url);
  g_ptr_array_add(args, NULL);
  GSubprocess *child = g_subprocess_newv(
      (const char *const *)args->pdata, G_SUBPROCESS_FLAGS_STDIN_INHERIT, NULL);
  bool ok = child && g_subprocess_wait_check(child, NULL, NULL);
  if (child)
    g_object_unref(child);
  g_ptr_array_free(args, TRUE);
  g_free(command);
  g_free(nix);
  g_free(output);
  if (!ok)
    return false;
  GDir *directory = g_dir_open(dir, 0, NULL);
  if (!directory)
    return false;
  GPtrArray *vtt = g_ptr_array_new_with_free_func(g_free),
            *srt = g_ptr_array_new_with_free_func(g_free);
  const char *name;
  bool found_info = false;
  while ((name = g_dir_read_name(directory))) {
    char *path = g_build_filename(dir, name, NULL);
    if (g_str_has_suffix(name, ".info.json") && !found_info) {
      found_info = true;
      *info = load_json(path);
    }
    if (g_str_has_suffix(name, ".vtt"))
      g_ptr_array_add(vtt, g_strdup(path));
    if (g_str_has_suffix(name, ".srt"))
      g_ptr_array_add(srt, g_strdup(path));
    g_free(path);
  }
  g_dir_close(directory);
  g_ptr_array_sort(vtt, file_order);
  g_ptr_array_sort(srt, file_order);
  const char *selected = NULL;
  for (size_t pass = 0; pass < 2 && !selected; pass++)
    for (size_t ext = 0; ext < 2; ext++) {
      GPtrArray *files = ext ? srt : vtt;
      for (size_t i = 0; i < files->len && !selected; i++) {
        const char *path = files->pdata[i];
        char *base = g_path_get_basename(path),
             *lower = g_ascii_strdown(base, -1);
        if (pass || !strstr(lower, "auto"))
          selected = path;
        g_free(base);
        g_free(lower);
      }
    }
  if (selected)
    *subs = captions_file(selected);
  ok = *info && (!selected || *subs);
  g_ptr_array_free(vtt, TRUE);
  g_ptr_array_free(srt, TRUE);
  return ok;
}
static void remove_temp(const char *path) {
  GFile *file = g_file_new_for_path(path);
  if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) ==
      G_FILE_TYPE_DIRECTORY) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
      char *child = g_build_filename(path, name, NULL);
      remove_temp(child);
      g_free(child);
    }
    if (dir)
      g_dir_close(dir);
  }
  g_file_delete(file, NULL, NULL);
  g_object_unref(file);
}
int youtube_main(int argc, char **argv) {
  const struct option options[] = {{"url", required_argument, NULL, 'u'},
                                   {"around", required_argument, NULL, 'a'},
                                   {"window", required_argument, NULL, 'w'},
                                   {"query", required_argument, NULL, 'q'},
                                   {"subs-file", required_argument, NULL, 's'},
                                   {"info-file", required_argument, NULL, 'i'},
                                   {"json", no_argument, NULL, 'j'},
                                   {"workdir", required_argument, NULL, 'd'},
                                   {"help", no_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
  const char *url_flag = NULL, *around_arg = NULL, *query = NULL,
             *subs_file = NULL, *info_file = NULL, *workdir = NULL;
  double window = 90, around = 0;
  bool as_json = false;
  int opt;
  opterr = 0;
  while ((opt = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    switch (opt) {
    case 'u':
      url_flag = optarg;
      break;
    case 'a':
      around_arg = optarg;
      break;
    case 'w':
      if (!number_arg(optarg, &window)) {
        fputs("invalid window\n", stderr);
        return 2;
      }
      break;
    case 'q':
      query = optarg;
      break;
    case 's':
      subs_file = optarg;
      break;
    case 'i':
      info_file = optarg;
      break;
    case 'j':
      as_json = true;
      break;
    case 'd':
      workdir = *optarg ? optarg : ".";
      break;
    case 'h':
      puts("usage: youtube-extract [URL] [--url URL] [--around TIME] [--window "
           "SECONDS]\n       [--query TEXT] [--subs-file FILE] [--info-file "
           "FILE] [--json] [--workdir DIR]");
      return 0;
    default:
      return 2;
    }
  }
  if (argc - optind > 1)
    return 2;
  const char *url = url_flag && *url_flag ? url_flag
                    : optind < argc       ? argv[optind]
                                          : "";
  if (!*url && !subs_file && !info_file) {
    fputs("a YouTube URL is required unless fixtures are supplied\n", stderr);
    return 1;
  }
  char *time = capture("[?&#]t=([0-9hmsHMS]+)", url, 1);
  bool has_around = (around_arg && *around_arg) || time;
  bool valid = !time || offset(time, &around);
  if (valid && around_arg && *around_arg)
    valid = offset(around_arg, &around);
  g_free(time);
  if (!valid) {
    fputs("invalid time offset\n", stderr);
    return 1;
  }
  yyjson_doc *info = NULL;
  GString *subs = NULL;
  bool ok = true;
  if (info_file || subs_file) {
    info = info_file ? load_json(info_file) : yyjson_read("{}", 2, 0);
    if (subs_file)
      subs = captions_file(subs_file);
    ok = info && (!subs_file || subs);
  } else {
    char *temporary =
        workdir ? NULL : g_dir_make_tmp("youtube-extract-XXXXXX", NULL);
    const char *dir = workdir ? workdir : temporary;
    ok = dir && fetch(url, dir, &info, &subs);
    if (temporary) {
      remove_temp(temporary);
      g_free(temporary);
    }
  }
  GPtrArray *cues = NULL;
  D *meta = NULL;
  if (ok) {
    if (!subs)
      subs = g_string_new(NULL);
    cues = parse_cues(subs);
    meta = metadata(yyjson_doc_get_root(info), url);
    ok = cues && meta;
  }
  if (ok) {
    if (as_json)
      ok = json_output(meta, cues, has_around, around, window, query);
    else
      ok = render(meta, cues, has_around, around, window, query);
  }
  if (meta)
    yyjson_mut_doc_free(meta);
  if (cues)
    g_ptr_array_free(cues, TRUE);
  if (subs)
    string_free(subs);
  if (info)
    yyjson_doc_free(info);
  if (!ok)
    fputs("YouTube extraction failed.\n", stderr);
  return ok ? 0 : 1;
}
int main(int argc, char **argv) { return youtube_main(argc, argv); }
