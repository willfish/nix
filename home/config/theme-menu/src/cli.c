#define _DEFAULT_SOURCE
#include "theme.h"
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
static void usage(FILE *f) {
  fputs("usage: theme-menu --catalogue FILE --state DIR [--reapply] "
        "[--no-reload] [palette]\n",
        f);
}
int theme_run(int argc, char **argv, const char *greeter, const char *proc_root,
              int fail_at) {
  const char *catalogue_path = NULL, *state = NULL, *palette = NULL;
  bool reapply = false, no_reload = false;
  const struct option options[] = {{"catalogue", required_argument, NULL, 'c'},
                                   {"state", required_argument, NULL, 's'},
                                   {"reapply", no_argument, NULL, 'r'},
                                   {"no-reload", no_argument, NULL, 'n'},
                                   {"help", no_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
  optind = 1;
  int option;
  while ((option = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    switch (option) {
    case 'c':
      catalogue_path = optarg;
      break;
    case 's':
      state = optarg;
      break;
    case 'r':
      reapply = true;
      break;
    case 'n':
      no_reload = true;
      break;
    case 'h':
      usage(stdout);
      return 0;
    default:
      usage(stderr);
      return 2;
    }
  }
  if (!catalogue_path || !state || argc - optind > 1) {
    usage(stderr);
    return 2;
  }
  if (optind < argc)
    palette = argv[optind];
  const char *config = g_getenv("XDG_CONFIG_HOME");
  Themes t = {.state = path_join(*state ? state : ".", "."),
              .config = config ? path_join(*config ? config : ".", ".")
                               : path_join(g_get_home_dir(), ".config"),
              .greeter = greeter ? greeter : "/var/lib/desktop-theme/william",
              .proc_root = proc_root ? proc_root : "/proc",
              .warnings = g_ptr_array_new_with_free_func(g_free),
              .fail_at = fail_at};
  int lock = -1, status = 1;
  char *selected = NULL, *title = NULL, *label = NULL;
  char *json = read_text(&t, *catalogue_path ? catalogue_path : ".");
  if (json) {
    yyjson_read_err error;
    t.doc = yyjson_read_opts(json, strlen(json), YYJSON_READ_ALLOW_INF_AND_NAN,
                             NULL, &error);
    g_free(json);
    if (!t.doc)
      fail(&t, "Invalid theme catalogue: %s", error.msg);
    else {
      t.catalogue = yyjson_doc_get_root(t.doc);
      if (!yyjson_is_obj(t.catalogue) ||
          !yyjson_is_obj(field(t.catalogue, "palettes")))
        fail(&t, "Invalid theme catalogue");
    }
  }
  if (!t.error && g_mkdir_with_parents(t.state, 0777))
    fail(&t, "Could not create theme state directory");
  if (!t.error) {
    char *path = path_join(t.state, "lock");
    lock = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    g_free(path);
    if (lock < 0)
      fail(&t, "Could not open theme state lock");
    else if (flock(lock, LOCK_EX | LOCK_NB))
      fail(&t, "Another theme menu is open; close it and retry.");
  }
  if (!t.error)
    selected =
        reapply ? selection(&t) : (palette ? g_strdup(palette) : choose(&t));
  if (!selected && !t.error) {
    status = 0;
    goto cleanup;
  }
  if (!t.error) {
    if (!strcmp(selected, "light") || !strcmp(selected, "dark")) {
      if (set_mode(&t, selected)) {
        title = g_strdup("Appearance mode");
        label =
            g_strdup(!strcmp(selected, "light") ? "Light mode" : "Dark mode");
      }
    } else if (apply(&t, selected)) {
      title = g_strdup("Theme selected");
      label = scalar(field(resolve(&t, selected), "label"), "None");
      if (!no_reload)
        reload_apps(&t);
    }
    if (!t.error && !no_reload)
      publish_session(&t);
  }
  if (lock >= 0) {
    close(lock);
    lock = -1;
  }
  if (!t.error) {
    for (size_t i = 0; i < t.warnings->len; i++)
      fprintf(stderr, "theme-menu: %s\n", (char *)t.warnings->pdata[i]);
    if (!reapply && !no_reload) {
      GString *body = g_string_new(label);
      if (t.warnings->len) {
        g_string_append_c(body, '\n');
        for (size_t i = 0; i < t.warnings->len; i++) {
          if (i)
            g_string_append_c(body, '\n');
          g_string_append(body, t.warnings->pdata[i]);
        }
      }
      const char *args[] = {"notify-send", title, body->str, NULL};
      Command *c = command(args, NULL, 5000, true);
      if (c->failure || c->timed_out)
        checked(&t, c);
      command_free(c);
      g_string_free(body, true);
    }
    if (!t.error)
      status = 0;
  }
  if (t.error) {
    fprintf(stderr, "theme-menu: %s\n", t.error);
    if (!reapply && !no_reload) {
      const char *args[] = {"notify-send", "Theme selection failed", t.error,
                            NULL};
      Command *c = command(args, NULL, 5000, true);
      command_free(c);
    }
  }
cleanup:
  if (lock >= 0)
    close(lock);
  if (t.doc)
    yyjson_doc_free(t.doc);
  g_free(selected);
  g_free(title);
  g_free(label);
  g_free(t.error);
  g_free(t.state);
  g_free(t.config);
  g_ptr_array_free(t.warnings, true);
  return status;
}
