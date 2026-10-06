#define _POSIX_C_SOURCE 200809L
#include "adapt.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static char *normalize(const char *path) {
  GString *out = g_string_new(NULL);
  size_t leading = 0;
  while (path[leading] == '/')
    leading++;
  if (leading)
    g_string_append(out, leading == 2 ? "//" : "/");
  char **parts = g_strsplit(path + leading, "/", -1);
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
  return g_string_free(out, false);
}
static bool valid(const char *data, size_t length) {
  for (size_t at = 0; at < length;) {
    const char *nul = memchr(data + at, 0, length - at);
    size_t n = nul ? (size_t)(nul - (data + at)) : length - at;
    if (!g_utf8_validate(data + at, (gssize)n, NULL))
      return false;
    at += n + 1;
  }
  return true;
}
static GString *read_text(const char *path) {
  char *data = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &data, &length, NULL))
    return NULL;
  if (!valid(data, length)) {
    g_free(data);
    return NULL;
  }
  GString *out = g_string_sized_new(length);
  for (size_t i = 0; i < length; i++) {
    char c = data[i];
    if (c == '\r') {
      c = '\n';
      if (i + 1 < length && data[i + 1] == '\n')
        i++;
    }
    g_string_append_c(out, c);
  }
  g_free(data);
  return out;
}
static bool write_text(const char *path, GString *text) {
  FILE *file = fopen(path, "w");
  if (!file)
    return false;
  bool ok = fwrite(text->str, 1, text->len, file) == text->len;
  if (fclose(file) != 0)
    ok = false;
  return ok;
}
typedef struct {
  char *name;
  GArray *order;
} Name;
static void name_free(void *data) {
  Name *name = data;
  g_free(name->name);
  g_array_free(name->order, true);
  g_free(name);
}
static Name *name_new(const char *value) {
  Name *name = g_new0(Name, 1);
  name->name = g_strdup(value);
  name->order = g_array_new(false, false, sizeof(gunichar));
  for (const char *p = value; *p;) {
    gunichar c = g_utf8_get_char_validated(p, -1);
    if (c == (gunichar)-1 || c == (gunichar)-2) {
      c = 0xdc00 + (unsigned char)*p++;
    } else
      p = g_utf8_next_char(p);
    g_array_append_val(name->order, c);
  }
  return name;
}
static gint compare(gconstpointer first, gconstpointer second) {
  const Name *a = *(Name *const *)first, *b = *(Name *const *)second;
  size_t i = 0;
  for (; i < a->order->len && i < b->order->len; i++) {
    gunichar x = g_array_index(a->order, gunichar, i),
             y = g_array_index(b->order, gunichar, i);
    if (x != y)
      return x < y ? -1 : 1;
  }
  return a->order->len == b->order->len  ? 0
         : a->order->len < b->order->len ? -1
                                         : 1;
}
static GPtrArray *names(const char *source) {
  GPtrArray *out = g_ptr_array_new_with_free_func(name_free);
  DIR *dir = opendir(source);
  if (!dir)
    return out;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno)
        g_ptr_array_set_size(out, 0);
      break;
    }
    size_t length = strlen(entry->d_name);
    if (length >= 3 && !memcmp(entry->d_name + length - 3, ".md", 3))
      g_ptr_array_add(out, name_new(entry->d_name));
  }
  closedir(dir);
  g_ptr_array_sort(out, compare);
  return out;
}
int adapt_tree(const char *source, const char *destination, bool agent) {
  char *src = normalize(source), *dst = normalize(destination);
  int status = 1;
  if (g_mkdir_with_parents(dst, 0777) != 0)
    goto done;
  GPtrArray *files = names(src);
  status = 0;
  for (size_t i = 0; i < files->len; i++) {
    Name *name = files->pdata[i];
    char *path = g_strconcat(src, "/", name->name, NULL);
    GString *text = read_text(path);
    g_free(path);
    if (!text) {
      status = 1;
      break;
    }
    GString *converted = adapt_markdown((Text){text->str, text->len}, agent);
    path = g_strconcat(dst, "/", name->name, NULL);
    bool ok = write_text(path, converted);
    g_free(path);
    g_string_free(converted, true);
    g_string_free(text, true);
    if (!ok) {
      status = 1;
      break;
    }
  }
  g_ptr_array_free(files, true);
done:
  g_free(src);
  g_free(dst);
  return status;
}
