#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "audio.h"
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
Json *a_text(const char *s) {
  GString *text = g_string_new(NULL);
  for (const char *p = s; *p;) {
    gunichar c = g_utf8_get_char_validated(p, -1);
    if (c == (gunichar)-1 || c == (gunichar)-2) {
      unsigned v = 0xdc00 + (unsigned char)*p++;
      char bytes[] = {(char)(0xe0 | (v >> 12)), (char)(0x80 | ((v >> 6) & 63)),
                      (char)(0x80 | (v & 63))};
      g_string_append_len(text, bytes, 3);
    } else {
      const char *next = g_utf8_next_char(p);
      g_string_append_len(text, p, next - p);
      p = next;
    }
  }
  Json *out = json_text(text->str, text->len);
  g_string_free(text, TRUE);
  return out;
}
Json *a_int(const char *s) {
  Json *j = json_node(J_INT);
  j->string = g_string_new(s);
  return j;
}
Json *a_size(guint64 n) {
  char *s = g_strdup_printf("%" G_GUINT64_FORMAT, n);
  Json *j = a_int(s);
  g_free(s);
  return j;
}
Json *a_bool(bool b) {
  Json *j = json_node(J_BOOL);
  j->boolean = b;
  return j;
}
void a_put(Json *o, const char *key, Json *v) {
  json_set(o, key, strlen(key), v);
}
void a_add(Json *a, Json *v) { g_ptr_array_add(a->values, v); }
char *a_integer(const char *s) {
  if (!s)
    return NULL;
  while (g_ascii_isspace(*s))
    s++;
  bool negative = *s == '-';
  if (*s == '+' || *s == '-')
    s++;
  GString *n = g_string_new(NULL);
  bool digit = false;
  for (; *s && !g_ascii_isspace(*s); s++) {
    if (*s == '_' && digit && g_ascii_isdigit(s[1])) {
      digit = false;
      continue;
    }
    if (!g_ascii_isdigit(*s)) {
      g_string_free(n, TRUE);
      return NULL;
    }
    digit = true;
    g_string_append_c(n, *s);
  }
  while (g_ascii_isspace(*s))
    s++;
  if (*s || !digit || n->len > 4300) {
    g_string_free(n, TRUE);
    return NULL;
  }
  size_t first = 0;
  while (first + 1 < n->len && n->str[first] == '0')
    first++;
  g_string_erase(n, 0, (gssize)first);
  if (negative && strcmp(n->str, "0"))
    g_string_prepend_c(n, '-');
  return g_string_free(n, FALSE);
}
char *a_normal(const char *path) {
  GString *out = g_string_new(NULL);
  size_t lead = 0;
  while (path[lead] == '/')
    lead++;
  if (lead)
    g_string_append(out, lead == 2 ? "//" : "/");
  char **parts = g_strsplit(path + lead, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    if (out->len && out->str[out->len - 1] != '/')
      g_string_append_c(out, '/');
    g_string_append(out, parts[i]);
  }
  g_strfreev(parts);
  if (!out->len)
    g_string_append_c(out, '.');
  return g_string_free(out, FALSE);
}
char *a_join(const char *root, const char *name) {
  char *s = *name == '/' ? g_strdup(name) : g_strconcat(root, "/", name, NULL),
       *out = a_normal(s);
  g_free(s);
  return out;
}
static char *resolved(const char *path, bool strict, unsigned depth) {
  char *real = realpath(path, NULL);
  if (real) {
    char *out = g_strdup(real);
    free(real);
    return out;
  }
  if (strict)
    return NULL;
  char *cur = *path == '/' ? g_strdup("/") : g_get_current_dir();
  char **parts = g_strsplit(path, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    char *next = !strcmp(parts[i], "..")
                     ? g_path_get_dirname(cur)
                     : g_build_filename(cur, parts[i], NULL);
    g_free(cur);
    real = realpath(next, NULL);
    if (real)
      cur = g_strdup(real);
    else {
      struct stat st;
      if (depth < 40 && lstat(next, &st) == 0 && S_ISLNK(st.st_mode)) {
        char *link = g_file_read_link(next, NULL),
             *parent = g_path_get_dirname(next),
             *target = link ? a_join(parent, link) : NULL;
        cur = target ? resolved(target, false, depth + 1) : g_strdup(next);
        g_free(link);
        g_free(parent);
        g_free(target);
      } else
        cur = g_strdup(next);
    }
    free(real);
    g_free(next);
  }
  g_strfreev(parts);
  return cur;
}
char *a_resolve(const char *path, bool strict) {
  return resolved(path, strict, 0);
}
char *a_expand(const char *path) {
  if (*path != '~')
    return a_normal(path);
  const char *slash = strchr(path, '/');
  size_t n = slash ? (size_t)(slash - path) : strlen(path);
  char *home = NULL;
  if (n == 1) {
    const char *env = g_getenv("HOME");
    home = g_strdup(env ? env : g_get_home_dir());
  } else {
    char *user = g_strndup(path + 1, n - 1);
    struct passwd *pw = getpwnam(user);
    if (pw)
      home = g_strdup(pw->pw_dir);
    g_free(user);
  }
  if (!home)
    return NULL;
  char *out = slash ? a_join(home, slash + 1) : a_normal(home);
  g_free(home);
  return out;
}
char *a_lower(const char *s) {
  char *valid = g_utf8_make_valid(s, -1), *lower = g_utf8_strdown(valid, -1);
  g_free(valid);
  return lower;
}
static gunichar next_cp(const char **p) {
  gunichar c = g_utf8_get_char_validated(*p, -1);
  if (c == (gunichar)-1 || c == (gunichar)-2)
    return 0xdc00 + (unsigned char)*(*p)++;
  *p = g_utf8_next_char(*p);
  return c;
}
static gint compare(gconstpointer x, gconstpointer y) {
  const char *a = *(char *const *)x, *b = *(char *const *)y;
  while (*a && *b) {
    gunichar c = next_cp(&a), d = next_cp(&b);
    if (c != d)
      return c < d ? -1 : 1;
  }
  return *a ? 1 : *b ? -1 : 0;
}
GPtrArray *a_names(const char *path, bool sorted, int *error) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  DIR *dir = opendir(path);
  *error = 0;
  if (!dir) {
    *error = errno;
    return out;
  }
  struct dirent *entry;
  errno = 0;
  while ((entry = readdir(dir))) {
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
      g_ptr_array_add(out, g_strdup(entry->d_name));
    errno = 0;
  }
  *error = errno;
  closedir(dir);
  if (sorted)
    g_ptr_array_sort(out, compare);
  return out;
}
bool a_suffix(const char *path, bool all, bool cue) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  const char *dot = strrchr(base, '.');
  if (!dot || dot == base)
    return false;
  char *s = g_ascii_strdown(dot, -1);
  bool yes = !strcmp(s, ".m4b") || !strcmp(s, ".m4a") || !strcmp(s, ".mp3") ||
             !strcmp(s, ".flac") || !strcmp(s, ".ogg") || !strcmp(s, ".opus") ||
             (all && (!strcmp(s, ".aac") || !strcmp(s, ".wav"))) ||
             (cue && !strcmp(s, ".cue"));
  g_free(s);
  return yes;
}
bool a_size_tree(const char *path, guint64 *size, bool *audio) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  if (S_ISREG(st.st_mode)) {
    *size += (guint64)st.st_size;
    *audio = *audio || a_suffix(path, true, false);
    return true;
  }
  if (!S_ISDIR(st.st_mode))
    return true;
  int error;
  GPtrArray *names = a_names(path, false, &error);
  for (size_t i = 0; i < names->len; i++) {
    char *p = a_join(path, names->pdata[i]);
    struct stat info, link;
    if (stat(p, &info) == 0) {
      if (S_ISREG(info.st_mode)) {
        *size += (guint64)info.st_size;
        *audio = *audio || a_suffix(p, true, false);
      } else if (S_ISDIR(info.st_mode) && lstat(p, &link) == 0 &&
                 !S_ISLNK(link.st_mode)) {
        a_size_tree(p, size, audio);
      }
    }
    g_free(p);
  }
  g_ptr_array_free(names, TRUE);
  return true;
}
char *a_repr(const char *s) {
  char quote = strchr(s, '\'') && !strchr(s, '"') ? '"' : '\'';
  GString *out = g_string_new(NULL);
  g_string_append_c(out, quote);
  for (const char *p = s; *p;) {
    gunichar c = next_cp(&p);
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
      g_string_append_printf(out,
                             c <= 255     ? "\\x%02x"
                             : c <= 65535 ? "\\u%04x"
                                          : "\\U%08x",
                             c);
    } else
      g_string_append_unichar(out, c);
  }
  g_string_append_c(out, quote);
  return g_string_free(out, FALSE);
}
GString *a_json(Json *value) {
  char *s = json_encode(value);
  if (!s)
    return NULL;
  GString *out = g_string_new(s);
  g_string_append_c(out, '\n');
  g_free(s);
  return out;
}
bool a_output(const char *path, GString *data, bool binary) {
  if (!data)
    return false;
  FILE *out = path ? fopen(path, "wb") : stdout;
  if (!out)
    return false;
  bool ok = true;
  if (path && !binary && !g_utf8_validate(data->str, (gssize)data->len, NULL))
    ok = false;
  if (ok)
    ok = fwrite(data->str, 1, data->len, out) == data->len;
  if (path && fclose(out) != 0)
    ok = false;
  return ok;
}
char *a_error(const char *path, int error) {
  char *quote = a_repr(path), *out = g_strdup_printf("[Errno %d] %s: %s", error,
                                                     g_strerror(error), quote);
  g_free(quote);
  return out;
}
