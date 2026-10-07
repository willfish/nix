#include "text.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}
static bool digit(char c) { return c >= '0' && c <= '9'; }
static bool numeric_label(const char *label) {
  if (g_str_has_prefix(label, "0x")) {
    for (const char *p = label + 2; *p; p++)
      if (!g_ascii_isxdigit(*p))
        return false;
    return true;
  }
  for (const char *p = label; *p; p++)
    if (!digit(*p))
      return false;
  return true;
}
static bool canonical_ipv4(const char *host) {
  const char *p = host;
  for (int i = 0; i < 4; i++) {
    const char *start = p;
    unsigned octet = 0;
    while (digit(*p)) {
      octet = octet * 10 + (unsigned)(*p++ - '0');
      if (p - start > 3 || octet > 255)
        return false;
    }
    if (p == start || (p - start > 1 && *start == '0'))
      return false;
    if (i < 3) {
      if (*p++ != '.')
        return false;
    } else if (*p)
      return false;
  }
  return true;
}
static bool path_allowed(const char *path) {
  for (const char *p = path; *p;) {
    if (*p == '\\' || *p == '?' || *p == '#')
      return false;
    gunichar c = g_utf8_get_char_validated(p, -1);
    if (c == (gunichar)-1 || c == (gunichar)-2) {
      // os.environ's surrogateescape maps invalid bytes to non-whitespace.
      p++;
      continue;
    }
    if (python_space(c))
      return false;
    p = g_utf8_next_char(p);
  }
  return true;
}
static char *bus_host(const char *url) {
  const char *start;
  if (g_str_has_prefix(url, "http://"))
    start = url + 7;
  else if (g_str_has_prefix(url, "https://"))
    start = url + 8;
  else
    return NULL;
  const char *end = start;
  while (alnum(*end) || *end == '.' || *end == '-')
    end++;
  if (end == start)
    return NULL;
  const char *tail = end;
  if (*tail == ':') {
    tail++;
    if (!digit(*tail))
      return NULL;
    while (digit(*tail))
      tail++;
  }
  if (*tail && (*tail != '/' || !path_allowed(tail + 1)))
    return NULL;
  char *host = g_ascii_strdown(start, (gssize)(end - start));
  const char *label = host, *last = host;
  for (const char *p = host;; p++) {
    if (*p && *p != '.')
      continue;
    size_t length = (size_t)(p - label);
    if (!length || length > 63 || !alnum(*label) || !alnum(p[-1])) {
      g_free(host);
      return NULL;
    }
    last = label;
    if (!*p)
      break;
    label = p + 1;
  }
  if (numeric_label(last) && !canonical_ipv4(host)) {
    g_free(host);
    return NULL;
  }
  return host;
}
int main(void) {
  const char *url = g_getenv("PI_AGENT_BUS_URL");
  char *host = bus_host(url && *url ? url : "http://terminus:7420");
  bool ok = puts(host ? host : "") >= 0 && fflush(stdout) == 0;
  g_free(host);
  return ok ? 0 : 1;
}
