#include "config.h"
#include <stdio.h>
#include <string.h>

static void free_string(void *value) { g_string_free(value, true); }
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
static int auth_main(int argc, char **argv) {
  GPtrArray *drop = g_ptr_array_new_with_free_func(free_string);
  GString *provider = NULL;
  char *path = NULL, *refresh = NULL, *account = NULL;
  int status = 2;
  bool options = true, invalid = false;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (options && !strcmp(arg, "--")) {
      options = false;
      continue;
    }
    if (options && g_str_has_prefix(arg, "-h="))
      goto done;
    if (options && arg[0] == '-' && arg[1] == 'h' && arg[2] != '=') {
      puts("usage: pi-merge-auth [-h] [--drop PROVIDER] [--oauth-provider "
           "PROVIDER]\n                     [--refresh-file FILE] "
           "[--account-file FILE] auth_json");
      status = 0;
      goto done;
    }
    if (options && g_str_has_prefix(arg, "--")) {
      const char *equal = strchr(arg, '=');
      size_t length = equal ? (size_t)(equal - arg) : strlen(arg);
      const char *names[] = {"--drop", "--oauth-provider", "--refresh-file",
                             "--account-file", "--help"};
      int option = -1;
      for (int n = 0; n < 5; n++)
        if (length <= strlen(names[n]) && !strncmp(arg, names[n], length)) {
          if (option != -1)
            goto done;
          option = n;
        }
      if (option < 0) {
        invalid = true;
        continue;
      }
      if (option == 4) {
        if (equal)
          goto done;
        puts("usage: pi-merge-auth [-h] [--drop PROVIDER] [--oauth-provider "
             "PROVIDER]\n                     [--refresh-file FILE] "
             "[--account-file FILE] auth_json");
        status = 0;
        goto done;
      }
      const char *value = equal ? equal + 1 : (i + 1 < argc ? argv[++i] : NULL);
      if (!value)
        goto done;
      if (!equal && value[0] == '-' && value[1] && !negative_number(value))
        goto done;
      if (option == 0)
        g_ptr_array_add(drop, json_argument(value));
      if (option == 1) {
        if (provider)
          g_string_free(provider, true);
        provider = json_argument(value);
      }
      if (option == 2) {
        g_free(refresh);
        refresh = config_path(value);
      }
      if (option == 3) {
        g_free(account);
        account = config_path(value);
      }
    } else {
      if (options && arg[0] == '-' && arg[1] && !negative_number(arg)) {
        invalid = true;
        continue;
      }
      if (path) {
        invalid = true;
        continue;
      }
      path = config_path(arg);
    }
  }
  if (invalid || !path || (provider && provider->len && (!refresh || !account)))
    goto done;
  status = config_auth(path, drop, provider, refresh, account);
done:
  if (status == 2)
    fputs("usage: pi-merge-auth [--drop PROVIDER] [--oauth-provider PROVIDER "
          "--refresh-file FILE --account-file FILE] auth_json\n",
          stderr);
  if (provider)
    g_string_free(provider, true);
  g_ptr_array_free(drop, true);
  g_free(path);
  g_free(refresh);
  g_free(account);
  return status;
}
int main(int argc, char **argv) {
  int status;
#ifdef AUTH_MERGER
  status = auth_main(argc, argv);
#else
  (void)auth_main;
  if (argc != 3) {
    fputs("usage: pi-merge-settings <defaults.json> <settings.json>\n", stderr);
    return 2;
  }
  char *defaults = config_path(argv[1]), *settings = config_path(argv[2]);
  status = config_settings(defaults, settings);
  g_free(defaults);
  g_free(settings);
#endif
  if (status == 1)
    fputs("Pi configuration could not be updated.\n", stderr);
  return status;
}
