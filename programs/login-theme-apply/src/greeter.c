#define _GNU_SOURCE
#include "greeter.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool greeter_id(const unsigned char *data, size_t length) {
  if (length && data[length - 1] == '\n')
    length--;
  if (!length)
    return false;
  bool segment = false;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = data[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      segment = true;
    else if (c == '-' && segment)
      segment = false;
    else
      return false;
  }
  return segment;
}
bool greeter_store(const char *path, size_t length) {
  if (!path || length < 11 || memcmp(path, "/nix/store/", 11) ||
      memchr(path, 0, length) || memchr(path, '\n', length))
    return false;
  size_t at = 0;
  while (at < length) {
    size_t end = at;
    while (end < length && path[end] != '/')
      end++;
    if (end - at == 2 && path[at] == '.' && path[at + 1] == '.')
      return false;
    at = end + 1;
  }
  return true;
}
int greeter_read(const char *directory, const char *name, char id[65]) {
  int parent;
  do {
    parent = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  } while (parent < 0 && errno == EINTR);
  if (parent < 0)
    return 0;
  int fd;
  do {
    fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  int result = 0;
  unsigned char data[65];
  ssize_t length = 0;
  if (fd >= 0) {
    struct stat info;
    if (fstat(fd, &info) < 0)
      result = -1;
    else if (S_ISREG(info.st_mode)) {
      do {
        length = read(fd, data, sizeof data);
      } while (length < 0 && errno == EINTR);
      if (length < 0)
        result = -1;
      else if (length <= 64 && greeter_id(data, (size_t)length))
        result = 1;
    }
    if (close(fd) < 0)
      result = -1;
  }
  if (close(parent) < 0)
    result = -1;
  if (result == 1) {
    size_t n = (size_t)length;
    if (data[n - 1] == '\n')
      n--;
    memcpy(id, data, n);
    id[n] = 0;
  }
  return result;
}
static yyjson_val *field_bytes(yyjson_val *object, const char *name,
                               size_t length) {
  yyjson_val *result = NULL, *key, *value;
  size_t i, n;
  yyjson_obj_foreach(object, i, n, key, value) {
    if (yyjson_get_len(key) == length &&
        !memcmp(yyjson_get_str(key), name, length))
      result = value;
  }
  return result;
}
static yyjson_val *field(yyjson_val *object, const char *name) {
  return field_bytes(object, name, strlen(name));
}
static char *filesystem(yyjson_val *value) {
  if (!yyjson_is_str(value))
    return NULL;
  const unsigned char *s = (const unsigned char *)yyjson_get_str(value);
  size_t n = yyjson_get_len(value);
  if (memchr(s, 0, n))
    return NULL;
  GString *out = g_string_new(NULL);
  // JSON can contain escaped lone surrogates. Filesystem surrogateescape maps
  // only U+DC80..U+DCFF to raw bytes; other surrogates remain unusable paths.
  for (size_t i = 0; i < n; i++) {
    if (s[i] == 0xed && i + 2 < n && (s[i + 1] & 0xe0) == 0xa0 &&
        (s[i + 2] & 0xc0) == 0x80) {
      unsigned cp =
          ((s[i] & 15) << 12) | ((s[i + 1] & 63) << 6) | (s[i + 2] & 63);
      if (cp < 0xdc80 || cp > 0xdcff) {
        g_string_free(out, true);
        return NULL;
      }
      g_string_append_c(out, (char)(cp - 0xdc00));
      i += 2;
    } else
      g_string_append_c(out, (char)s[i]);
  }
  return g_string_free(out, false);
}
static bool python_tokens(const char *data, size_t length) {
  // yyjson's non-finite option is case-insensitive and permits Inf/-NaN.
  // Retain only the Python spellings, plus its decimal integer digit limit.
  size_t at = 0;
  while (at < length) {
    unsigned char c = (unsigned char)data[at];
    if (c == '"') {
      at++;
      while (at < length) {
        if (data[at] == '\\') {
          at += 2;
          continue;
        }
        if (data[at++] == '"')
          break;
      }
      continue;
    }
    if (strchr("{}[]:, \t\r\n", c)) {
      at++;
      continue;
    }
    size_t start = at;
    while (at < length && !strchr("{}[]:, \t\r\n\"", data[at]))
      at++;
    if (at == start)
      return false;
    size_t n = at - start;
    const char *s = data + start;
    if ((n == 4 && !memcmp(s, "true", 4)) ||
        (n == 5 && !memcmp(s, "false", 5)) ||
        (n == 4 && !memcmp(s, "null", 4)) || (n == 3 && !memcmp(s, "NaN", 3)) ||
        (n == 8 && !memcmp(s, "Infinity", 8)) ||
        (n == 9 && !memcmp(s, "-Infinity", 9)))
      continue;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == n || s[i] < '0' || s[i] > '9')
      return false;
    size_t digits = 0;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
      digits++;
    if (i == n && digits > 4300)
      return false;
  }
  return true;
}
static unsigned escape_code(const char *s) {
  unsigned value = 0;
  for (size_t i = 0; i < 4; i++) {
    int digit = g_ascii_xdigit_value(s[i]);
    if (digit < 0)
      return 0;
    value = (value << 4) | (unsigned)digit;
  }
  return value;
}
static GString *escaped_surrogates(const char *data, size_t length) {
  GString *out = g_string_new(NULL);
  bool quoted = false;
  for (size_t i = 0; i < length;) {
    if (quoted && data[i] == '\\' && i + 1 < length) {
      if (data[i + 1] == 'u' && i + 6 <= length) {
        unsigned cp = escape_code(data + i + 2);
        bool pair = false;
        if (cp >= 0xd800 && cp <= 0xdbff && i + 12 <= length &&
            data[i + 6] == '\\' && data[i + 7] == 'u') {
          unsigned low = escape_code(data + i + 8);
          pair = low >= 0xdc00 && low <= 0xdfff;
        }
        if (pair) {
          g_string_append_len(out, data + i, 12);
          i += 12;
          continue;
        }
        if (cp >= 0xd800 && cp <= 0xdfff) {
          // yyjson permits literal WTF-8 with its Unicode option but still
          // rejects escaped lone surrogates, which Python JSON retains.
          char bytes[] = {(char)(0xe0 | (cp >> 12)),
                          (char)(0x80 | ((cp >> 6) & 63)),
                          (char)(0x80 | (cp & 63))};
          g_string_append_len(out, bytes, 3);
          i += 6;
          continue;
        }
      }
      g_string_append_len(out, data + i, 2);
      i += 2;
      continue;
    }
    if (data[i] == '"')
      quoted = !quoted;
    g_string_append_c(out, data[i++]);
  }
  return out;
}
static int publish(yyjson_val *manifest, const char *id, const char **error) {
  yyjson_val *themes = field(manifest, "themes"),
             *entry = id ? field(themes, id) : NULL;
  if (!entry || yyjson_is_null(entry)) {
    yyjson_val *fallback = field(manifest, "fallback");
    if (!yyjson_is_str(fallback))
      entry = NULL;
    else
      entry = field_bytes(themes, yyjson_get_str(fallback),
                          yyjson_get_len(fallback));
    if (!entry) {
      *error = "greeter fallback is not in the allowlist";
      return 1;
    }
  }
  if (!yyjson_is_obj(entry))
    return 1;
  yyjson_val *asset = field(entry, "path");
  if (!yyjson_is_str(asset) ||
      !greeter_store(yyjson_get_str(asset), yyjson_get_len(asset))) {
    *error = "refusing non-store theme asset";
    return 1;
  }
  char *directory = filesystem(field(manifest, "runtimeDir"));
  if (!directory)
    return 1;
  struct stat info;
  int status = 1;
  if (lstat(directory, &info) < 0)
    goto done;
  if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid()) {
    *error = "refusing unowned runtime directory";
    goto done;
  }
  if (info.st_mode & 0022) {
    *error = "refusing writable runtime directory";
    goto done;
  }
  char *temporary = g_strconcat(directory, "/.theme-XXXXXX", NULL);
  bool created = g_mkdtemp(temporary) != NULL;
  if (created) {
    char *link = g_strconcat(temporary, "/omarchy", NULL),
         *target = g_strconcat(directory, "/omarchy", NULL),
         *path = filesystem(asset);
    bool linked = false;
    if (path && symlink(path, link) == 0) {
      linked = true;
      if (rename(link, target) == 0)
        status = 0;
    }
    if (linked && status)
      unlink(link);
    if (rmdir(temporary) < 0)
      status = 1;
    g_free(path);
    g_free(link);
    g_free(target);
  }
  g_free(temporary);
