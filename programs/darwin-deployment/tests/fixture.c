#define _DEFAULT_SOURCE
#include "tools.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct {
  Val *input;
  Doc *doc;
  Mut *calls, *messages;
  unsigned checks;
} Fixture;
static Result reply(int status, const char *out) {
  return (Result){status, false, g_string_new(out ? out : "")};
}
static void point(const char *path, const char *target) {
  unlink(path);
  if (symlink(target, path) != 0)
    abort();
}
static bool mode(Fixture *f, const char *expected) {
  const char *s = text(field(f->input, "mode"));
  return s && !strcmp(s, expected);
}
static Result run(void *context, const char *const *argv, unsigned timeout,
                  bool capture) {
  Fixture *f = context;
  Mut *call = yyjson_mut_obj(f->doc), *args = yyjson_mut_arr(f->doc);
  for (size_t i = 0; argv[i]; i++)
    yyjson_mut_arr_add_strcpy(f->doc, args, argv[i]);
  yyjson_mut_obj_add_val(f->doc, call, "args", args);
  yyjson_mut_obj_add_uint(f->doc, call, "timeout", timeout);
  yyjson_mut_arr_add_val(f->calls, call);
  if (capture) {
    char *key = g_strjoinv(" ", (char **)argv);
    Val *v = field(field(f->input, "responses"), key);
    g_free(key);
    Result r = reply(v ? (int)yyjson_get_int(field(v, "code")) : 113,
                     text(field(v, "out")));
    r.error = yyjson_get_bool(field(v, "error"));
    return r;
  }
  const char *profile = text(field(f->input, "profile")),
             *current = text(field(f->input, "current")),
             *target = text(field(f->input, "target")),
             *state = text(field(f->input, "state"));
  yyjson_mut_obj_add_bool(f->doc, call, "stateExists", exists(state));
  if (g_str_has_suffix(argv[0], "darwin-preflight")) {
    f->checks++;
    return reply(mode(f, "preflight") ||
                         (mode(f, "preflight-locked") && f->checks == 2)
                     ? 1
                     : 0,
                 "");
  }
  if (!strcmp(argv[0], NIX_ENV)) {
    if (mode(f, "rollback") && strcmp(argv[4], target))
      return reply(1, "");
    point(profile, argv[4]);
    return reply(mode(f, "registration") && !strcmp(argv[4], target) ? 1 : 0,
                 "");
  }
  if (g_str_has_suffix(argv[0], "darwin-rebuild")) {
    if (mode(f, "race") || mode(f, "race-success"))
      point(profile, text(field(f->input, "other")));
    if (mode(f, "activation") || mode(f, "rollback") || mode(f, "race"))
      return reply(1, "");
    if (!mode(f, "no-activation"))
      point(current, target);
    return reply(0, "");
  }
  abort();
}
static void report(void *context, const char *message) {
  Fixture *f = context;
  yyjson_mut_arr_add_strcpy(f->doc, f->messages, message);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  yyjson_doc *input = load(argv[1]);
  if (!input)
    return 2;
  Val *in = yyjson_doc_get_root(input);
  Doc *d = yyjson_mut_doc_new(NULL);
  Mut *root = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, root);
  Fixture f = {in, d, yyjson_mut_arr(d), yyjson_mut_arr(d), 0};
  const char *op = text(field(in, "op"));
  Val *config = field(in, "config");
  if (!strcmp(op, "health")) {
    Doc *snapshot = health_snapshot(config, run, &f);
    bool ok = snapshot != NULL;
    if (ok && yyjson_get_bool(field(in, "record")))
      ok = health_record(config, snapshot);
    yyjson_mut_obj_add_bool(d, root, "ok", ok);
    if (snapshot) {
      yyjson_mut_obj_add_val(
          d, root, "snapshot",
          yyjson_mut_val_mut_copy(d, yyjson_mut_doc_get_root(snapshot)));
      yyjson_mut_doc_free(snapshot);
    }
  } else if (!strcmp(op, "rotate"))
    yyjson_mut_obj_add_int(d, root, "rotated",
                           rotate_log(text(field(in, "path")),
                                      yyjson_get_uint(field(in, "limit"))));
  else if (!strcmp(op, "preflight")) {
    Val *v = field(in, "identity");
    Identity identity = {text(field(v, "system")), text(field(v, "machine")),
                         yyjson_get_bool(field(v, "found")),
                         (uid_t)yyjson_get_uint(field(v, "uid")),
                         text(field(v, "home"))};
    bool error;
    GPtrArray *errors = preflight_problems(config, &identity, run, &f, &error);
    Mut *array = yyjson_mut_arr(d);
    for (size_t i = 0; i < errors->len; i++)
      yyjson_mut_arr_add_strcpy(d, array, errors->pdata[i]);
    yyjson_mut_obj_add_val(d, root, "errors", array);
    yyjson_mut_obj_add_bool(d, root, "error", error);
    g_ptr_array_free(errors, TRUE);
  } else if (!strcmp(op, "diff")) {
    char *diff = metadata_diff(config);
    yyjson_mut_obj_add_bool(d, root, "ok", diff != NULL);
    if (diff)
      yyjson_mut_obj_add_strcpy(d, root, "diff", diff);
    g_free(diff);
  } else if (!strcmp(op, "deploy")) {
    int lock = -1;
    if (mode(&f, "locked")) {
      const char *state = text(field(in, "state"));
      char *p = g_build_filename(state, "deploy.lock", NULL);
      g_mkdir_with_parents(state, 0700);
      lock = open(p, O_RDWR | O_CREAT, 0600);
      if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
        abort();
      g_free(p);
    }
    bool ok =
        deploy_system(text(field(in, "target")), text(field(in, "profile")),
                      text(field(in, "current")), text(field(in, "state")),
                      text(field(in, "homeProfile")), run, &f, report);
    yyjson_mut_obj_add_bool(d, root, "ok", ok);
    if (lock >= 0)
      close(lock);
  } else if (!strcmp(op, "command")) {
    char **args = strings(field(in, "args"));
    Result r = command(NULL, (const char *const *)args,
                       (unsigned)yyjson_get_uint(field(in, "timeout")), true);
    yyjson_mut_obj_add_int(d, root, "code", r.code);
    yyjson_mut_obj_add_bool(d, root, "error", r.error);
    yyjson_mut_obj_add_strncpy(d, root, "out", r.out->str, r.out->len);
    result_free(&r);
    g_strfreev(args);
  } else
    return 2;
  yyjson_mut_obj_add_val(d, root, "calls", f.calls);
  yyjson_mut_obj_add_val(d, root, "messages", f.messages);
  char *out = json(d, false);
  puts(out);
  free(out);
  yyjson_mut_doc_free(d);
  yyjson_doc_free(input);
  return 0;
}
