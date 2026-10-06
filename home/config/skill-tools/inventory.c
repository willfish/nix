#include "audio.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  const char *data;
  size_t size, at;
} Decoder;
static Json *decode(Decoder *p, unsigned depth) {
  if (p->at >= p->size || depth > 512)
    return NULL;
  char c = p->data[p->at++];
  if (c == 'i') {
    const char *end = memchr(p->data + p->at, 'e', p->size - p->at);
    if (!end)
      return NULL;
    char *raw = g_strndup(p->data + p->at, (size_t)(end - p->data - p->at));
    char *number =
        strlen(raw) == (size_t)(end - p->data - p->at) ? a_integer(raw) : NULL;
    g_free(raw);
    if (!number)
      return NULL;
    p->at = (size_t)(end - p->data) + 1;
    Json *v = a_int(number);
    g_free(number);
    return v;
  }
  if (c == 'l' || c == 'd') {
    Json *out = json_node(c == 'l' ? J_ARRAY : J_OBJECT);
    while (p->at < p->size && p->data[p->at] != 'e') {
      Json *key = decode(p, depth + 1);
      if (!key)
        goto invalid;
      if (c == 'l')
        a_add(out, key);
      else {
        if (key->kind != J_STRING) {
          json_free(key);
          goto invalid;
        }
        Json *value = decode(p, depth + 1);
        if (!value) {
          json_free(key);
          goto invalid;
        }
        json_set(out, key->string->str, key->string->len, value);
        json_free(key);
      }
    }
    if (p->at >= p->size)
      goto invalid;
    p->at++;
    return out;
  invalid:
    json_free(out);
    return NULL;
  }
  if (g_ascii_isdigit(c)) {
    size_t start = --p->at;
    const char *colon = memchr(p->data + start, ':', p->size - start);
    if (!colon)
      return NULL;
    char *raw = g_strndup(p->data + start, (size_t)(colon - p->data - start)),
         *length = a_integer(raw);
    g_free(raw);
    if (!length)
      return NULL;
    char *end;
    guint64 n = g_ascii_strtoull(length, &end, 10);
    bool ok = *length != '-' && !*end;
    g_free(length);
    p->at = (size_t)(colon - p->data) + 1;
    if (!ok || n > p->size - p->at)
      return NULL;
    Json *v = json_text(p->data + p->at, (size_t)n);
    p->at += (size_t)n;
    return v;
  }
  return NULL;
}
static Json *get(Json *v, const char *key) {
  return json_get(v, key, strlen(key));
}
static bool truth(Json *v) {
  return v && ((v->kind == J_STRING && v->string->len) ||
               (v->kind == J_INT && strcmp(v->string->str, "0")) ||
               ((v->kind == J_ARRAY || v->kind == J_OBJECT) && v->values->len));
}
static void byte_repr(GString *out, GString *s) {
  char quote =
      memchr(s->str, '\'', s->len) && !memchr(s->str, '"', s->len) ? '"' : '\'';
  g_string_append_c(out, 'b');
  g_string_append_c(out, quote);
  for (size_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->str[i];
    if (c == '\\' || c == (unsigned char)quote) {
      g_string_append_c(out, '\\');
      g_string_append_c(out, (char)c);
    } else if (c == '\n')
      g_string_append(out, "\\n");
    else if (c == '\r')
      g_string_append(out, "\\r");
    else if (c == '\t')
      g_string_append(out, "\\t");
    else if (c < 32 || c >= 127)
      g_string_append_printf(out, "\\x%02x", c);
    else
      g_string_append_c(out, (char)c);
  }
  g_string_append_c(out, quote);
}
static void representation(GString *out, Json *v) {
  if (v->kind == J_STRING)
    byte_repr(out, v->string);
  else if (v->kind == J_INT)
    g_string_append(out, v->string->str);
  else {
    g_string_append_c(out, v->kind == J_ARRAY ? '[' : '{');
    for (size_t i = 0; i < v->values->len; i++) {
      if (i)
        g_string_append(out, ", ");
      if (v->kind == J_OBJECT) {
        byte_repr(out, v->keys->pdata[i]);
        g_string_append(out, ": ");
      }
      representation(out, v->values->pdata[i]);
    }
    g_string_append_c(out, v->kind == J_ARRAY ? ']' : '}');
  }
}
static char *as_text(Json *v, bool *invalid) {
  if (!v)
    return NULL;
  if (v->kind == J_STRING) {
    if (strlen(v->string->str) != v->string->len) {
      *invalid = true;
      return NULL;
    }
    return g_strdup(v->string->str);
  }
  GString *out = g_string_new(NULL);
  representation(out, v);
  return g_string_free(out, FALSE);
}
static char *as_int(Json *v) {
  char *s = NULL;
  if (v && (v->kind == J_STRING || v->kind == J_INT) &&
      strlen(v->string->str) == v->string->len)
    s = a_integer(v->string->str);
  return s ? s : g_strdup("0");
}
static bool positive(const char *s) { return *s != '-' && strcmp(s, "0"); }
typedef struct {
  char *name, *save, *path, *finished, *downloaded, *fr;
  const char *status;
  bool exists, paused, audio, size_known;
  guint64 size;
} Record;
static void record_free(gpointer p) {
  Record *r = p;
  g_free(r->name);
  g_free(r->save);
  g_free(r->path);
  g_free(r->finished);
  g_free(r->downloaded);
  g_free(r->fr);
  g_free(r);
}
static Record *parse(const char *fr, const char *fallback, bool *fatal) {
  char *raw = NULL;
  gsize n;
  if (!g_file_get_contents(fr, &raw, &n, NULL))
    return NULL;
  Decoder parser = {raw, n, 0};
  Json *data = decode(&parser, 0);
  g_free(raw);
  if (!data || data->kind != J_OBJECT) {
    json_free(data);
    return NULL;
  }
  Json *name = get(data, "name"), *save = get(data, "qBt-savePath");
  if (!truth(name))
    name = get(data, "qBt-name");
  if (!truth(save))
    save = get(data, "save_path");
  Record *r = g_new0(Record, 1);
  r->name = as_text(name, fatal);
  char *saved = as_text(save, fatal);
  r->save = a_normal(saved && *saved ? saved : fallback);
  g_free(saved);
  if (*fatal || !r->name || !*r->name || *r->name == '/' || *r->name == '\\')
    goto invalid;
  char **parts = g_strsplit(r->name, "/", -1);
  bool traversal = false;
  for (size_t i = 0; parts[i]; i++)
    if (!strcmp(parts[i], ".."))
      traversal = true;
  g_strfreev(parts);
  if (traversal)
    goto invalid;
  char *candidate = a_join(r->save, r->name), *root = a_resolve(r->save, false);
  r->path = a_resolve(candidate, false);
  g_free(candidate);
  bool inside = !strcmp(root, "/") ||
                (!strncmp(r->path, root, strlen(root)) &&
                 (r->path[strlen(root)] == '/' || r->path[strlen(root)] == 0));
  g_free(root);
  if (!inside)
    goto invalid;
  r->finished = as_int(get(data, "finished_time"));
  r->downloaded = as_int(get(data, "total_downloaded"));
  char *paused = as_int(get(data, "paused"));
  r->paused = strcmp(paused, "0") != 0;
  g_free(paused);
  struct stat st;
  r->exists = stat(r->path, &st) == 0;
  r->size_known = r->exists && a_size_tree(r->path, &r->size, &r->audio);
  char *lower = a_lower(r->name);
  const char *keywords[] = {"audiobook",     "unabridged", "abridged",   "m4b",
                            "great courses", "bbc r4",     "radio drama"};
  const char *suffixes[] = {".m4b", ".m4a",  ".mp3", ".flac",
                            ".ogg", ".opus", ".aac", ".wav"};
  for (size_t i = 0; i < G_N_ELEMENTS(suffixes); i++)
    r->audio = r->audio || g_str_has_suffix(lower, suffixes[i]);
  for (size_t i = 0; i < G_N_ELEMENTS(keywords); i++)
    r->audio = r->audio || strstr(lower, keywords[i]);
  g_free(lower);
  Json *pieces = get(data, "pieces");
  int complete = -1;
  if (pieces && pieces->kind == J_STRING && pieces->string->len) {
    complete = 1;
    for (size_t i = 0; i < pieces->string->len; i++) {
      unsigned char byte = (unsigned char)pieces->string->str[i];
      if (i + 1 < pieces->string->len && byte != 255) {
        complete = 0;
        break;
      }
      if (i + 1 == pieces->string->len)
        complete = byte == 255 ? 1 : byte == 0 ? 0 : -1;
    }
  }
  r->status =
      !r->exists ? "missing_path"
      : positive(r->finished) || (complete == 1 && positive(r->downloaded))
          ? "complete"
      : complete == 0 || !positive(r->downloaded) ? "incomplete"
                                                  : "unknown";
  r->fr = g_strdup(fr);
  json_free(data);
  return r;
invalid:
  json_free(data);
  record_free(r);
  return NULL;
}
static Json *record_json(Record *r) {
  Json *o = json_node(J_OBJECT);
  a_put(o, "name", a_text(r->name));
  a_put(o, "save_path", a_text(r->save));
  a_put(o, "path", a_text(r->path));
  a_put(o, "exists", a_bool(r->exists));
  a_put(o, "status", a_text(r->status));
  a_put(o, "finished_time", a_int(r->finished));
  a_put(o, "paused", a_bool(r->paused));
  a_put(o, "total_downloaded", a_int(r->downloaded));
  a_put(o, "size_bytes", r->size_known ? a_size(r->size) : json_node(J_NULL));
  a_put(o, "is_audiobook_like", a_bool(r->audio));
  a_put(o, "fastresume", a_text(r->fr));
  return o;
}
static char *commas(guint64 size) {
  char *s = g_strdup_printf("%" G_GUINT64_FORMAT, size);
  GString *out = g_string_new(NULL);
  size_t n = strlen(s);
  for (size_t i = 0; i < n; i++) {
    if (i && (n - i) % 3 == 0)
      g_string_append_c(out, ',');
    g_string_append_c(out, s[i]);
  }
  g_free(s);
  return g_string_free(out, FALSE);
}
static GString *markdown(GPtrArray *records) {
  const char *dash = "\342\200\224";
  GString *out = g_string_new("# qBittorrent import candidates\n\n");
  g_string_append_printf(
      out,
      "Total torrents inventoried: **%u**\n\n| Name | Status | Exists | Size | "
      "Audiobook-like | Path |\n|---|---|---|---|---|---|\n",
      records->len);
  size_t counts[3] = {0};
  const char *statuses[] = {"complete", "incomplete", "missing_path"};
  for (size_t i = 0; i < records->len; i++) {
    Record *r = records->pdata[i];
    char *size = r->size_known ? commas(r->size) : g_strdup(dash);
    g_string_append_printf(out, "| %s | %s | %s | %s | %s | `%s` |\n", r->name,
                           r->status, r->exists ? "True" : "False", size,
                           r->audio ? "True" : "False", r->path);
    g_free(size);
    for (size_t j = 0; j < 3; j++)
      if (!strcmp(r->status, statuses[j]))
        counts[j]++;
  }
  g_string_append_printf(
      out,
      "\n## Disposition hints\n\n- **import candidate (complete + present):** "
      "%zu\n- **incomplete (do not copy yet):** %zu\n- **missing path (skip / "
      "re-check):** %zu\n",
      counts[0], counts[1], counts[2]);
  const char *titles[] = {"Import candidates", "Incomplete", "Missing path"};
  for (size_t j = 0; j < 3; j++) {
    if (!counts[j])
      continue;
    g_string_append_printf(out, "\n### %s\n", titles[j]);
    for (size_t i = 0; i < records->len; i++) {
      Record *r = records->pdata[i];
      if (strcmp(r->status, statuses[j]))
        continue;
      if (j == 0)
        g_string_append_printf(out, "- `%s` %s `%s`\n", r->name, dash, r->path);
      else if (j == 1)
        g_string_append_printf(out, "- `%s` %s status=%s\n", r->name, dash,
                               r->status);
      else
        g_string_append_printf(out, "- `%s` %s expected `%s`\n", r->name, dash,
                               r->path);
    }
  }
  return out;
}
int main(int argc, char **argv) {
  const struct option options[] = {
      {"fastresume-dir", required_argument, NULL, 'f'},
      {"download-root", required_argument, NULL, 'd'},
      {"audiobook-only", no_argument, NULL, 'a'},
      {"transfer-ready-only", no_argument, NULL, 't'},
      {"format", required_argument, NULL, 'm'},
      {"output", required_argument, NULL, 'o'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0}};
  char *fr = a_join(g_getenv("HOME") ? g_getenv("HOME") : g_get_home_dir(),
                    ".local/share/qBittorrent/BT_backup"),
       *root = a_join(g_getenv("HOME") ? g_getenv("HOME") : g_get_home_dir(),
                      "Downloads");
  const char *format = "markdown", *output = NULL;
  bool only_audio = false, ready = false;
  int opt, status = 2;
  opterr = 0;
  while ((opt = getopt_long(argc, argv, "ho:", options, NULL)) != -1) {
    switch (opt) {
    case 'f':
      g_free(fr);
      fr = a_normal(optarg);
      break;
    case 'd':
      g_free(root);
      root = a_normal(optarg);
      break;
    case 'a':
      only_audio = true;
      break;
    case 't':
      ready = true;
      break;
    case 'm':
      format = optarg;
      break;
    case 'o':
      output = optarg;
      break;
    case 'h':
      puts("usage: qbittorrent-inventory [--fastresume-dir DIR] "
           "[--download-root DIR]\n       [--audiobook-only] "
           "[--transfer-ready-only] [--format markdown|json|nul|names] [-o "
           "FILE]");
      status = 0;
      goto done;
    default:
      goto done;
    }
  }
  if (optind != argc ||
      (!g_str_equal(format, "markdown") && !g_str_equal(format, "json") &&
       !g_str_equal(format, "nul") && !g_str_equal(format, "names")))
    goto done;
  GPtrArray *records = g_ptr_array_new_with_free_func(record_free);
  int error;
  GPtrArray *names = a_names(fr, true, &error);
  bool fatal = false;
  for (size_t i = 0; i < names->len && !fatal; i++) {
    const char *name = names->pdata[i];
    if (!g_str_has_suffix(name, ".fastresume"))
      continue;
    char *path = a_join(fr, name);
    Record *r = parse(path, root, &fatal);
    g_free(path);
    if (!r)
      continue;
    if ((only_audio && !r->audio) || (ready && strcmp(r->status, "complete")))
      record_free(r);
    else
      g_ptr_array_add(records, r);
  }
  g_ptr_array_free(names, TRUE);
  GString *payload = NULL;
  if (!fatal) {
    if (!strcmp(format, "json")) {
      Json *array = json_node(J_ARRAY);
      for (size_t i = 0; i < records->len; i++)
        a_add(array, record_json(records->pdata[i]));
      payload = a_json(array);
      json_free(array);
    } else if (!strcmp(format, "markdown"))
      payload = markdown(records);
    else {
      payload = g_string_new(NULL);
      for (size_t i = 0; i < records->len; i++) {
        Record *r = records->pdata[i];
        g_string_append(payload, r->name);
        g_string_append_c(payload, !strcmp(format, "nul") ? 0 : '\n');
      }
    }
  }
  status = !fatal && a_output(output, payload, !strcmp(format, "nul")) ? 0 : 1;
  if (payload)
    g_string_free(payload, TRUE);
  g_ptr_array_free(records, TRUE);
done:
  g_free(fr);
  g_free(root);
  if (status)
    fputs(status == 2 ? "Invalid inventory arguments.\n"
                      : "Inventory failed.\n",
          stderr);
  return status;
}
