#include "run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char *root, *bin_dir, *inventory, *duplicate, *libation;
static int serial;
static void expect(int cond, const char *what) {
  if (!cond)
    check_fail("%s", what);
}
static char *make_dir(void) {
  char leaf[32];
  snprintf(leaf, sizeof leaf, "%d", serial++);
  char *path = check_join(root, leaf);
  if (check_mkdir(path))
    check_fail("mkdir");
  return path;
}
typedef struct {
  unsigned char *data;
  size_t len, cap;
} Buf;
static void buf_add(Buf *b, const void *data, size_t n) {
  if (b->len > SIZE_MAX - n - 1)
    exit(1);
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + n + 1) {
      if (cap > SIZE_MAX / 2)
        exit(1);
      cap *= 2;
    }
    unsigned char *grown = realloc(b->data, cap);
    if (!grown)
      exit(1);
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, data, n);
  b->len += n;
  b->data[b->len] = 0;
}
static void bencode(Buf *out, const void *value, size_t n, int kind, long number,
                    const Buf *items, size_t count) {
  (void)items;
  (void)count;
  if (kind == 0) {
    char prefix[32];
    int wrote = snprintf(prefix, sizeof prefix, "%zu:", n);
    buf_add(out, prefix, (size_t)wrote);
    buf_add(out, value, n);
  } else if (kind == 1) {
    char text[64];
    int wrote = snprintf(text, sizeof text, "i%lde", number);
    buf_add(out, text, (size_t)wrote);
  }
}
static void bencode_bytes(Buf *out, const void *value, size_t n) {
  bencode(out, value, n, 0, 0, NULL, 0);
}
static void bencode_text(Buf *out, const char *text) {
  bencode_bytes(out, text, strlen(text));
}
static void bencode_int(Buf *out, long number) { bencode(out, NULL, 0, 1, number, NULL, 0); }
typedef struct {
  char *downloads, *resume, **args;
  size_t argc;
} Inv;
static Inv inv(void) {
  Inv f = {.downloads = make_dir(), .resume = make_dir(), .argc = 4};
  f.args = calloc(16, sizeof *f.args);
  f.args[0] = inventory;
  f.args[1] = "--download-root";
  f.args[2] = f.downloads;
  f.args[3] = "--fastresume-dir";
  f.args[4] = f.resume;
  f.argc = 5;
  return f;
}
static void inv_add(Inv *f, const void *name, size_t name_len, const char *file,
                    const unsigned char *extra, size_t extra_len, int finished,
                    int downloaded, int has_finished, int has_downloaded) {
  Buf body = {0};
  buf_add(&body, "d", 1);
  bencode_text(&body, "name");
  bencode_bytes(&body, name, name_len);
  bencode_text(&body, "qBt-savePath");
  bencode_text(&body, f->downloads);
  if (has_finished) {
    bencode_text(&body, "finished_time");
    bencode_int(&body, finished);
  }
  if (has_downloaded) {
    bencode_text(&body, "total_downloaded");
    bencode_int(&body, downloaded);
  }
  if (extra)
    buf_add(&body, extra, extra_len);
  buf_add(&body, "e", 1);
  char leaf[64];
  if (file)
    snprintf(leaf, sizeof leaf, "%s", file);
  else
    snprintf(leaf, sizeof leaf, "%d.fastresume", serial++);
  char *path = check_join(f->resume, leaf);
  if (check_write(path, body.data, body.len))
    check_fail("fastresume");
  free(path);
  free(body.data);
}
static Proc call_cmd(const char *command, const char *const *args, const void *input,
                     size_t input_len, int parity) {
  Proc result = check_run(command, args, input, input_len, NULL, 10);
  if (!result.exited)
    check_fail("did not exit: %s", result.err);
  if (contains_text(result.err, result.err_len, "Sanitizer"))
    check_fail("sanitizer: %s", result.err);
  const char *legacy_name =
      command == inventory ? "INVENTORY_LEGACY" : "DUPLICATE_LEGACY";
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
    Proc old = check_run(old_args[0], old_args, input, input_len, NULL, 10);
    if (result.status != old.status)
      check_fail("legacy status %d != %d", result.status, old.status);
    int json = 0;
    for (size_t i = 0; i < n; i++)
      if (!strcmp(args[i], "json"))
        json = 1;
    if (json && result.out_len) {
      yyjson_doc *left = parse_json(result.out, result.out_len);
      yyjson_doc *right = parse_json(old.out, old.out_len);
      if (!left || !right ||
          !json_equal(yyjson_doc_get_root(left), yyjson_doc_get_root(right)))
        check_fail("legacy JSON");
      yyjson_doc_free(left);
      yyjson_doc_free(right);
    } else if ((result.status == 0 ||
                (command == duplicate && result.out_len)) &&
               (result.out_len != old.out_len ||
                memcmp(result.out, old.out, result.out_len)))
      check_fail("legacy bytes");
    proc_free(&old);
    free(old_args);
  }
  return result;
}
static yyjson_doc *report(const char *command, const char *const *args,
                          const void *input, size_t input_len, int *code) {
  size_t n = 0;
  while (args[n])
    n++;
  const char **all = calloc(n + 3, sizeof *all);
  for (size_t i = 0; i < n; i++)
    all[i] = args[i];
  all[n] = "--format";
  all[n + 1] = "json";
  Proc result = call_cmd(command, all, input, input_len, 1);
  free(all);
  if (code)
    *code = result.status;
  yyjson_doc *doc = parse_json(result.out, result.out_len);
  if (!doc)
    check_fail("report JSON: %s", result.err);
  proc_free(&result);
  return doc;
}
static const char **inv_args(Inv *f, const char *const *extra, size_t *count) {
  size_t extra_n = 0;
  if (extra)
    while (extra[extra_n])
      extra_n++;
  const char **args = calloc(f->argc + extra_n + 1, sizeof *args);
  for (size_t i = 0; i < f->argc; i++)
    args[i] = f->args[i];
  for (size_t i = 0; i < extra_n; i++)
    args[f->argc + i] = extra[i];
  if (count)
    *count = f->argc + extra_n;
  return args;
}
static void inventory_status(void) {
  check_begin("inventory statuses, bit padding, libtorrent piece bytes and transfer-ready filtering");
  Inv f = inv();
  struct {
    const char *name;
    const char *status;
    const unsigned char *pieces;
    size_t pieces_len;
    int finished, downloaded, has_finished, has_downloaded, write;
  } rows[] = {
      {"complete.m4b", "complete", NULL, 0, 1, 10, 1, 1, 1},
      {"pieces.mp3", "complete", (const unsigned char *)"\xff\xff", 2, 0, 10, 1, 1, 1},
      {"partial", "incomplete", (const unsigned char *)"\x00\xff", 2, 0, 10, 1, 1, 1},
      {"padding", "unknown", (const unsigned char *)"\xff\x80", 2, 0, 10, 1, 1, 1},
      {"empty", "incomplete", NULL, 0, 0, 0, 1, 1, 1},
      {"missing.m4b", "missing_path", NULL, 0, 1, 10, 1, 1, 0},
      {"have.mp3", "complete", (const unsigned char *)"\x01\x01", 2, 0, 10, 1, 1, 1},
      {"verified.mp3", "complete", (const unsigned char *)"\x03\x03", 2, 0, 10, 1, 1, 1},
      {"gap.mp3", "incomplete", (const unsigned char *)"\x01\x00", 2, 0, 10, 1, 1, 1},
      {"unverified.mp3", "incomplete", (const unsigned char *)"\x02\x02", 2, 0, 10, 1, 1, 1}};
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    if (rows[i].write) {
      char *path = check_join(f.downloads, rows[i].name);
      check_write(path, "payload", 7);
      free(path);
    }
    Buf extra = {0};
    if (rows[i].pieces) {
      bencode_text(&extra, "pieces");
      bencode_bytes(&extra, rows[i].pieces, rows[i].pieces_len);
    }
    inv_add(&f, rows[i].name, strlen(rows[i].name), NULL, extra.data, extra.len,
            rows[i].finished, rows[i].downloaded, rows[i].has_finished,
            rows[i].has_downloaded);
    free(extra.data);
    const char **args = inv_args(&f, NULL, NULL);
    yyjson_doc *doc = report(inventory, args, NULL, 0, NULL);
    yyjson_val *array = yyjson_doc_get_root(doc);
    yyjson_val *row = NULL;
    size_t count = yyjson_arr_size(array);
    for (size_t j = 0; j < count; j++) {
      yyjson_val *item = yyjson_arr_get(array, j);
      size_t nlen = 0;
      const char *name = jstr(jget(item, "name"), &nlen);
      if (name && !strcmp(name, rows[i].name))
        row = item;
    }
    size_t len = 0;
    const char *status = row ? jstr(jget(row, "status"), &len) : NULL;
    if (!status || strcmp(status, rows[i].status))
      check_fail("status %s got %s", rows[i].status, status ? status : "");
    yyjson_doc_free(doc);
    free(args);
  }
  const char *ready[] = {"--transfer-ready-only", NULL};
  const char **args = inv_args(&f, ready, NULL);
  yyjson_doc *doc = report(inventory, args, NULL, 0, NULL);
  expect(yyjson_arr_size(yyjson_doc_get_root(doc)) == 4, "transfer-ready");
  yyjson_doc_free(doc);
  free(args);
  const char *markdown[] = {"--format", "markdown", NULL};
  args = inv_args(&f, markdown, NULL);
  Proc text = call_cmd(inventory, args, NULL, 0, 1);
  expect(text.status == 0, "markdown");
  proc_free(&text);
  free(args);
  free(f.downloads);
  free(f.resume);
  free(f.args);
}
static void audiobook_filter(void) {
  check_begin("inventory never walks unregistered download roots and filters audiobook names and nested media");
  Inv f = inv();
  char *private = check_join(f.downloads, "not registered.m4b");
  check_write(private, "private", 7);
  free(private);
  const char *names[] = {"Course", "Unabridged story", "text.txt"};
  for (size_t i = 0; i < 3; i++) {
    char *path = check_join(f.downloads, names[i]);
    check_mkdir(path);
    free(path);
    inv_add(&f, names[i], strlen(names[i]), NULL, NULL, 0, 1, 10, 1, 1);
  }
  char *course = check_join(f.downloads, "Course");
  char *track = check_join(course, "01.MP3");
  check_write(track, "123", 3);
  free(track);
  free(course);
  char *notes_dir = check_join(f.downloads, "text.txt");
  char *notes = check_join(notes_dir, "notes");
  check_write(notes, "12345", 5);
  free(notes);
  free(notes_dir);
  char *dot = check_join(f.downloads, ".mp3");
  check_write(dot, "media", 5);
  free(dot);
  inv_add(&f, ".mp3", 4, NULL, NULL, 0, 1, 10, 1, 1);
  const char *only[] = {"--audiobook-only", NULL};
  const char **args = inv_args(&f, only, NULL);
  yyjson_doc *doc = report(inventory, args, NULL, 0, NULL);
  yyjson_val *array = yyjson_doc_get_root(doc);
  const char *wanted[] = {"Course", "Unabridged story", ".mp3"};
  expect(yyjson_arr_size(array) == 3, "audiobook count");
  for (size_t i = 0; i < 3; i++) {
    size_t len = 0;
    const char *name = jstr(jget(yyjson_arr_get(array, i), "name"), &len);
    if (!name || strcmp(name, wanted[i]))
      check_fail("name %s", wanted[i]);
  }
  expect(yyjson_get_num(jget(yyjson_arr_get(array, 0), "size_bytes")) == 3, "size");
  expect(yyjson_get_num(jget(yyjson_arr_get(array, 1), "size_bytes")) == 0, "empty size");
  yyjson_doc_free(doc);
  free(args);
  free(f.downloads);
  free(f.resume);
  free(f.args);
}
static void unsafe_names(void) {
  check_begin("unsafe names, parent traversal and payload symlink escapes are excluded");
  Inv f = inv();
  char *outside = make_dir();
  char *secret = check_join(outside, "secret.m4b");
  check_write(secret, "do not include", 14);
  free(secret);
  char *escape = check_join(f.downloads, "escape");
  if (symlink(outside, escape))
    check_fail("symlink");
  free(escape);
  const char *names[] = {"/absolute", "../secret.m4b", "inside/../../outside",
                         "\\absolute", "escape/secret.m4b"};
  for (size_t i = 0; i < 5; i++)
    inv_add(&f, names[i], strlen(names[i]), NULL, NULL, 0, 1, 10, 1, 1);
  const char **args = inv_args(&f, NULL, NULL);
  yyjson_doc *doc = report(inventory, args, NULL, 0, NULL);
  expect(yyjson_arr_size(yyjson_doc_get_root(doc)) == 0, "excluded");
  yyjson_doc_free(doc);
  char *inside = check_join(f.downloads, "inside");
  check_mkdir(inside);
  char *book = check_join(inside, "book.m4b");
  check_write(book, "ok", 2);
  free(book);
  free(inside);
  inv_add(&f, "./inside//book.m4b", strlen("./inside//book.m4b"), NULL, NULL, 0, 1,
          10, 1, 1);
  doc = report(inventory, args, NULL, 0, NULL);
  expect(yyjson_arr_size(yyjson_doc_get_root(doc)) == 1, "relative name");
  yyjson_doc_free(doc);
  free(args);
  free(outside);
  free(f.downloads);
  free(f.resume);
  free(f.args);
}
static void resume_aliases(void) {
  check_begin("resume aliases, fallback save path, integer coercion, malformed bencode and trailing junk");
  Inv f = inv();
  char *book = check_join(f.downloads, "book.m4b");
  check_write(book, "12345", 5);
  free(book);
  Buf body = {0};
  buf_add(&body, "d", 1);
  bencode_text(&body, "qBt-name");
  bencode_text(&body, "book.m4b");
  bencode_text(&body, "finished_time");
  bencode_text(&body, " +123 ");
  bencode_text(&body, "paused");
  bencode_text(&body, "-1");
  bencode_text(&body, "total_downloaded");
  bencode_text(&body, "invalid");
  buf_add(&body, "e", 1);
  buf_add(&body, "trailing junk", 13);
  char *alias = check_join(f.resume, "a.fastresume");
  check_write(alias, body.data, body.len);
  free(alias);
  free(body.data);
  const char *bad[] = {"garbage", "d4:name100:x", "di1e1:xe", "l1:xe", "d4:namei0ee"};
  for (size_t i = 0; i < 5; i++) {
    char leaf[32];
    snprintf(leaf, sizeof leaf, "%d.fastresume", serial++);
    char *path = check_join(f.resume, leaf);
    check_write(path, bad[i], strlen(bad[i]));
    free(path);
  }
  const char **args = inv_args(&f, NULL, NULL);
  yyjson_doc *doc = report(inventory, args, NULL, 0, NULL);
  yyjson_val *array = yyjson_doc_get_root(doc);
  yyjson_val *row = yyjson_arr_get(array, 0);
  size_t len = 0;
  const char *save = jstr(jget(row, "save_path"), &len);
  expect(yyjson_arr_size(array) == 1, "one resume");
  expect(yyjson_is_true(jget(row, "paused")), "paused");
  expect(yyjson_get_num(jget(row, "finished_time")) == 123, "finished");
  expect(yyjson_get_num(jget(row, "total_downloaded")) == 0, "downloaded");
  expect(save && !strcmp(save, f.downloads), "save path");
  yyjson_doc_free(doc);
  free(args);
  free(f.downloads);
  free(f.resume);
  free(f.args);
}
static void nul_names(void) {
  check_begin("byte-safe NUL transfer lists preserve newlines and surrogateescaped filenames");
  Inv f = inv();
  const unsigned char raw[] = {0x62, 0xff, 0x2e, 0x6d, 0x34, 0x62};
  char *prefix = check_join(f.downloads, "");
  size_t prefix_len = strlen(prefix);
  char *raw_path = malloc(prefix_len + sizeof raw + 1);
  memcpy(raw_path, prefix, prefix_len);
  memcpy(raw_path + prefix_len, raw, sizeof raw);
  raw_path[prefix_len + sizeof raw] = 0;
  check_write(raw_path, "data", 4);
  free(raw_path);
  free(prefix);
  inv_add(&f, raw, sizeof raw, "a.fastresume", NULL, 0, 1, 10, 1, 1);
  const char *name = "book\nsecond line.m4b";
  char *book = check_join(f.downloads, name);
  check_write(book, "data", 4);
  free(book);
  inv_add(&f, name, strlen(name), "b.fastresume", NULL, 0, 1, 10, 1, 1);
  const char *flags[] = {"--transfer-ready-only", "--format", "nul", NULL};
  const char **args = inv_args(&f, flags, NULL);
  Proc result = call_cmd(inventory, args, NULL, 0, 1);
  Buf expected = {0};
  buf_add(&expected, raw, sizeof raw);
  buf_add(&expected, "\0", 1);
  buf_add(&expected, name, strlen(name));
  buf_add(&expected, "\0", 1);
  if (result.out_len != expected.len || memcmp(result.out, expected.data, expected.len))
    check_fail("nul list");
  proc_free(&result);
  free(args);
  free(expected.data);
  const char *json_flags[] = {"--format", "json", NULL};
  args = inv_args(&f, json_flags, NULL);
  Proc json_out = call_cmd(inventory, args, NULL, 0, 1);
  expect(contains_text(json_out.out, json_out.out_len, "\"b\\udcff.m4b\""),
         "surrogate name");
  proc_free(&json_out);
  free(args);
  char *out_dir = make_dir();
  char *output = check_join(out_dir, "list");
  const char *nul_flags[] = {"--format", "nul", "-o", output, NULL};
  args = inv_args(&f, nul_flags, NULL);
  result = call_cmd(inventory, args, NULL, 0, 1);
  expect(result.status == 0 && result.out_len == 0, "nul file status");
  size_t file_len = 0;
  unsigned char *file = check_read(output, &file_len);
  expect(file && memchr(file, 0, file_len), "embedded nul");
  free(file);
  proc_free(&result);
  free(args);
  free(output);
  free(out_dir);
  free(f.downloads);
  free(f.resume);
  free(f.args);
}
static void inventory_errors(void) {
  check_begin("inventory missing resume directories are empty and output errors are nonzero");
  Inv f = inv();
  char *missing = check_join(root, "missing");
  const char *args[] = {inventory, "--fastresume-dir", missing, NULL};
  yyjson_doc *doc = report(inventory, args, NULL, 0, NULL);
  expect(yyjson_arr_size(yyjson_doc_get_root(doc)) == 0, "missing dir");
  yyjson_doc_free(doc);
  const char *names[] = {"--format", "names", NULL};
  const char **named = inv_args(&f, names, NULL);
  Proc empty = call_cmd(inventory, named, NULL, 0, 1);
  expect(empty.out_len == 0, "empty names");
  proc_free(&empty);
  free(named);
  char *absent = check_join(root, "absent");
  char *out = check_join(absent, "out");
  const char *output[] = {"-o", out, NULL};
  const char **bad = inv_args(&f, output, NULL);
  Proc failed = call_cmd(inventory, bad, NULL, 0, 1);
  expect(failed.status == 1, "output error");
  proc_free(&failed);
  free(bad);
  const char *format[] = {inventory, "--format", "bad", NULL};
  Proc usage = call_cmd(inventory, format, NULL, 0, 1);
  expect(usage.status == 2, "bad format");
  proc_free(&usage);
  free(out);
  free(absent);
  free(missing);
  free(f.downloads);
  free(f.resume);
  free(f.args);
}
typedef struct {
  char *library, *source_root, *sources, **args;
  size_t argc;
} Dup;
static Dup source_set(const char *names) {
  Dup f = {.library = make_dir(), .source_root = make_dir()};
  char *folder = make_dir();
  f.sources = check_join(folder, "names");
  check_write(f.sources, names, strlen(names));
  free(folder);
  f.args = calloc(8, sizeof *f.args);
  f.args[0] = duplicate;
  f.args[1] = "--sources-file";
  f.args[2] = f.sources;
  f.args[3] = "--targets";
  f.args[4] = f.library;
  f.argc = 5;
  return f;
}
static const char **dup_args(Dup *f, const char *const *extra) {
  size_t extra_n = 0;
  if (extra)
    while (extra[extra_n])
      extra_n++;
  const char **args = calloc(f->argc + extra_n + 1, sizeof *args);
  for (size_t i = 0; i < f->argc; i++)
    args[i] = f->args[i];
  for (size_t i = 0; i < extra_n; i++)
    args[f->argc + i] = extra[i];
  return args;
}
static void duplicate_precedence(void) {
  check_begin("duplicate match precedence covers exact, normalized, ASIN and size hints");
  Dup f = source_set("Exact\nNormal Book (Unabridged).m4b\nElse [B012345678]\nsize source\nClean\n");
  const char *dirs[] = {"Exact", "Normal Book", "Target [B012345678]"};
  for (size_t i = 0; i < 3; i++) {
    char *path = check_join(f.library, dirs[i]);
    check_mkdir(path);
    free(path);
  }
  char *other = check_join(f.library, "other.mp3");
  check_write(other, "1234567", 7);
  free(other);
  char *source = check_join(f.source_root, "size source");
  check_write(source, "1234567", 7);
  free(source);
  const char *extra[] = {"--source-root", f.source_root, NULL};
  const char **args = dup_args(&f, extra);
  yyjson_doc *doc = report(duplicate, args, NULL, 0, NULL);
  yyjson_val *value = yyjson_doc_get_root(doc);
  size_t len = 0;
  const char *status = jstr(jget(value, "status"), &len);
  expect(status && !strcmp(status, "complete"), "complete");
  const char *kinds[] = {"exact_name", "normalized", "asin", "size"};
  yyjson_val *matches = jget(value, "matches");
  expect(yyjson_arr_size(matches) == 4, "match count");
  for (size_t i = 0; i < 4; i++) {
    const char *kind = jstr(jget(yyjson_arr_get(matches, i), "match_type"), &len);
    if (!kind || strcmp(kind, kinds[i]))
      check_fail("match %s", kinds[i]);
  }
  expect(yyjson_get_num(jget(yyjson_arr_get(jget(value, "sources"), 3), "size_bytes")) == 7,
         "source size");
  yyjson_doc_free(doc);
  Proc text = call_cmd(duplicate, args, NULL, 0, 1);
  expect(contains_text(text.out, text.out_len,
                       "No hit (eligible to stage pending full AC5)"),
         "eligible");
  proc_free(&text);
  free(args);
  free(f.library);
  free(f.source_root);
  free(f.sources);
  free(f.args);
}
static void duplicate_normalization(void) {
  check_begin("duplicate normalization removes noise, audio extensions and ASIN brackets");
  Dup f = source_set("The Book Complete WEBRip x264 720p NF GalaxyTV TGx.m4b\nOther [B087654321] (abridged).cue\n");
  char *book = check_join(f.library, "The Book");
  char *other = check_join(f.library, "Other");
  check_mkdir(book);
  check_mkdir(other);
  free(book);
  free(other);
  const char **args = dup_args(&f, NULL);
  yyjson_doc *doc = report(duplicate, args, NULL, 0, NULL);
  yyjson_val *value = yyjson_doc_get_root(doc);
  expect(yyjson_get_num(jget(value, "match_count")) == 2, "normalized count");
  yyjson_val *matches = jget(value, "matches");
  size_t i, n;
  yyjson_val *item;
  yyjson_arr_foreach(matches, i, n, item) {
    size_t len = 0;
    const char *kind = jstr(jget(item, "match_type"), &len);
    expect(kind && !strcmp(kind, "normalized"), "normalized type");
  }
  yyjson_doc_free(doc);
  free(args);
  free(f.library);
  free(f.source_root);
  free(f.sources);
  free(f.args);
}
static void incomplete_scans(void) {
  check_begin("missing roots, non-directories and mixed successful scans remain incomplete");
  Dup f = source_set("Book\n");
  char *book = check_join(f.library, "Book");
  check_mkdir(book);
  free(book);
  char *missing = check_join(root, "missing-one");
  char *second = check_join(root, "missing-two");
  const char *args[] = {duplicate, "--sources-file", f.sources, "--targets",
                        f.library, missing, second, f.sources, NULL};
  int code = 0;
  yyjson_doc *doc = report(duplicate, args, NULL, 0, &code);
  yyjson_val *value = yyjson_doc_get_root(doc);
  size_t len = 0;
  const char *status = jstr(jget(value, "status"), &len);
  expect(code == 1, "incomplete code");
  expect(status && !strcmp(status, "incomplete"), "incomplete");
  expect(yyjson_arr_size(jget(value, "scan_errors")) == 3, "errors");
  expect(yyjson_is_null(jget(value, "match_count")), "null count");
  expect(yyjson_arr_size(jget(value, "matches")) == 0, "no matches");
  yyjson_doc_free(doc);
  Proc text = call_cmd(duplicate, args, NULL, 0, 1);
  expect(!contains_text(text.out, text.out_len, "eligible to stage"), "not eligible");
  proc_free(&text);
  char *folder = make_dir();
  char *output = check_join(folder, "incomplete.json");
  const char *saved[] = {duplicate, "--sources-file", f.sources, "--targets",
                         f.library, missing, second, f.sources, "--format",
                         "json", "-o", output, NULL};
  Proc written = call_cmd(duplicate, saved, NULL, 0, 1);
  expect(written.status == 1, "saved status");
  proc_free(&written);
  size_t file_len = 0;
  unsigned char *file = check_read(output, &file_len);
  yyjson_doc *saved_doc = parse_json(file, file_len);
  status = jstr(jget(yyjson_doc_get_root(saved_doc), "status"), &len);
  expect(status && !strcmp(status, "incomplete"), "saved status text");
  yyjson_doc_free(saved_doc);
  free(file);
  free(output);
  free(folder);
  free(missing);
  free(second);
  free(f.library);
  free(f.source_root);
  free(f.sources);
  free(f.args);
}
static void permission_failures(void) {
  check_begin("permission failures and broken links fail closed instead of reporting a clean scan");
  expect(getuid() != 0, "not root");
  Dup f = source_set("Book\n");
  char *locked = check_join(f.library, "locked");
  check_mkdir(locked);
  chmod(locked, 0);
  const char **args = dup_args(&f, NULL);
  int code = 0;
  yyjson_doc *doc = report(duplicate, args, NULL, 0, &code);
  expect(code == 1, "locked code");
  expect(yyjson_is_null(jget(yyjson_doc_get_root(doc), "match_count")), "locked count");
  yyjson_doc_free(doc);
  chmod(locked, 0700);
  free(locked);
  char *broken = check_join(f.library, "broken");
  if (symlink("/missing/target", broken))
    check_fail("broken symlink");
  free(broken);
  doc = report(duplicate, args, NULL, 0, &code);
  expect(code == 1, "broken code");
  yyjson_doc_free(doc);
  free(args);
  free(f.library);
  free(f.source_root);
  free(f.sources);
  free(f.args);
}
static void target_depth(void) {
  check_begin("target depth, file suffix rules, symlink traversal and canonical path deduplication");
  Dup f = source_set("Book\nonlyaac.aac\nonlywav.wav\ntrack.mp3\n");
  char *author = check_join(f.library, "Author");
  check_mkdir(author);
  char *book = check_join(author, "Book");
  check_mkdir(book);
  free(book);
  const char *files[] = {"onlyaac.aac", "onlywav.wav", "track.mp3"};
  for (size_t i = 0; i < 3; i++) {
    char *path = check_join(author, files[i]);
    check_write(path, "data", 4);
    free(path);
  }
  const char *shallow[] = {"--max-depth", "1", NULL};
  const char **args = dup_args(&f, shallow);
  yyjson_doc *doc = report(duplicate, args, NULL, 0, NULL);
  expect(yyjson_get_num(jget(yyjson_doc_get_root(doc), "match_count")) == 0, "depth 1");
  yyjson_doc_free(doc);
  free(args);
  const char *deep[] = {"--max-depth", "2", NULL};
  args = dup_args(&f, deep);
  doc = report(duplicate, args, NULL, 0, NULL);
  expect(yyjson_get_num(jget(yyjson_doc_get_root(doc), "match_count")) == 2, "depth 2");
  yyjson_doc_free(doc);
  free(args);
  char *alias = check_join(f.library, "Alias");
  if (symlink(author, alias))
    check_fail("alias");
  free(alias);
  const char *twice[] = {"--targets", f.library, f.library, NULL};
  args = dup_args(&f, twice);
  doc = report(duplicate, args, NULL, 0, NULL);
  expect(yyjson_get_num(jget(yyjson_doc_get_root(doc), "match_count")) == 2, "dedup");
  yyjson_doc_free(doc);
  free(args);
  const char *zero[] = {"--max-depth", "0", NULL};
  args = dup_args(&f, zero);
  Proc bad = call_cmd(duplicate, args, NULL, 0, 1);
  expect(bad.status == 2, "depth 0");
  proc_free(&bad);
  free(args);
  free(author);
  free(f.library);
  free(f.source_root);
  free(f.sources);
  free(f.args);
}
static void libation_index(void) {
  check_begin("Libation inventory reads only the index and emits present direct/nested media paths");
  char *home = make_dir();
  char *folder = check_join(home, ".local/share/Libation");
  check_mkdir_p(folder);
  char *media = check_join(home, "book.m4b");
  check_write(media, "media", 5);
  char *index = check_join(folder, "FileLocationsV2.json");
  char body[1024];
  snprintf(body, sizeof body,
           "{\"Dictionary\":{\"B012345678\":[{\"Path\":{\"Path\":\"%s\"}},{\"Path\":\"%s\"},"
           "{\"Path\":\"/missing/book\"},{\"Path\":null},{\"Path\":0},{\"Path\":[]},{\"Path\":{}}]}}",
           media, media);
  check_write(index, body, strlen(body));
  char *accounts = check_join(folder, "AccountsSettings.json");
  check_write(accounts, "not JSON; never read this", 24);
  char *home_env = malloc(strlen(home) + 6);
  snprintf(home_env, strlen(home) + 6, "HOME=%s", home);
  const char *env[] = {home_env, NULL};
  const char *args[] = {libation, NULL};
  Proc result = check_run(libation, args, NULL, 0, env, 10);
  char *expected = malloc(strlen(media) * 2 + 3);
  snprintf(expected, strlen(media) * 2 + 3, "%s\n%s\n", media, media);
  expect(result.status == 0, (const char *)result.err);
  if (result.out_len != strlen(expected) || memcmp(result.out, expected, result.out_len))
    check_fail("libation paths");
  proc_free(&result);
  check_write(index, "{", 1);
  const char *bad[] = {libation, index, NULL};
  Proc failed = check_run(libation, bad, NULL, 0, env, 10);
  expect(failed.status == 1, "bad index");
  proc_free(&failed);
  const char *help[] = {libation, "--help", NULL};
  Proc helped = check_run(libation, help, NULL, 0, env, 10);
  expect(helped.status == 0, "help");
  proc_free(&helped);
  free(expected);
  free(home_env);
  free(accounts);
  free(index);
  free(media);
  free(folder);
  free(home);
}
static void source_stdin(void) {
  check_begin("sources stdin, Unicode whitespace, comments, repeated names and empty roots");
  char *library = make_dir();
  char *book = check_join(library, "Book");
  check_mkdir(book);
  free(book);
  const char *input = "\xc2\x85# comment\n\xe2\x80" "\x83" "Book\xc2\x85" "\nBook\nClean\n";
  const char *args[] = {duplicate, "--sources-file", "-", "--targets", library, NULL};
  yyjson_doc *doc = report(duplicate, args, input, strlen(input), NULL);
  yyjson_val *value = yyjson_doc_get_root(doc);
  expect(yyjson_arr_size(jget(value, "sources")) == 3, "sources");
  expect(yyjson_get_num(jget(value, "match_count")) == 2, "repeated");
  yyjson_doc_free(doc);
  Proc text = call_cmd(duplicate, args, input, strlen(input), 1);
  expect(text.status == 0, "stdin status");
  proc_free(&text);
  Dup empty = source_set("");
  const char **empty_args = dup_args(&empty, NULL);
  doc = report(duplicate, empty_args, NULL, 0, NULL);
  expect(yyjson_get_num(jget(yyjson_doc_get_root(doc), "match_count")) == 0, "empty");
  yyjson_doc_free(doc);
  free(empty_args);
  free(empty.library);
  free(empty.source_root);
  free(empty.sources);
  free(empty.args);
  free(library);
}
void run_audio_tests(void) {
  if (!root)
    root = check_temp("audio-tools-");
  if (!bin_dir)
    bin_dir = getenv("SKILL_TOOLS_BIN") ? strdup(getenv("SKILL_TOOLS_BIN"))
                                        : check_exe_dir();
  inventory = getenv("INVENTORY_BIN") ? strdup(getenv("INVENTORY_BIN"))
                                      : check_join(bin_dir, "qbittorrent-inventory");
  duplicate = getenv("DUPLICATE_BIN")
                  ? strdup(getenv("DUPLICATE_BIN"))
                  : check_join(bin_dir, "source-target-duplicate-check");
  libation = getenv("LIBATION_BIN") ? strdup(getenv("LIBATION_BIN"))
                                    : check_join(bin_dir, "libation-inventory");
  inventory_status();
  audiobook_filter();
  unsafe_names();
  resume_aliases();
  nul_names();
  inventory_errors();
  duplicate_precedence();
  duplicate_normalization();
  incomplete_scans();
  permission_failures();
  target_depth();
  libation_index();
  source_stdin();
  check_rm_rf(root);
  free(root);
  free(bin_dir);
  free(inventory);
  free(duplicate);
  free(libation);
  root = NULL;
  bin_dir = NULL;
}
