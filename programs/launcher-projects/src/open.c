#define _DEFAULT_SOURCE
#include "projects.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void detached(void *unused) {
  (void)unused;
  if (setsid() < 0)
    _exit(127);
}
bool spawn_project(const char *action, const char *path) {
  char *working = g_strconcat("--working-directory=", path, NULL);
  const char *argv[14] = {"systemd-run", "--user", "--scope", "--collect",
                          "--quiet",     "--",     NULL};
  if (!strcmp(action, "terminal")) {
    argv[6] = "ghostty";
    argv[7] = working;
  } else if (!strcmp(action, "editor")) {
    argv[6] = "ghostty";
    argv[7] = working;
    argv[8] = "-e";
    argv[9] = "nvim";
    argv[10] = ".";
  } else if (!strcmp(action, "files")) {
    argv[6] = "xdg-open";
    argv[7] = path;
  } else {
    g_free(working);
    return false;
  }
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_cwd(launcher, "/");
  g_subprocess_launcher_set_child_setup(launcher, detached, NULL, NULL);
  GError *error = NULL;
  GSubprocess *child = g_subprocess_launcher_spawnv(launcher, argv, &error);
  g_clear_error(&error);
  bool ok = child != NULL;
  if (child)
    g_object_unref(child);
  g_object_unref(launcher);
  g_free(working);
  return ok;
}
int open_project(const char *home, const char *id, const char *action) {
  if (strcmp(action, "terminal") && strcmp(action, "editor") &&
      strcmp(action, "files")) {
    fputs("invalid action\n", stderr);
    return 1;
  }
  if (!valid_id(id)) {
    fputs("unknown project\n", stderr);
    return 1;
  }
  GPtrArray *paths = discover(home, project_limits, NULL);
  if (!paths) {
    fputs("unknown project\n", stderr);
    return 1;
  }
  const char *selected = NULL;
  for (size_t i = 0; i < paths->len; i++) {
    char *value = project_id(paths->pdata[i]);
    bool match = !strcmp(value, id);
    g_free(value);
    if (match) {
      selected = paths->pdata[i];
      break;
    }
  }
  int result = 0;
  if (!selected || !launchable(home, selected)) {
    fputs("unknown project\n", stderr);
    result = 1;
  } else if (!spawn_project(action, selected)) {
    fputs("unable to launch\n", stderr);
    result = 1;
  }
  g_ptr_array_free(paths, true);
  return result;
}
