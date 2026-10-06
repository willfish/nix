#include "usage.h"
#include <ctype.h>
#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *allocate(size_t n) {
  void *p = calloc(1, n ? n : 1);
  if (!p) {
    fputs("agent-usage: allocation failed\n", stderr);
    exit(1);
  }
  return p;
}
char *join(const char *a, const char *b) {
  size_t n = strlen(a) + strlen(b) + 2;
  char *s = allocate(n);
  snprintf(s, n, "%s/%s", a, b);
  return s;
}
const char *home_dir(void) {
  const char *s = getenv("HOME");
  if (s && *s)
    return s;
  struct passwd *p = getpwuid(getuid());
  return p ? p->pw_dir : ".";
}
char *agent_dir(void) {
  const char *p = getenv("PI_AGENT_DIR");
  if (!p || !*p)
    return join(home_dir(), ".pi/agent");
  if (!strcmp(p, "~"))
    return strdup(home_dir());
  if (!strncmp(p, "~/", 2))
    return join(home_dir(), p + 2);
  return strdup(p);
}
Val *get(Val *v, const char *key) { return yyjson_mut_obj_get(v, key); }
bool truth(Val *v) {
  if (!v || yyjson_mut_is_null(v))
    return false;
  if (yyjson_mut_is_bool(v))
    return yyjson_mut_get_bool(v);
  if (yyjson_mut_is_num(v))
    return yyjson_mut_get_num(v) != 0;
  if (yyjson_mut_is_str(v))
    return yyjson_mut_get_len(v) != 0;
  if (yyjson_mut_is_arr(v))
    return yyjson_mut_arr_size(v) != 0;
  return yyjson_mut_obj_size(v) != 0;
}
const char *text(Val *v) {
  if (!v || yyjson_mut_is_null(v))
    return "";
  if (yyjson_mut_is_str(v))
    return yyjson_mut_get_str(v);
  if (yyjson_mut_is_bool(v))
    return yyjson_mut_get_bool(v) ? "True" : "False";
  static char buffers[16][64];
  static unsigned next;
  char *s = buffers[next++ % 16];
  if (yyjson_mut_is_int(v))
    snprintf(s, 64, "%lld", (long long)yyjson_mut_get_sint(v));
  else if (yyjson_mut_is_real(v))
    snprintf(s, 64, "%.17g", yyjson_mut_get_real(v));
  else
    return "";
  return s;
}
void put(Doc *d, Val *o, const char *key, Val *v) {
  yyjson_mut_obj_put(o, yyjson_mut_strcpy(d, key), v ? v : yyjson_mut_null(d));
}
void strput(Doc *d, Val *o, const char *key, const char *s) {
  put(d, o, key, yyjson_mut_strcpy(d, s ? s : ""));
}
void intput(Doc *d, Val *o, const char *key, int64_t n) {
  put(d, o, key, yyjson_mut_sint(d, n));
}
Val *copy(Doc *d, Val *v) {
  return v ? yyjson_mut_val_mut_copy(d, v) : yyjson_mut_obj(d);
}
Val *parse(Doc *d, const char *s) { return parse_bytes(d, s, strlen(s)); }
Val *parse_bytes(Doc *d, const char *s, size_t len) {
  if (memchr(s, 0, len))
    return NULL;
  // Session logs and HTTP bodies previously used UTF-8 replacement decoding.
  char *valid = g_utf8_make_valid(s, (gssize)len);
  yyjson_doc *r =
      yyjson_read(valid, strlen(valid), YYJSON_READ_ALLOW_INF_AND_NAN);
  g_free(valid);
  if (!r)
    return NULL;
  Val *v = yyjson_val_mut_copy(d, yyjson_doc_get_root(r));
  yyjson_doc_free(r);
  return v;
}
Val *load(Doc *d, const char *path) {
  yyjson_doc *r =
      yyjson_read_file(path, YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL);
  if (!r)
    return NULL;
  Val *v = yyjson_val_mut_copy(d, yyjson_doc_get_root(r));
  yyjson_doc_free(r);
  return v;
}
char *encode(Val *v) { return yyjson_mut_val_write(v, 0, NULL); }
static bool text_space(gunichar c) {
  return g_unichar_isspace(c) || c == '\v' || c == 0x85 ||
         (c >= 0x1c && c <= 0x1f);
}
char *strip_text(char *s) {
  char *begin = s, *end = s + strlen(s);
  while (begin < end &&
         text_space(g_utf8_get_char_validated(begin, end - begin)))
    begin = g_utf8_next_char(begin);
  while (end > begin) {
    char *previous = g_utf8_find_prev_char(begin, end);
    if (!previous ||
        !text_space(g_utf8_get_char_validated(previous, end - previous)))
      break;
    end = previous;
  }
  size_t length = (size_t)(end - begin);
  memmove(s, begin, length);
  s[length] = 0;
  return s;
}
static double numeric(Val *v, bool percent, bool *valid) {
  if (yyjson_mut_is_num(v)) {
    *valid = true;
    return yyjson_mut_get_num(v);
  }
  if (!yyjson_mut_is_str(v)) {
    *valid = false;
    return 0;
  }
  char *s = strdup(text(v));
  if (percent)
    strip_text(s);
  char *out = s;
  for (const char *p = s; *p; p++)
    if (!percent || *p != '%')
      *out++ = *p;
  *out = 0;
  // float() rejects these separators, although explicit str.strip() removes
  // them.
  if (strpbrk(s, "\x1c\x1d\x1e\x1f")) {
    free(s);
    *valid = false;
    return 0;
  }
  strip_text(s);
  char *end;
  double n = strtod(s, &end);
  *valid = end != s && !*end;
  free(s);
  return n;
}
int64_t number(Val *v) {
  if (yyjson_mut_is_bool(v))
    return yyjson_mut_get_bool(v);
  bool valid;
  double n = numeric(v, false, &valid);
  if (!valid || !isfinite(n) || n >= 0x1p63 || n < -0x1p63)
    return 0;
  // nearbyint matches Python's ties-to-even round, not C's
  // round-away-from-zero.
  return (int64_t)nearbyint(n);
}
double fraction(Val *v, bool percent) {
  bool valid;
  double n = numeric(v, true, &valid);
  if (!valid || isnan(n) || n < 0)
    return -1;
  if (percent || n > 1)
    n /= 100;
  return fmin(1, n);
}
const char *iso_time(Doc *d, double seconds, bool z) {
  time_t whole = (time_t)floor(seconds);
  long micros = lround((seconds - floor(seconds)) * 1000000);
  if (micros == 1000000) {
    whole++;
    micros = 0;
  }
  struct tm t;
  if (!gmtime_r(&whole, &t))
    return "";
  char base[48], s[80];
  strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &t);
  if (micros && !z)
    snprintf(s, sizeof s, "%s.%06ld+00:00", base, micros);
  else
    snprintf(s, sizeof s, "%s%s", base, z ? "Z" : "+00:00");
  return yyjson_mut_get_str(yyjson_mut_strcpy(d, s));
}
static bool digits(const char *s) {
  if (!*s)
    return false;
  while (*s)
    if (!isdigit((unsigned char)*s++))
      return false;
  return true;
}
const char *iso_timestamp(Doc *d, Val *v) {
  if (!truth(v))
    return "";
  const char *s = text(v);
  if (yyjson_mut_is_str(v)) {
    char *clean = strip_text(strdup(s));
    bool is_number = digits(clean);
    free(clean);
    if (!is_number)
      return s;
  }
  bool valid;
  double n = numeric(v, false, &valid);
  if (yyjson_mut_is_bool(v)) {
    valid = true;
    n = yyjson_mut_get_bool(v);
  }
  if (!valid)
    return s;
  if (n > 10000000000.0)
    n /= 1000;
  if (!isfinite(n) || n <= 0 || n > 253402300799.0)
    return "";
  return iso_time(d, n, false);
}
void local_day(Val *v, double now, char out[11]) {
  time_t instant = (time_t)now;
  bool valid;
  double n = numeric(v, false, &valid);
  char *clean = yyjson_mut_is_str(v) ? strip_text(strdup(text(v))) : NULL;
  bool numeric_string = clean && digits(clean);
  if (numeric_string) {
    n = strtod(clean, NULL);
    valid = true;
  }
  if (yyjson_mut_is_bool(v)) {
    valid = true;
    n = yyjson_mut_get_bool(v);
  }
  if (yyjson_mut_is_num(v) || yyjson_mut_is_bool(v) || numeric_string) {
    if (n > 10000000000.0)
      n /= 1000;
    if (valid && isfinite(n) && n > -62135596800.0 && n < 253402300800.0)
      instant = (time_t)n;
  } else if (clean && *clean) {
    struct tm parsed = {.tm_isdst = -1};
    const char *s = clean;
    char *end = strptime(s, "%Y-%m-%d", &parsed);
    if (end) {
      bool date_ok =
          parsed.tm_mon >= 0 && parsed.tm_mon <= 11 && parsed.tm_mday >= 1;
      int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      int y = parsed.tm_year + 1900;
      if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)
        lengths[1]++;
      date_ok = date_ok && parsed.tm_mday <= lengths[parsed.tm_mon];
      if (*end == 'T' || *end == ' ')
        end = strptime(end + 1, "%H:%M:%S", &parsed);
      if (end && *end == '.') {
        do {
          end++;
        } while (isdigit((unsigned char)*end));
      }
      if (end && date_ok) {
        if (*end == 'Z' && !end[1])
          instant = timegm(&parsed);
        else if (*end == '+' || *end == '-') {
          int h = 0, m = 0;
          char sign = *end;
          if (sscanf(end + 1, "%2d:%2d", &h, &m) >= 1 && h < 24 && m < 60)
            instant =
                timegm(&parsed) - (sign == '+' ? 1 : -1) * (h * 3600 + m * 60);
        } else if (!*end)
          instant = mktime(&parsed);
      }
    }
  }
  free(clean);
  struct tm t;
  if (!localtime_r(&instant, &t))
    localtime_r(&(time_t){(time_t)now}, &t);
  strftime(out, 11, "%Y-%m-%d", &t);
}
