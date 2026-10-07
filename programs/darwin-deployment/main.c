#define _DEFAULT_SOURCE
#include "tools.h"
#include <getopt.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
static int usage(const char *message) {
  fprintf(stderr, "darwin-%s: %s\n",
          MODE == 0   ? "health"
          : MODE == 1 ? "preflight"
                      : "deploy",
          message);
  return 2;
}
static void report(void *context, const char *message) {
  (void)context;
  puts(message);
  fflush(stdout);
}
int main(int argc, char **argv) {
  const char *config_path = NULL, *target_arg = NULL;
  bool record = false;
  const struct option options[] = {{"config", required_argument, NULL, 'c'},
                                   {"target", required_argument, NULL, 't'},
                                   {"record", no_argument, NULL, 'r'},
                                   {"help", no_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
  opterr = 0;
  int option;
  while ((option = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    if (option == 'h') {
      puts("usage: darwin-health --config FILE [--record]\n       "
           "darwin-preflight --config FILE --target STORE_PATH\n       "
           "darwin-deploy STORE_PATH");
      return 0;
    }
    if (option == 'c' && MODE < 2)
      config_path = optarg;
    else if (option == 't' && MODE == 1)
      target_arg = optarg;
    else if (option == 'r' && MODE == 0)
      record = true;
    else
      return usage("invalid arguments");
  }
  if (MODE == 2) {
    if (argc - optind != 1)
      return usage("target required");
    if (geteuid() != 0)
      return usage("deployment requires explicit sudo authorization");
    target_arg = argv[optind];
  } else if (argc != optind || !config_path || (MODE == 1 && !target_arg))
    return usage("configuration and target arguments required");
  char *target = target_arg ? resolve_path(target_arg, MODE == 2) : NULL;
  if (MODE > 0 && (!target || !g_str_has_prefix(target, "/nix/store/"))) {
    g_free(target);
    return usage("target must be an already-built Nix store path");
  }
  if (MODE == 2) {
    const char *names[] = {"darwin-preflight", "darwin-rebuild"};
    for (size_t i = 0; i < 2; i++) {
      char *p = g_build_filename(target, "sw/bin", names[i], NULL);
      bool ok = access(p, X_OK) == 0;
      g_free(p);
      if (!ok) {
        g_free(target);
        return usage("target lacks required deployment tooling");
      }
    }
  }
  char *contract =
      MODE == 2 ? g_build_filename(target, "etc/dotfiles/server.json", NULL)
                : NULL;
  yyjson_doc *doc = load(MODE == 2 ? contract : config_path);
  g_free(contract);
  if (!doc) {
    g_free(target);
    fputs("Darwin configuration could not be read.\n", stderr);
    return 1;
  }
  Val *config = yyjson_doc_get_root(doc);
  int status = 1;
  if (MODE == 0) {
    Doc *result = health_snapshot(config, command, NULL);
    if (result) {
      if (record && geteuid() != 0) {
        status = usage("recording and log retention require root");
      } else if (!record || health_record(config, result)) {
        char *s = json(result, false);
        if (s) {
          puts(s);
          free(s);
          status = yyjson_mut_get_bool(yyjson_mut_obj_get(
                       yyjson_mut_doc_get_root(result), "healthy"))
                       ? 0
                       : 1;
        }
      } else
        fputs("Darwin health recording failed.\n", stderr);
      yyjson_mut_doc_free(result);
    } else
      fputs("Darwin health configuration is invalid.\n", stderr);
  } else if (MODE == 1) {
    char *diff = metadata_diff(config);
    const char *user = text(field(config, "user"));
    struct utsname os;
    bool valid = uname(&os) == 0;
    if (!diff || !user || !valid) {
      fputs("Darwin preflight configuration is invalid.\n", stderr);
    } else {
      puts(diff);
      struct passwd *account = getpwnam(user);
      char *account_home = account ? g_strdup(account->pw_dir) : NULL;
      Identity id = {os.sysname, os.machine, account != NULL,
                     account ? account->pw_uid : 0, account_home};
      bool error = false;
      GPtrArray *errors =
          preflight_problems(config, &id, command, NULL, &error);
      for (size_t i = 0; i < errors->len; i++)
        puts(errors->pdata[i]);
      status = error || errors->len ? 1 : 0;
      if (error)
        fputs("Darwin preflight command failed.\n", stderr);
      g_ptr_array_free(errors, TRUE);
      g_free(account_home);
    }
    g_free(diff);
  } else {
    const char *home = text(field(config, "homeGenerationProfile"));
    if (home &&
        deploy_system(target, "/nix/var/nix/profiles/system",
                      "/run/current-system", "/var/db/dotfiles/rollouts", home,
                      command, NULL, report))
      status = 0;
    else
      fputs("Darwin deployment failed.\n", stderr);
  }
  yyjson_doc_free(doc);
  g_free(target);
  return status;
}
