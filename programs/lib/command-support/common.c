#include "common.h"
#include <string.h>
char *decimal_text(const char *s) {
  GString *out = g_string_new(NULL);
  for (const char *p = s; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    int d = g_unichar_type(c) == G_UNICODE_DECIMAL_NUMBER
                ? g_unichar_digit_value(c)
                : -1;
    if (d >= 0)
      g_string_append_c(out, (char)('0' + d));
    else
      g_string_append_unichar(out, c);
  }
  return g_string_free(out, FALSE);
}
bool number_arg(const char *s, double *out) {
  if (!s || strpbrk(s, "\x1c\x1d\x1e\x1f"))
    return false;
  char *trim = strip(s), *ascii = decimal_text(trim);
  g_free(trim);
  bool ok =
      g_regex_match_simple("\\A[+-]?(?:(?:[0-9](?:_?[0-9])*(?:\\.(?:[0-9](?:_?["
                           "0-9])*)?)?|\\.[0-9](?:_?[0-9])*)(?:[eE][+-]?[0-9](?"
                           ":_?[0-9])*)?|[+-]?(?i:nan|inf(?:inity)?))\\z",
                           ascii, 0, 0);
  if (ok) {
    char *clean = replace_all(ascii, "_", "");
    *out = g_ascii_strtod(clean, NULL);
    g_free(clean);
  }
  g_free(ascii);
  return ok;
}
char *replace_all(const char *s, const char *from, const char *to) {
  char **parts = g_strsplit(s, from, -1);
  char *out = g_strjoinv(to, parts);
  g_strfreev(parts);
  return out;
}
