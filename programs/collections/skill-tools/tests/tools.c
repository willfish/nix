#include "run.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char *root, *bin_dir, *contrast, *youtube, *stub, *partial;
static int serial;
static void expect(int cond, const char *what) {
  if (!cond)
    check_fail("%s", what);
}
static char *file_path(const char *name) {
  char leaf[64];
  snprintf(leaf, sizeof leaf, "%d-%s", serial++, name);
  return check_join(root, leaf);
}
static char *write_named(const char *name, const void *data, size_t len) {
  char *path = file_path(name);
  if (!path || check_write(path, data, len))
    check_fail("write %s", name);
  return path;
}
static int args_have(const char *const *args, const char *flag) {
  for (size_t i = 0; args[i]; i++)
    if (!strcmp(args[i], flag))
      return 1;
  return 0;
}
static Proc invoke(const char *program, const char *const *args,
                   const char *const *env, int parity) {
  Proc result = check_run(program, args, NULL, 0, env, 10);
  if (!result.exited)
    check_fail("process did not exit: %s", result.err);
  if (contains_text(result.err, result.err_len, "Sanitizer"))
    check_fail("sanitizer: %s", result.err);
  const char *legacy_name =
      program == contrast ? "CONTRAST_LEGACY" : "YOUTUBE_LEGACY";
  const char *legacy = getenv(legacy_name);
  if (legacy && *legacy && parity) {
    size_t n = 0;
    while (args[n])
      n++;
    const char **old_args = calloc(n + 3, sizeof *old_args);
    const char *python = getenv("LEGACY_PYTHON");
    old_args[0] = python && *python ? python : "python3";
    old_args[1] = legacy;
    for (size_t i = 0; i < n; i++)
      old_args[i + 2] = args[i];
    Proc old = check_run(old_args[0], old_args, NULL, 0, env, 10);
    if (result.status != old.status)
      check_fail("legacy status %d != %d\n%s\n%s", result.status, old.status,
                 result.err, old.err);
    if (result.status == 0 || (program == contrast && result.status == 1)) {
      if (args_have(args, "--json")) {
        yyjson_doc *left = parse_json(result.out, result.out_len);
        yyjson_doc *right = parse_json(old.out, old.out_len);
        if (!left || !right ||
            !json_equal(yyjson_doc_get_root(left), yyjson_doc_get_root(right)))
          check_fail("legacy JSON mismatch");
        yyjson_doc_free(left);
        yyjson_doc_free(right);
      } else if (result.out_len != old.out_len ||
                 memcmp(result.out, old.out, result.out_len))
        check_fail("legacy stdout mismatch");
    }
    proc_free(&old);
    free(old_args);
  }
  return result;
}
static const char info_json[] =
    "{\"id\":\"qILTuXLxfBM\",\"title\":\"Example metadata\",\"channel\":"
    "\"Example channel\",\"duration\":1610,\"webpage_url\":\"https://"
    "www.youtube.com/watch?v=qILTuXLxfBM\",\"description\":\"\\u0085 A "
    "description\\nwith two lines. \\u2003\",\"chapters\":[{\"start_time\":"
    "214,\"title\":\"The model\"},{\"start_time\":1051,\"title\":\"Settings\"}]}";
static const char vtt_text[] =
    "WEBVTT\nKind: captions\nLanguage: en\n\n00:03:34.080 --> 00:03:40.309\nat "
    "what I'm talking about. This is the\nQuen 3.8 27 billion GSQRCO GGUF. "
    "It's a\n\n00:03:40.319 --> 00:03:43.350\nQuen 3.8 27 billion GSQRCO GGUF. "
    "It's a\nnonuniform GGUF quantization produced\n\n00:17:31.000 --> "
    "00:17:34.000\nthat I have the settings here and I got\nthe prompt "
    "library.\n\n00:17:34.000 --> 00:17:34.000\nthat I have the settings here "
    "and I got\nthe prompt library.\n";
