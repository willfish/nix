#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "tools.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static bool execute(Run run, void *context, const char *const *args) {
  Result r = run(context, args, 0, false);
  bool ok = !r.error && r.code == 0;
  result_free(&r);
  return ok;
}
static bool points_to(const char *path, const char *target) {
  char *p = linked(path);
  bool same = p && !strcmp(p, target);
  g_free(p);
  return same;
}
static bool recovery_file(const char *path, const char *target,
                          const char *previous, const char *current,
                          const char *home) {
  Doc *d = yyjson_mut_doc_new(NULL);
  Mut *root = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, root);
  yyjson_mut_obj_add_strcpy(d, root, "target", target);
  yyjson_mut_obj_add_val(d, root, "previousProfile",
                         previous ? yyjson_mut_strcpy(d, previous)
                                  : yyjson_mut_null(d));
  char *active = linked(current), *old_home = home ? linked(home) : NULL;
  yyjson_mut_obj_add_val(d, root, "previousActive",
                         active ? yyjson_mut_strcpy(d, active)
                                : yyjson_mut_null(d));
  yyjson_mut_obj_add_val(d, root, "previousHome",
                         old_home ? yyjson_mut_strcpy(d, old_home)
                                  : yyjson_mut_null(d));
  char *s = json(d, true);
  g_free(active);
  g_free(old_home);
  yyjson_mut_doc_free(d);
  if (!s)
    return false;
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  bool ok = fd >= 0 && fchmod(fd, 0600) == 0;
  size_t at = 0, length = strlen(s);
  while (ok && at < length) {
    ssize_t n = write(fd, s + at, length - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      ok = false;
    else
      at += (size_t)n;
  }
  if (ok && write(fd, "\n", 1) != 1)
    ok = false;
  if (fd >= 0 && close(fd) != 0)
    ok = false;
  free(s);
  return ok;
}
bool deploy_system(const char *target, const char *profile, const char *current,
                   const char *state, const char *home, Run run, void *context,
                   Report report) {
  char *preflight = g_build_filename(target, "sw/bin/darwin-preflight", NULL),
       *rebuild = g_build_filename(target, "sw/bin/darwin-rebuild", NULL),
       *lock_path = g_build_filename(state, "deploy.lock", NULL),
       *previous = NULL, *recovery = NULL, *record = NULL;
  int lock = -1;
  bool ok = false;
  const char *check[] = {preflight, "--target", target, NULL};
  if (!execute(run, context, check))
    goto done;
  if (!make_parent(lock_path, 0700) || chmod(state, 0700) != 0)
    goto done;
  lock = open(lock_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (lock < 0 || fchmod(lock, 0600) != 0 ||
      flock(lock, LOCK_EX | LOCK_NB) != 0)
    goto done;
  if (!execute(run, context, check))
    goto done;
  previous = linked(profile);
  recovery = g_build_filename(state, "rollout-XXXXXX", NULL);
  if (!g_mkdtemp(recovery))
    goto done;
  record = g_build_filename(recovery, "generations.json", NULL);
  if (!recovery_file(record, target, previous, current, home))
    goto done;
  char *message = g_strconcat("Recovery references: ", recovery, NULL);
  report(context, message);
  g_free(message);
  const char *register_profile[] = {NIX_ENV, "-p",   profile,
                                    "--set", target, NULL},
             *activate[] = {rebuild, "activate", NULL};
  ok = execute(run, context, register_profile) &&
       execute(run, context, activate) && points_to(current, target) &&
       points_to(profile, target);
  if (!ok) {
    if (points_to(profile, target)) {
      if (previous) {
        const char *restore[] = {NIX_ENV, "-p",     profile,
                                 "--set", previous, NULL};
        if (!execute(run, context, restore))
          goto done;
      } else if (unlink(profile) != 0)
        goto done;
    } else
      report(context,
             "Profile changed concurrently; refusing to overwrite it.");
    report(context,
           "Activation failed. Profile recovery attempted; services may have "
           "changed. Follow the saved-reference rollback runbook.");
  }
done:
  if (lock >= 0)
    close(lock);
  g_free(preflight);
  g_free(rebuild);
  g_free(lock_path);
  g_free(previous);
  g_free(recovery);
  g_free(record);
  return ok;
}
