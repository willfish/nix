#include "adapt.h"
#include <stdio.h>
#include <string.h>
static bool negative(const char *s) {
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
static int usage(void) {
  fputs("usage: opencode-adapt-markdown --kind {agent,command} source "
        "destination\n",
        stderr);
  return 2;
}
int adapt_cli(int argc, char **argv) {
  const char *source = NULL, *destination = NULL, *kind = NULL;
  bool options = true, invalid = false;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (options && !strcmp(arg, "--")) {
      options = false;
      continue;
    }
    if (options && g_str_has_prefix(arg, "-h="))
      return usage();
    if (options && arg[0] == '-' && arg[1] == 'h') {
      puts("usage: opencode-adapt-markdown [-h] --kind {agent,command} source "
           "destination");
      return 0;
    }
    if (options && g_str_has_prefix(arg, "--")) {
      const char *equal = strchr(arg, '=');
      size_t length = equal ? (size_t)(equal - arg) : strlen(arg);
      if (length <= 6 && !strncmp("--help", arg, length)) {
        if (equal)
          return usage();
        puts("usage: opencode-adapt-markdown [-h] --kind {agent,command} "
             "source destination");
        return 0;
      }
      if (length > 6 || strncmp("--kind", arg, length)) {
        invalid = true;
        continue;
      }
      kind = equal ? equal + 1 : i + 1 < argc ? argv[++i] : NULL;
      if (!kind || (strcmp(kind, "agent") && strcmp(kind, "command")))
        return usage();
    } else {
      if (options && arg[0] == '-' && arg[1] && !negative(arg)) {
        invalid = true;
        continue;
      }
      if (!source)
        source = arg;
      else if (!destination)
        destination = arg;
      else
        invalid = true;
    }
  }
  if (invalid || !kind || !source || !destination)
    return usage();
  int status = adapt_tree(source, destination, !strcmp(kind, "agent"));
  if (status)
    fputs("OpenCode Markdown adaptation failed.\n", stderr);
  return status;
}
int main(int argc, char **argv) { return adapt_cli(argc, argv); }