static char *info_path, *vtt_path;
static yyjson_doc *payload(const char *const *args, const char *const *env) {
  size_t n = 0;
  while (args[n])
    n++;
  const char **all = calloc(n + 3, sizeof *all);
  all[0] = youtube;
  for (size_t i = 0; i < n; i++)
    all[i + 1] = args[i];
  all[n + 1] = "--json";
  Proc result = invoke(youtube, all, env, 1);
  free(all);
  if (result.status != 0)
    check_fail("payload status %d: %s", result.status, result.err);
  yyjson_doc *doc = parse_json(result.out, result.out_len);
  if (!doc)
    check_fail("payload JSON");
  proc_free(&result);
  return doc;
}
static void contrast_ratios(void) {
  check_begin("contrast black/white, identical colours and channel transfer/weighting");
  const char *rows[][3] = {
      {"#000000", "#ffffff", "PASS 21.000000:1"},
      {"#ffffff", "#000000", "PASS 21.000000:1"},
      {"#18212b", "#18212b", "PASS 1.000000:1"},
      {"#ff0000", "#000000", "PASS 5.252000:1"},
      {"#00ff00", "#000000", "PASS 15.304000:1"},
      {"#0000ff", "#000000", "PASS 2.444000:1"},
      {"#0a0a0a", "#000000", "PASS 1.060705:1"},
      {"#0b0b0b", "#000000", "PASS 1.066931:1"}};
  for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
    const char *args[] = {contrast, rows[i][0], rows[i][1], "--minimum", "1",
                          NULL};
    Proc result = invoke(contrast, args, NULL, 1);
    expect(result.status == 0, "contrast status");
    if (!contains_text(result.out, result.out_len, rows[i][2]) ||
        result.out_len < strlen(rows[i][2]) ||
        memcmp(result.out, rows[i][2], strlen(rows[i][2])))
      check_fail("contrast prefix %s got %s", rows[i][2], result.out);
    proc_free(&result);
  }
}
static void contrast_threshold(void) {
  check_begin("contrast uses the unrounded ratio and preserves explicit thresholds");
  const char *fail[] = {contrast, "#777777", "#ffffff", NULL};
  const char *pass[] = {contrast, "#767676", "#ffffff", NULL};
  const char *exact[] = {contrast, "#777777", "#ffffff", "--minimum",
                         "4.478089463577213", NULL};
  Proc a = invoke(contrast, fail, NULL, 1);
  Proc b = invoke(contrast, pass, NULL, 1);
  Proc c = invoke(contrast, exact, NULL, 1);
  expect(a.status == 1 && b.status == 0 && c.status == 1, "threshold status");
  proc_free(&a);
  proc_free(&b);
  proc_free(&c);
  const char *mins[] = {"3", "21", "1", " 4.5 ", "4_._5"};
  for (size_t i = 0; i < 5; i++) {
    const char *args[] = {contrast, "#000000", "#ffffff", "--minimum", mins[i],
                          NULL};
    Proc result = invoke(contrast, args, NULL, 1);
    expect(result.status == (strcmp(mins[i], "4_._5") ? 0 : 2), mins[i]);
    proc_free(&result);
  }
}
static void contrast_invalid(void) {
  check_begin("contrast rejects invalid colours, nonfinite thresholds and bad usage");
  const char *colours[] = {"white", "#fff", "#12345678", "#zz0000", "123456",
                           " #000000"};
  for (size_t i = 0; i < sizeof colours / sizeof *colours; i++) {
    const char *args[] = {contrast, colours[i], "#ffffff", NULL};
    Proc result = invoke(contrast, args, NULL, 1);
    expect(result.status == 2, colours[i]);
    proc_free(&result);
  }
  const char *mins[] = {"0", "22", "nan", "inf", "-inf", "banana", "0x10"};
  for (size_t i = 0; i < sizeof mins / sizeof *mins; i++) {
    const char *args[] = {contrast, "#000000", "#ffffff", "--minimum", mins[i],
                          NULL};
    Proc result = invoke(contrast, args, NULL, 1);
    expect(result.status == 2, mins[i]);
    proc_free(&result);
  }
  const char *none[] = {contrast, NULL};
  const char *help[] = {contrast, "--help", NULL};
  Proc empty = check_run(contrast, none, NULL, 0, NULL, 10);
  Proc helped = check_run(contrast, help, NULL, 0, NULL, 10);
  expect(empty.status == 2 && helped.status == 0, "usage");
  proc_free(&empty);
  proc_free(&helped);
}
static void contrast_sample(void) {
  check_begin("contrast matches retained CLI across a deterministic colour sample");
  uint32_t state = 1234567;
  for (int i = 0; i < 40; i++) {
    state = state * 1664525u + 1013904223u;
    char left[8], right[8];
    snprintf(left, sizeof left, "#%06x", state & 0xffffffu);
    state = state * 1664525u + 1013904223u;
    snprintf(right, sizeof right, "#%06x", state & 0xffffffu);
    const char *args[] = {contrast, left, right, NULL};
    Proc result = invoke(contrast, args, NULL, 1);
    expect(result.status == 0 || result.status == 1, "sample status");
    proc_free(&result);
  }
}
static double jreal(yyjson_val *value) { return yyjson_get_num(value); }
static void vtt_window(void) {
  check_begin("VTT metadata, rolling duplicate removal, around window and search");
  const char *args[] = {"--info-file", info_path, "--subs-file", vtt_path,
                        "--around",    "214",     "--query",     "GGUF", NULL};
  yyjson_doc *doc = payload(args, NULL);
  yyjson_val *root_ = yyjson_doc_get_root(doc);
  expect(jreal(jget(root_, "cue_count")) == 3, "cue_count");
  size_t len = 0;
  const char *duration = jstr(jget(root_, "duration_string"), &len);
  expect(duration && len == 5 && !memcmp(duration, "26:50", 5), "duration");
  yyjson_val *chapter = yyjson_arr_get(jget(root_, "chapters"), 0);
  const char *clock = jstr(jget(chapter, "clock"), &len);
  expect(clock && !strcmp(clock, "3:34"), "clock");
  expect(yyjson_arr_size(jget(root_, "matches")) == 2, "matches");
  expect(yyjson_arr_size(jget(root_, "around")) == 2, "around");
  expect(jreal(jget(root_, "timestamp")) == 214, "timestamp");
  yyjson_doc_free(doc);
}
static void url_forms(void) {
  check_begin("URL timestamp forms, explicit overrides and positional/flag precedence");
  const char *urls[][2] = {
      {"https://youtu.be/qILTuXLxfBM?t=17m31s", "1051"},
      {"https://youtube.com/watch?v=qILTuXLxfBM&t=1051s", "1051"},
      {"https://youtube.com/live/qILTuXLxfBM#t=1h2m3s", "3723"},
      {"https://youtube.com/shorts/qILTuXLxfBM?t=1.5s", "1"}};
  for (size_t i = 0; i < 4; i++) {
    const char *args[] = {urls[i][0], "--subs-file", vtt_path, NULL};
    yyjson_doc *doc = payload(args, NULL);
    expect(jreal(jget(yyjson_doc_get_root(doc), "timestamp")) ==
               strtod(urls[i][1], NULL),
           urls[i][0]);
    yyjson_doc_free(doc);
    const char *over[] = {urls[i][0], "--subs-file", vtt_path, "--around",
                          "2.5",       NULL};
    doc = payload(over, NULL);
    expect(jreal(jget(yyjson_doc_get_root(doc), "timestamp")) == 2.5, "override");
    yyjson_doc_free(doc);
  }
  const char *both[] = {"https://youtu.be/qILTuXLxfBM?t=2",
                        "--url",
                        "https://youtu.be/qILTuXLxfBM?t=3",
                        "--subs-file",
                        vtt_path,
                        NULL};
  yyjson_doc *doc = payload(both, NULL);
  expect(jreal(jget(yyjson_doc_get_root(doc), "timestamp")) == 3, "flag wins");
  yyjson_doc_free(doc);
  const char *values[] = {"1051", "1H2M3S", "17m31s", "\xd9\xa1\xd9\xa2.\xd9\xa5", "\xc2\x85 10 \xe2\x80\x83"};
  for (size_t i = 0; i < 5; i++) {
    const char *args[] = {youtube, "--subs-file", vtt_path, "--around",
                          values[i], "--json", NULL};
    Proc result = invoke(youtube, args, NULL, 1);
    expect(result.status == 0, values[i]);
    proc_free(&result);
  }
}
static void srt_casefold(void) {
  check_begin("SRT, timestamp settings, CRLF, inclusive boundaries and Unicode casefold search");
  const char *srt =
      "1\r\n00:00:01,000 --> 00:00:02,500 align:start\r\n<i>Hello</i>&nbsp;Straße\r\n\r\n2\r00:00:03,000 --> 00:00:04,000\rSecond line\r";
  char *path = write_named("captions.srt", srt, strlen(srt));
  const char *args[] = {"--subs-file", path, "--around", "2.5", "--window",
                        "0", "--query", "STRASSE", NULL};
  yyjson_doc *doc = payload(args, NULL);
  yyjson_val *root_ = yyjson_doc_get_root(doc);
  expect(yyjson_arr_size(jget(root_, "around")) == 1, "around");
  size_t len = 0;
  const char *text = jstr(jget(yyjson_arr_get(jget(root_, "matches"), 0), "text"), &len);
  expect(text && !strcmp(text, "Hello Straße"), "text");
  expect(jreal(jget(root_, "cue_count")) == 2, "cues");
  yyjson_doc_free(doc);
  free(path);
}
static void rolling(void) {
  check_begin("rolling equal, growing and shrinking cues retain the earliest start and latest end");
  const char *subs =
      "0:00 --> 0:01\nhello\n\n0:01 --> 0:02\nhello world\n\n0:02 --> 0:03\nhello\n\n0:03 --> 0:04\nhello world\n\n0:04 --> 0:05\nother\n";
  char *path = write_named("rolling.vtt", subs, strlen(subs));
  const char *args[] = {"--subs-file", path, "--around", "0", "--window", "10",
                        NULL};
  yyjson_doc *doc = payload(args, NULL);
  yyjson_val *root_ = yyjson_doc_get_root(doc);
  expect(jreal(jget(root_, "cue_count")) == 2, "cue_count");
  yyjson_val *cue = yyjson_arr_get(jget(root_, "around"), 0);
  size_t len = 0;
  const char *text = jstr(jget(cue, "text"), &len);
  expect(jreal(jget(cue, "start")) == 0 && jreal(jget(cue, "end")) == 4 && text &&
             !strcmp(text, "hello world"),
         "rolling cue");
  yyjson_doc_free(doc);
  free(path);
}
static void binary_caption(void) {
  check_begin("replacement decoding, NUL and tag cleanup preserve caption content");
  const char prefix[] = "0:00 --> 0:01\n<tag>foo";
  const char suffix[] = "bar</tag> &amp; &nbsp;";
  unsigned char raw[128];
  size_t n = 0;
  memcpy(raw, prefix, sizeof prefix - 1);
  n += sizeof prefix - 1;
  raw[n++] = 0;
  memcpy(raw + n, suffix, sizeof suffix - 1);
  n += sizeof suffix - 1;
  raw[n++] = 0xff;
  raw[n++] = '\n';
  char *path = write_named("binary.vtt", raw, n);
  const char *args[] = {"--subs-file", path, "--query", "BAR", NULL};
  yyjson_doc *doc = payload(args, NULL);
  size_t len = 0;
  const char *text = jstr(
      jget(yyjson_arr_get(jget(yyjson_doc_get_root(doc), "matches"), 0), "text"),
      &len);
  const unsigned char expected[] = "foo\0bar &amp;  \xef\xbf\xbd";
  size_t expected_len = sizeof expected - 1;
  if (!text || len != expected_len || memcmp(text, expected, expected_len))
    check_fail("binary caption mismatch len %zu", len);
  yyjson_doc_free(doc);
  free(path);
}
static void text_rendering(void) {
  check_begin("text rendering covers metadata, timestamps, quoted search, empty results and bounded samples");
  const char *plain[] = {youtube, "--info-file", info_path, "--subs-file",
                         vtt_path, NULL};
  const char *around[] = {youtube, "--info-file", info_path, "--subs-file",
                          vtt_path, "--around", "17m31s", NULL};
  const char *both[] = {youtube, "--info-file", info_path, "--subs-file",
                        vtt_path, "--around", "214", "--query", "GGUF", NULL};
  const char *miss[] = {youtube, "--info-file", info_path, "--subs-file",
                        vtt_path, "--query", "doesn't match", NULL};
  const char *space[] = {youtube, "--info-file", info_path, "--subs-file",
                         vtt_path, "--query", "\xc2\x85", NULL};
  const char *quoted[] = {youtube, "--info-file", info_path, "--subs-file",
                          vtt_path, "--query", "\"both'\"", NULL};
  const char *empty[] = {youtube, "--info-file", info_path, "--subs-file",
                         vtt_path, "--query", "", NULL};
  const char *const *all[] = {plain, around, both, miss, space, quoted, empty};
  for (size_t i = 0; i < 7; i++) {
    Proc result = invoke(youtube, all[i], NULL, 1);
    expect(result.status == 0, "text status");
    proc_free(&result);
  }
  const char *meta[] = {youtube, "--info-file", info_path, NULL};
  Proc meta_out = invoke(youtube, meta, NULL, 1);
  expect(ends_with(meta_out.out, meta_out.out_len, "Captions: none\n"),
         "captions none");
  proc_free(&meta_out);
  char many[2048];
  size_t at = 0;
  for (int i = 0; i < 15; i++) {
    int wrote = snprintf(many + at, sizeof many - at,
                         "0:%02d --> 0:%02d\nrow %02d\n%s", i, i + 1, i,
                         i == 14 ? "" : "\n");
    if (wrote < 0)
      check_fail("many vtt");
    at += (size_t)wrote;
  }
  char *path = write_named("many.vtt", many, at);
  const char *sample[] = {youtube, "--subs-file", path, NULL};
  Proc sample_out = invoke(youtube, sample, NULL, 1);
  int marks = 0;
  for (size_t i = 0; i < sample_out.out_len; i++)
    if (sample_out.out[i] == '[' && (i == 0 || sample_out.out[i - 1] == '\n'))
      marks++;
  expect(marks == 12, "bounded sample");
  proc_free(&sample_out);
  free(path);
}
static void empty_metadata(void) {
  check_begin("empty metadata, uploader fallback, chapters and negative windows keep the schema");
  const char *fallback =
      "{\"uploader\":\"Uploader\",\"duration\":3661,\"chapters\":[{\"title\":\"Start\"},{\"start_time\":\"2.5\",\"title\":\"\"}]}";
  char *path = write_named("fallback.json", fallback, strlen(fallback));
  const char *args[] = {"--info-file", path, "--subs-file", vtt_path, "--window",
                        "-1", "--around", "214", NULL};
  yyjson_doc *doc = payload(args, NULL);
  yyjson_val *root_ = yyjson_doc_get_root(doc);
  size_t len = 0;
  const char *channel = jstr(jget(root_, "channel"), &len);
  const char *duration = jstr(jget(root_, "duration_string"), &len);
  yyjson_val *chapter = yyjson_arr_get(jget(root_, "chapters"), 0);
  expect(channel && !strcmp(channel, "Uploader"), "channel");
  expect(duration && !strcmp(duration, "1:01:01"), "duration");
  expect(jreal(jget(chapter, "start")) == 0, "chapter start");
  expect(yyjson_is_null(jget(root_, "id")), "id");
  expect(jreal(jget(root_, "window")) == -1, "window");
  yyjson_doc_free(doc);
  char *empty = write_named("empty.vtt", "", 0);
  const char *none[] = {"--subs-file", empty, NULL};
  doc = payload(none, NULL);
  expect(yyjson_arr_size(jget(yyjson_doc_get_root(doc), "around")) == 0, "empty");
  yyjson_doc_free(doc);
  free(path);
  free(empty);
}
static void invalid_inputs(void) {
  check_begin("invalid timestamps, malformed cues/JSON, missing files and usage fail visibly");
  char *bad_vtt = write_named("bad.vtt", "nonsense --> 0:01\ntext\n", 23);
  char *bad_json = write_named("bad.json", "{", 1);
  const char *none[] = {youtube, NULL};
  const char *missing[] = {youtube, "--info-file", "/missing/info", NULL};
  const char *badcue[] = {youtube, "--subs-file", bad_vtt, NULL};
  const char *badjson[] = {youtube, "--info-file", bad_json, NULL};
  const char *badtime[] = {youtube, "--subs-file", vtt_path, "--around",
                           "invalid", NULL};
  const char *blank[] = {youtube, "--subs-file", vtt_path, "--around", " ", NULL};
  const char *badwin[] = {youtube, "--subs-file", vtt_path, "--window", "bad",
                          NULL};
  const char *const *cases[] = {none, missing, badcue, badjson, badtime, blank,
                                badwin};
  for (size_t i = 0; i < 7; i++) {
    Proc result = invoke(youtube, cases[i], NULL, 1);
    expect(result.status != 0, "visible failure");
    proc_free(&result);
  }
  const char *help[] = {youtube, "--help", NULL};
  Proc helped = check_run(youtube, help, NULL, 0, NULL, 10);
  expect(helped.status == 0, "help");
  proc_free(&helped);
  free(bad_vtt);
  free(bad_json);
}
static char *downloader(const char *name, char **log_out) {
  char leaf[32];
  snprintf(leaf, sizeof leaf, "bin-%d", serial++);
  char *dir = check_join(root, leaf);
  check_mkdir(dir);
  char *log = file_path("download-log");
  check_write(log, "", 0);
  char *link = check_join(dir, name);
  if (symlink(stub, link))
    check_fail("symlink stub");
  free(link);
  *log_out = log;
  return dir;
}
static int json_string_equal(yyjson_val *array, size_t index, const char *text) {
  size_t len = 0;
  const char *got = jstr(yyjson_arr_get(array, index), &len);
  return got && len == strlen(text) && !memcmp(got, text, len);
}
static void host_downloader(void) {
  check_begin("host yt-dlp uses only metadata/subtitle flags, selects manual captions and cleans temporary data");
  char *log = NULL;
  char *dir = downloader("yt-dlp", &log);
  char *path_env = NULL;
  size_t n = strlen(dir) + 6;
  path_env = malloc(n);
  snprintf(path_env, n, "PATH=%s", dir);
  char *log_env = malloc(strlen(log) + 14);
  snprintf(log_env, strlen(log) + 14, "DOWNLOAD_LOG=%s", log);
  const char *env[] = {path_env, log_env, NULL};
  const char *args[] = {"https://youtu.be/qILTuXLxfBM", "--query", "manual", NULL};
  yyjson_doc *doc = payload(args, env);
  size_t len = 0;
  const char *text = jstr(
      jget(yyjson_arr_get(jget(yyjson_doc_get_root(doc), "matches"), 0), "text"),
      &len);
  expect(text && !strcmp(text, "manual"), "manual caption");
  yyjson_doc_free(doc);
  size_t log_len = 0;
  unsigned char *logged = check_read(log, &log_len);
  yyjson_doc *args_doc = parse_json(logged, log_len);
  yyjson_val *list = yyjson_doc_get_root(args_doc);
  const char *expect_args[] = {"--ignore-errors", "--skip-download", "--no-playlist",
                               "--no-warnings", "--write-info-json", "--write-subs",
                               "--write-auto-subs", "--sub-langs", "en.*,en",
                               "--sub-format", "vtt/best", "-o"};
  size_t count = yyjson_arr_size(list);
  expect(count >= 14, "arg count");
  for (size_t i = 0; i < 12; i++)
    if (!json_string_equal(list, i, expect_args[i]))
      check_fail("flag %s", expect_args[i]);
  int banned = 0;
  for (size_t i = 0; i < count; i++) {
    size_t item_len = 0;
    const char *item = jstr(yyjson_arr_get(list, i), &item_len);
    if (item && (!strcmp(item, "-x") || !strcmp(item, "-f")))
      banned = 1;
  }
  expect(!banned, "no media flags");
  size_t out_len = 0;
  const char *output = jstr(yyjson_arr_get(list, count - 2), &out_len);
  char *copy = malloc(out_len + 1);
  if (!copy)
    check_fail("output path");
  memcpy(copy, output, out_len);
  copy[out_len] = 0;
  char *slash = strrchr(copy, '/');
  if (slash)
    *slash = 0;
  expect(access(copy, F_OK) != 0, "temp removed");
  free(copy);
  yyjson_doc_free(args_doc);
  free(logged);
  free(path_env);
  free(log_env);
  free(dir);
  free(log);
}
static void partial_failure(void) {
  check_begin("yt-dlp partial caption failure still yields metadata and captions");
  char leaf[40];
  snprintf(leaf, sizeof leaf, "bin-partial-%d", serial++);
  char *dir = check_join(root, leaf);
  check_mkdir(dir);
  char *link = check_join(dir, "yt-dlp");
  if (symlink(partial, link))
    check_fail("partial symlink");
  free(link);
  char *strict_log = file_path("strict-log");
  check_write(strict_log, "", 0);
  char path_env[512], log_env[512], mode_env[] = "DOWNLOAD_MODE=strict";
  snprintf(path_env, sizeof path_env, "PATH=%s", dir);
  snprintf(log_env, sizeof log_env, "DOWNLOAD_LOG=%s", strict_log);
  const char *strict_env[] = {path_env, log_env, mode_env, NULL};
  const char *args[] = {youtube, "https://youtu.be/qILTuXLxfBM", NULL};
  Proc strict = check_run(youtube, args, NULL, 0, strict_env, 10);
  expect(strict.status == 1, "strict failure");
  proc_free(&strict);
  char *log = file_path("with-log");
  check_write(log, "", 0);
  snprintf(log_env, sizeof log_env, "DOWNLOAD_LOG=%s", log);
  const char *ok_env[] = {path_env, log_env, NULL};
  const char *flags[] = {"https://youtu.be/qILTuXLxfBM", NULL};
  yyjson_doc *doc = payload(flags, ok_env);
  yyjson_val *root_ = yyjson_doc_get_root(doc);
  size_t len = 0;
  const char *title = jstr(jget(root_, "title"), &len);
  expect(jreal(jget(root_, "cue_count")) == 1, "partial cues");
  expect(title && !strcmp(title, "stub title"), "partial title");
  yyjson_doc_free(doc);
  size_t log_len = 0;
  unsigned char *logged = check_read(log, &log_len);
  expect(contains_text(logged, log_len, "--ignore-errors"), "ignore-errors");
  free(logged);
  free(strict_log);
  free(log);
  free(dir);
}
static void nix_fallback(void) {
  check_begin("Nix fallback keeps existing work directories, handles missing captions and propagates fetch failure");
  char *log = NULL;
  char *dir = downloader("nix", &log);
  char *workdir = check_join(root, "work");
  check_mkdir(workdir);
  char *path_env = malloc(strlen(dir) + 6);
  char *log_env = malloc(strlen(log) + 14);
  snprintf(path_env, strlen(dir) + 6, "PATH=%s", dir);
  snprintf(log_env, strlen(log) + 14, "DOWNLOAD_LOG=%s", log);
  const char *env[] = {path_env, log_env, "DOWNLOAD_MODE=no-subs", NULL};
  const char *args[] = {"--url", "https://youtu.be/qILTuXLxfBM", "--workdir",
                        workdir, NULL};
  yyjson_doc *doc = payload(args, env);
  expect(jreal(jget(yyjson_doc_get_root(doc), "cue_count")) == 0, "no subs");
  yyjson_doc_free(doc);
  char *info = check_join(workdir, "video.info.json");
  expect(access(info, F_OK) == 0, "retained info");
  free(info);
  size_t log_len = 0;
  unsigned char *logged = check_read(log, &log_len);
  yyjson_doc *args_doc = parse_json(logged, log_len);
  yyjson_val *list = yyjson_doc_get_root(args_doc);
  const char *prefix[] = {"shell", "nixpkgs#yt-dlp", "-c", "yt-dlp"};
  for (size_t i = 0; i < 4; i++)
    if (!json_string_equal(list, i, prefix[i]))
      check_fail("nix prefix %s", prefix[i]);
  yyjson_doc_free(args_doc);
  free(logged);
  const char *modes[] = {"fail", "no-info"};
  for (size_t i = 0; i < 2; i++) {
    char mode[64];
    snprintf(mode, sizeof mode, "DOWNLOAD_MODE=%s", modes[i]);
    const char *mode_env[] = {path_env, log_env, mode, NULL};
    const char *fetch[] = {youtube, "https://youtu.be/qILTuXLxfBM", NULL};
    Proc result = invoke(youtube, fetch, mode_env, 1);
    expect(result.status == 1, modes[i]);
    proc_free(&result);
  }
  char *missing_path = malloc(strlen(root) + 6);
  snprintf(missing_path, strlen(root) + 6, "PATH=%s", root);
  const char *missing_env[] = {missing_path, NULL};
  const char *fetch[] = {youtube, "https://youtu.be/qILTuXLxfBM", NULL};
  Proc missing = invoke(youtube, fetch, missing_env, 1);
  expect(missing.status == 1, "missing downloader");
  proc_free(&missing);
  free(missing_path);
  free(path_env);
  free(log_env);
  free(workdir);
  free(dir);
  free(log);
}
void run_tool_tests(void) {
  root = check_temp("skill-tools-");
  bin_dir = getenv("SKILL_TOOLS_BIN") ? strdup(getenv("SKILL_TOOLS_BIN"))
                                     : check_exe_dir();
  contrast = getenv("CONTRAST_BIN") ? strdup(getenv("CONTRAST_BIN"))
                                    : check_join(bin_dir, "contrast");
  youtube = getenv("YOUTUBE_BIN") ? strdup(getenv("YOUTUBE_BIN"))
                                  : check_join(bin_dir, "youtube-extract");
  char *exe = check_exe_dir();
  stub = check_join(exe, "ytdlp-stub");
  partial = check_join(exe, "ytdlp-partial");
  free(exe);
  if (!root || !contrast || !youtube)
    check_fail("setup");
  info_path = write_named("info.json", info_json, strlen(info_json));
  vtt_path = write_named("captions.vtt", vtt_text, strlen(vtt_text));
  contrast_ratios();
  contrast_threshold();
  contrast_invalid();
  contrast_sample();
  vtt_window();
  url_forms();
  srt_casefold();
  rolling();
  binary_caption();
  text_rendering();
  empty_metadata();
  invalid_inputs();
  host_downloader();
  partial_failure();
  nix_fallback();
  check_rm_rf(root);
  free(root);
  free(bin_dir);
  free(contrast);
  free(youtube);
  free(stub);
  free(partial);
  free(info_path);
  free(vtt_path);
}