done:
  g_free(directory);
  return status;
}
int greeter_launch(const char *path, const char **error) {
  *error = "Greeter theme selection failed.";
  char *data = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &data, &length, NULL))
    return 1;
  if (!g_utf8_validate(data, (gssize)length, NULL) ||
      !python_tokens(data, length)) {
    g_free(data);
    return 1;
  }
  GString *compatible = escaped_surrogates(data, length);
  yyjson_doc *doc = yyjson_read_opts(compatible->str, compatible->len,
                                     YYJSON_READ_ALLOW_INF_AND_NAN |
                                         YYJSON_READ_ALLOW_INVALID_UNICODE |
                                         YYJSON_READ_BIGNUM_AS_RAW,
                                     NULL, NULL);
  g_string_free(compatible, true);
  g_free(data);
  if (!doc)
    return 1;
  yyjson_val *manifest = yyjson_doc_get_root(doc);
  char *directory = filesystem(field(manifest, "selectionDir")),
       *name = filesystem(field(manifest, "selectionName"));
  int status = 1;
  if (directory && name) {
    char id[65];
    int read = greeter_read(directory, name, id);
    if (read >= 0)
      status = publish(manifest, read ? id : NULL, error);
  }
  g_free(directory);
  g_free(name);
  yyjson_doc_free(doc);
  return status;
}
static bool negative_number(const char *s) {
  if (*s++ != '-')
    return false;
  bool point = false, before = false, after = false;
  while (*s) {
    gunichar c = g_utf8_get_char_validated(s, -1);
    if (c == '.' && !point) {
      point = true;
      s++;
      continue;
    }
    if (c == (gunichar)-1 || c == (gunichar)-2 || !g_unichar_isdigit(c))
      return false;
    if (point)
      after = true;
    else
      before = true;
    s = g_utf8_next_char(s);
  }
  return point ? after : before;
}
int greeter_cli(int argc, char **argv) {
  const char *manifest = NULL;
  bool options = true, bad = false;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (options && !strcmp(arg, "--")) {
      options = false;
      continue;
    }
    const char *equal = options ? strchr(arg, '=') : NULL;
    size_t prefix = equal ? (size_t)(equal - arg) : 0;
    if (equal &&
        ((prefix == 2 && !strncmp(arg, "-h", 2)) ||
         (prefix >= 3 && prefix <= 6 && !strncmp("--help", arg, prefix)))) {
      fputs("usage: greeter-select [-h] manifest\n", stderr);
      return 2;
    }
    if (options && ((arg[0] == '-' && arg[1] == 'h') ||
                    (strlen(arg) >= 3 && strlen(arg) <= 6 &&
                     !strncmp("--help", arg, strlen(arg))))) {
      puts("usage: greeter-select [-h] manifest\n\npositional arguments:\n  "
           "manifest\n\noptions:\n  -h, --help  show this help message and "
           "exit");
      return 0;
    }
    if (options && arg[0] == '-' && arg[1] && !negative_number(arg)) {
      bad = true;
      continue;
    }
    if (manifest)
      bad = true;
    else
      manifest = arg;
  }
  if (bad || !manifest) {
    fputs("usage: greeter-select [-h] manifest\n", stderr);
    return 2;
  }
  const char *error;
  int status = greeter_launch(manifest, &error);
  if (status)
    fprintf(stderr, "%s\n", error);
  return status;
}
