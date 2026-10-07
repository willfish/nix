#include "tools.h"
#include <git2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool space(gunichar c) {
  return g_unichar_isspace(c) || c == '\v' || c == 0x85 ||
         (c >= 0x1c && c <= 0x1f);
}
static char *strip(const char *s) {
  const char *end = s + strlen(s);
  while (s < end && space(g_utf8_get_char(s)))
    s = g_utf8_next_char(s);
  while (end > s) {
    const char *previous = g_utf8_find_prev_char(s, end);
    if (!space(g_utf8_get_char(previous)))
      break;
    end = previous;
  }
  return g_strndup(s, (gsize)(end - s));
}
static void add(GPtrArray *errors, const char *prefix, const char *value) {
  g_ptr_array_add(errors,
                  value ? g_strconcat(prefix, value, NULL) : g_strdup(prefix));
}
static bool system_call(Run run, void *context, const char *const *args,
                        Result *r, bool *error) {
  *r = run(context, args, 10000, true);
  if (r->error) {
    *error = true;
    result_free(r);
    return false;
  }
  return true;
}
GPtrArray *preflight_problems(Val *config, const Identity *id, Run run,
                              void *context, bool *error) {
  GPtrArray *errors = g_ptr_array_new_with_free_func(g_free);
  *error = false;
  const char *arch = text(field(config, "architecture")),
             *host = text(field(config, "host")),
             *home = text(field(config, "home")),
             *system_dir = text(field(config, "legacySystemDirectory")),
             *secrets = text(field(config, "systemSecrets")),
             *installed = text(field(config, "installedConfig"));
  char **user_jobs = strings(field(config, "legacyUserJobs")),
       **system_jobs = strings(field(config, "legacySystemJobs")),
       **required = strings(field(config, "requiredFiles")),
       **executables = strings(field(config, "executables"));
  if (!arch || !host || !home || !system_dir || !secrets || !installed ||
      !user_jobs || !system_jobs || !required || !executables) {
    *error = true;
    goto done;
  }
  if (strcmp(id->system, "Darwin"))
    add(errors, "Target requires macOS", NULL);
  if (strcmp(id->machine, arch))
    add(errors, "Target architecture differs from the running machine", NULL);
  const char *hostname[] = {"/bin/hostname", "-s", NULL};
  Result r;
  if (!system_call(run, context, hostname, &r, error))
    goto done;
  char *trim = strip(r.out->str), *lower = g_utf8_strdown(trim, -1);
  if (strcmp(lower, host))
    add(errors, "Register the correct node identity before deployment", NULL);
  g_free(trim);
  g_free(lower);
  result_free(&r);
  if (!id->found) {
    add(errors, "Provision the account before deployment", NULL);
    goto done;
  }
  if (strcmp(id->home, home))
    add(errors, "Account home differs from the target", NULL);
  const char *auto_login[] = {"/usr/bin/defaults", "read",
                              "/Library/Preferences/com.apple.loginwindow",
                              "autoLoginUser", NULL};
  if (!system_call(run, context, auto_login, &r, error))
    goto done;
  if (r.code == 0)
    add(errors, "Disable automatic graphical login before deployment", NULL);
  result_free(&r);
  for (size_t i = 0; user_jobs[i]; i++) {
    char *leaf = g_strconcat(user_jobs[i], ".plist", NULL),
         *path = g_build_filename(home, "Library/LaunchAgents", leaf, NULL),
         *label = g_strdup_printf("gui/%u/%s", (unsigned)id->uid, user_jobs[i]);
    const char *args[] = {"/bin/launchctl", "print", label, NULL};
    bool ran = system_call(run, context, args, &r, error);
    if (ran && (is_link(path) || exists(path) || r.code == 0))
      add(errors, "Retire legacy user job: ", user_jobs[i]);
    if (ran)
      result_free(&r);
    g_free(leaf);
    g_free(path);
    g_free(label);
    if (!ran)
      goto done;
  }
  for (size_t i = 0; system_jobs[i]; i++) {
    char *leaf = g_strconcat(system_jobs[i], ".plist", NULL),
         *path = g_build_filename(system_dir, leaf, NULL),
         *label = g_strconcat("system/", system_jobs[i], NULL);
    const char *args[] = {"/bin/launchctl", "print", label, NULL};
    bool ran = system_call(run, context, args, &r, error);
    if (ran && (is_link(path) || exists(path) || r.code == 0))
      add(errors, "Retire legacy system job: ", system_jobs[i]);
    if (ran)
      result_free(&r);
    g_free(leaf);
    g_free(path);
    g_free(label);
    if (!ran)
      goto done;
  }
  for (size_t i = 0; required[i]; i++)
    if (!is_file(required[i]))
      add(errors, "Provision required runtime/model file: ", required[i]);
  for (size_t i = 0; executables[i]; i++)
    if (access(executables[i], X_OK) != 0)
      add(errors, "Runtime is not executable: ", executables[i]);
  char *key = g_build_filename(home, ".ssh/id_ed25519", NULL);
  struct stat st;
  if (stat(key, &st) != 0)
    add(errors, "Provision the approved shared identity before deployment",
        NULL);
  else if (!S_ISREG(st.st_mode) || (st.st_mode & 0077) || st.st_uid != id->uid)
    add(errors, "Shared SSH key must be private and user-owned", NULL);
  g_free(key);
  char *alias = g_build_filename(home, ".config/sops-nix/secrets", NULL);
  if (exists(alias) && !is_link(alias))
    add(errors, "Inspect the unmanaged secret directory before migration",
        NULL);
  g_free(alias);
  if (exists(secrets) && !is_file(installed))
    add(errors, "Existing system secrets are not owned by this profile", NULL);
done:
  g_strfreev(user_jobs);
  g_strfreev(system_jobs);
  g_strfreev(required);
  g_strfreev(executables);
  return errors;
}
typedef struct {
  GString *text;
  bool header;
} Diff;
static int hunk(const git_diff_delta *delta, const git_diff_hunk *h,
                void *payload) {
  (void)delta;
  Diff *d = payload;
  if (!d->header) {
    g_string_append(
        d->text,
        "--- installed server policy\n\n+++ candidate server policy\n\n");
    d->header = true;
  }
  g_string_append_len(d->text, h->header, h->header_len);
  g_string_append_c(d->text, '\n');
  return 0;
}
static int line(const git_diff_delta *delta, const git_diff_hunk *h,
                const git_diff_line *l, void *payload) {
  (void)delta;
  (void)h;
  Diff *d = payload;
  if (l->origin == ' ' || l->origin == '+' || l->origin == '-') {
    g_string_append_c(d->text, l->origin);
    g_string_append_len(d->text, l->content, l->content_len);
  }
  return 0;
}
char *metadata_diff(Val *config) {
  const char *path = text(field(config, "installedConfig"));
  if (!path || !yyjson_is_obj(config))
    return NULL;
  yyjson_doc *old = is_file(path) ? load(path) : NULL;
  if (is_file(path) && !old)
    return NULL;
  if (old && !yyjson_is_obj(yyjson_doc_get_root(old))) {
    yyjson_doc_free(old);
    return NULL;
  }
  Doc *a = yyjson_mut_doc_new(NULL), *b = yyjson_mut_doc_new(NULL);
  Mut *filtered = yyjson_mut_obj(a);
  yyjson_mut_doc_set_root(a, filtered);
  Val *key, *v;
  size_t i, n;
  yyjson_obj_foreach(config, i, n, key, v) {
    Val *previous =
        old ? field(yyjson_doc_get_root(old), yyjson_get_str(key)) : NULL;
    if (previous)
      yyjson_mut_obj_add(filtered, yyjson_val_mut_copy(a, key),
                         copy_sorted(a, previous));
  }
  // Sort top-level keys too, without ever serializing unknown installed fields.
  char *raw = json(a, false);
  yyjson_doc *parsed = raw ? yyjson_read(raw, strlen(raw), 0) : NULL;
  free(raw);
  if (!parsed) {
    if (old)
      yyjson_doc_free(old);
    yyjson_mut_doc_free(a);
    yyjson_mut_doc_free(b);
    return NULL;
  }
  yyjson_mut_doc_set_root(b, copy_sorted(b, yyjson_doc_get_root(parsed)));
  char *before = json(b, true);
  yyjson_doc_free(parsed);
  yyjson_mut_doc_free(b);
  b = yyjson_mut_doc_new(NULL);
  yyjson_mut_doc_set_root(b, copy_sorted(b, config));
  char *after = json(b, true);
  if (!before || !after) {
    free(before);
    free(after);
    yyjson_mut_doc_free(a);
    yyjson_mut_doc_free(b);
    if (old)
      yyjson_doc_free(old);
    return NULL;
  }
  Diff result = {g_string_new(NULL), false};
  git_libgit2_init();
  git_diff_options options;
  git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION);
  options.context_lines = 3;
  options.interhunk_lines = 0;
  char *left = g_strconcat(before, "\n", NULL),
       *right = g_strconcat(after, "\n", NULL);
  int status = git_diff_buffers(left, strlen(left), "installed server policy",
                                right, strlen(right), "candidate server policy",
                                &options, NULL, NULL, hunk, line, &result);
  git_libgit2_shutdown();
  g_free(left);
  g_free(right);
  free(before);
  free(after);
  yyjson_mut_doc_free(a);
  yyjson_mut_doc_free(b);
  if (old)
    yyjson_doc_free(old);
  if (status) {
    g_string_free(result.text, TRUE);
    return NULL;
  }
  if (result.text->len && result.text->str[result.text->len - 1] == '\n')
    g_string_truncate(result.text, result.text->len - 1);
  return g_string_free(result.text, FALSE);
}
