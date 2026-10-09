#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

typedef struct {
  yyjson_mut_doc *doc;
  yyjson_mut_val *files;
  GHashTable *seen;
} Capture;
static const char *const runtime[] = {
    "last_run_at",         "next_run_at",   "last_status",    "last_error",
    "last_delivery_error", "last_dispatch", "failure_streak", "fire_claim"};
static const char *const managed[] = {
    "crunch-attention-check.sh",     "hmrc-vat-messages-check.sh",
    "hmrc-vat-messages-telegram.sh", "weekly-hermes-update.sh",
    "mac-studio-512-check.sh",       "apply-house-telegram.py"};
static yyjson_val *field(yyjson_val *object, const char *name) {
  yyjson_val *key, *value, *found = NULL;
  size_t i, n, length = strlen(name);
  yyjson_obj_foreach(object, i, n, key,
                     value) if (yyjson_get_len(key) == length &&
                                !memcmp(yyjson_get_str(key), name, length))
      found = value;
  return found;
}
static gboolean listed(const char *name, const char *const *list,
                       size_t length) {
  for (size_t i = 0; i < length; i++)
    if (!strcmp(name, list[i]))
      return TRUE;
  return FALSE;
}
static char *normal(const char *path) {
  GString *result = g_string_new(NULL);
  size_t slash = 0;
  while (path[slash] == '/')
    slash++;
  if (slash)
    g_string_append(result, slash == 2 ? "//" : "/");
  char **parts = g_strsplit(path + slash, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    if (result->len && result->str[result->len - 1] != '/')
      g_string_append_c(result, '/');
    g_string_append(result, parts[i]);
  }
  g_strfreev(parts);
  if (!result->len)
    g_string_append_c(result, '.');
  return g_string_free(result, FALSE);
}
static char *join(const char *parent, const char *name) {
  return !strcmp(parent, ".") ? g_strdup(name)
                              : g_strconcat(parent, "/", name, NULL);
}
static gboolean component(const char *path, const char *wanted) {
  gboolean found = FALSE;
  char **parts = g_strsplit(path, "/", -1);
  for (size_t i = 0; parts[i]; i++)
    if (!strcmp(parts[i], wanted))
      found = TRUE;
  g_strfreev(parts);
  return found;
}
static char *resolved(const char *path) {
  char *current = g_get_current_dir();
  if (*path == '/') {
    g_free(current);
    current = g_strdup("/");
  }
  char **parts = g_strsplit(path, "/", -1);
  for (size_t i = 0; parts[i]; i++) {
    if (!*parts[i] || !strcmp(parts[i], "."))
      continue;
    char *next = !strcmp(parts[i], "..") ? g_path_get_dirname(current)
                                         : join(current, parts[i]);
    g_free(current);
    char *real = realpath(next, NULL);
    current = real ? g_strdup(real) : g_strdup(next);
    free(real);
    g_free(next);
  }
  g_strfreev(parts);
  return current;
}
static gboolean has_store(const char *data, size_t length) {
  const char needle[] = "/nix/store/";
  if (length < sizeof needle - 1)
    return FALSE;
  for (size_t i = 0; i <= length - (sizeof needle - 1); i++)
    if (!memcmp(data + i, needle, sizeof needle - 1))
      return TRUE;
  return FALSE;
}
static gboolean add(Capture *c, const char *path, const char *relative,
                    yyjson_mut_val *array, gboolean executable) {
  struct stat st;
  if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))
    return TRUE;
  if (executable && g_hash_table_contains(c->seen, relative))
    return TRUE;
  char *data = NULL;
  gsize length = 0;
  if (!g_file_get_contents(path, &data, &length, NULL))
    return FALSE;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  gboolean markdown = strlen(base) > 3 && g_str_has_suffix(base, ".md");
  if (executable && !markdown && has_store(data, length)) {
    g_free(data);
    return FALSE;
  }
  char *encoded = g_base64_encode((const guchar *)data, length);
  g_free(data);
  yyjson_mut_val *item = yyjson_mut_obj(c->doc);
  yyjson_mut_obj_add_strcpy(c->doc, item, "path", relative);
  yyjson_mut_obj_add_strcpy(c->doc, item, "content", encoded);
  g_free(encoded);
  if (executable) {
    if (stat(path, &st) != 0)
      return FALSE;
    yyjson_mut_obj_add_bool(c->doc, item, "executable",
                            (st.st_mode & 0111) != 0);
    g_hash_table_add(c->seen, g_strdup(relative));
  }
  yyjson_mut_arr_add_val(array, item);
  return TRUE;
}
static gunichar next_cp(const char **p) {
  gunichar cp = g_utf8_get_char_validated(*p, -1);
  if (cp == (gunichar)-1 || cp == (gunichar)-2)
    cp = 0xdc00 + (unsigned char)*(*p)++;
  else
    *p = g_utf8_next_char(*p);
  return cp;
}
static gint name_compare(gconstpointer first, gconstpointer second) {
  const char *a = *(char *const *)first, *b = *(char *const *)second;
  while (*a && *b) {
    gunichar x = next_cp(&a), y = next_cp(&b);
    if (x != y)
      return x < y ? -1 : 1;
  }
  return *a ? 1 : *b ? -1 : 0;
}
static GPtrArray *names(const char *path) {
  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  DIR *dir = opendir(path);
  if (!dir)
    return result;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno)
        g_ptr_array_set_size(result, 0);
      break;
    }
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
      g_ptr_array_add(result, g_strdup(entry->d_name));
  }
  closedir(dir);
  g_ptr_array_sort(result, name_compare);
  return result;
}
static gboolean walk(Capture *c, const char *path, const char *relative,
                     gboolean plugin, gboolean scripts) {
  GPtrArray *entries = names(path);
  gboolean ok = TRUE;
  for (size_t i = 0; i < entries->len && ok; i++) {
    const char *name = entries->pdata[i];
    if (!strcmp(name, "__pycache__") ||
        (!plugin && (name[0] == '.' || !strcmp(name, "node_modules"))))
      continue;
    char *child = join(path, name), *rel = join(relative, name);
    struct stat st;
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
      ok = walk(c, child, rel, plugin, scripts);
    else if (!scripts || !listed(name, managed, G_N_ELEMENTS(managed)))
      ok = add(c, child, rel, c->files, TRUE);
    g_free(child);
    g_free(rel);
  }
  g_ptr_array_free(entries, TRUE);
  return ok;
}
static gboolean runtime_key(yyjson_val *key) {
  for (size_t i = 0; i < G_N_ELEMENTS(runtime); i++)
    if (yyjson_get_len(key) == strlen(runtime[i]) &&
        !memcmp(yyjson_get_str(key), runtime[i], yyjson_get_len(key)))
      return TRUE;
  return FALSE;
}
static yyjson_mut_val *copy_object(yyjson_mut_doc *doc, yyjson_val *object,
                                   gboolean omit_runtime) {
  yyjson_mut_val *result = yyjson_mut_obj(doc);
  yyjson_val *key, *value;
  size_t i, n;
  yyjson_obj_foreach(object, i, n, key, value) {
    if (omit_runtime && runtime_key(key))
      continue;
    yyjson_mut_val *k = yyjson_val_mut_copy(doc, key),
                   *v = yyjson_val_mut_copy(doc, value);
    yyjson_mut_obj_put(result, k, v);
  }
  return result;
}
static gboolean capture(Capture *c, const char *home) {
  yyjson_mut_val *root = yyjson_mut_obj(c->doc), *jobs = yyjson_mut_arr(c->doc),
                 *seeds = yyjson_mut_arr(c->doc);
  c->files = yyjson_mut_arr(c->doc);
  yyjson_mut_doc_set_root(c->doc, root);
  yyjson_mut_obj_add_int(c->doc, root, "version", 1);
  yyjson_mut_obj_add_val(c->doc, root, "files", c->files);
  yyjson_mut_obj_add_val(c->doc, root, "seed_files", seeds);
  yyjson_mut_obj_add_val(c->doc, root, "jobs", jobs);
  const char *required[] = {"config.yaml", "SOUL.md", "AGENTS.md",
                            "profiles/qwen/config.yaml"};
  for (size_t i = 0; i < G_N_ELEMENTS(required); i++) {
    char *path = join(home, required[i]);
    struct stat st;
    gboolean ok = stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                  add(c, path, required[i], c->files, TRUE);
    g_free(path);
    if (!ok)
      return FALSE;
  }
  const char *directories[] = {"skills", "scripts", "hooks", "plugins",
                               "assets"};
  for (size_t i = 0; i < G_N_ELEMENTS(directories); i++) {
    char *path = join(home, directories[i]);
    gboolean ok = walk(c, path, directories[i], FALSE,
                       !strcmp(directories[i], "scripts"));
    g_free(path);
    if (!ok)
      return FALSE;
  }
  char *plugin = join(home, "hermes-agent/plugins/memory/mem0-selfhosted");
  gboolean ok = component(plugin, "__pycache__") ||
                walk(c, plugin, "plugins/mem0-selfhosted", TRUE, FALSE);
  g_free(plugin);
  if (!ok)
    return FALSE;
  char *path = join(home, "cron/jobs.json"), *bytes = NULL;
  gsize length = 0;
  ok = g_file_get_contents(path, &bytes, &length, NULL);
  g_free(path);
  if (!ok)
    return FALSE;
  yyjson_doc *data = yyjson_read(
      bytes, length, YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_BIGNUM_AS_RAW);
  g_free(bytes);
  if (!data)
    return FALSE;
  yyjson_val *input = field(yyjson_doc_get_root(data), "jobs"), *job;
  size_t i, n;
  ok = yyjson_is_arr(input);
  if (ok)
    yyjson_arr_foreach(input, i, n, job) {
      if (!yyjson_is_obj(job)) {
        ok = FALSE;
        break;
      }
      yyjson_mut_val *result = copy_object(c->doc, job, TRUE);
      yyjson_val *repeat = field(job, "repeat");
      if (yyjson_is_obj(repeat)) {
        yyjson_mut_val *r = copy_object(c->doc, repeat, FALSE);
        yyjson_mut_obj_put(r, yyjson_mut_str(c->doc, "completed"),
                           yyjson_mut_int(c->doc, 0));
        yyjson_mut_obj_put(result, yyjson_mut_str(c->doc, "repeat"), r);
      }
      yyjson_mut_arr_add_val(jobs, result);
    }
  yyjson_doc_free(data);
  if (!ok)
    return FALSE;
  const char *auth[] = {"auth.json", "profiles/qwen/auth.json"};
  for (size_t i = 0; i < G_N_ELEMENTS(auth); i++) {
    path = join(home, auth[i]);
    ok = add(c, path, auth[i], seeds, FALSE);
    g_free(path);
    if (!ok)
      return FALSE;
  }
  return TRUE;
}
static gboolean encrypt_declaration(Capture *c, const char *output) {
  size_t length;
  char *json = yyjson_mut_write(
      c->doc, YYJSON_WRITE_ESCAPE_UNICODE | YYJSON_WRITE_ALLOW_INF_AND_NAN,
      &length);
  if (!json)
    return FALSE;
  char *absolute = resolved(output), *parent = g_path_get_dirname(absolute),
       *cwd = g_path_get_dirname(parent);
  g_free(absolute);
  g_free(parent);
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
      G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_set_cwd(launcher, cwd);
  g_free(cwd);
  const char *argv[] = {
      "sops",          "--encrypt", "--input-type",        "json",
      "--output-type", "json",      "--filename-override", output,
      "/dev/stdin",    NULL};
  GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, NULL);
  g_object_unref(launcher);
  GBytes *input = g_bytes_new_take(json, length), *out = NULL, *err = NULL;
  gboolean communicated =
      process &&
      g_subprocess_communicate(process, input, NULL, &out, &err, NULL);
  if (process && !communicated) {
    g_subprocess_force_exit(process);
    g_subprocess_wait(process, NULL, NULL);
  }
  gboolean ok = communicated && g_subprocess_get_successful(process);
  g_bytes_unref(input);
  if (ok) {
    const void *data = g_bytes_get_data(out, &length);
    FILE *file = fopen(output, "wb");
    ok = file != NULL;
    if (file) {
      ok = fwrite(data, 1, length, file) == length;
      if (fclose(file) != 0)
        ok = FALSE;
    }
  }
  if (out)
    g_bytes_unref(out);
  if (err)
    g_bytes_unref(err);
  if (process)
    g_object_unref(process);
  return ok;
}
static int usage(void) {
  fputs("usage: hermes-export [-h] home output\n", stderr);
  return 2;
}
int main(int argc, char **argv) {
  const char *source = NULL, *destination = NULL;
  gboolean options = TRUE, invalid = FALSE;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (options && !strcmp(arg, "--")) {
      options = FALSE;
      continue;
    }
    if (options &&
        (g_str_has_prefix(arg, "-h=") || g_str_has_prefix(arg, "--help=") ||
         g_str_has_prefix(arg, "--h=")))
      return usage();
    if (options && (g_str_has_prefix(arg, "-h") ||
                    (strlen(arg) >= 3 && strlen(arg) <= 6 &&
                     !strncmp("--help", arg, strlen(arg))))) {
      puts("usage: hermes-export [-h] home output\n\nEncrypt a Hermes "
           "declaration directly to SOPS, never plaintext Git.");
      return 0;
    }
    if (options && arg[0] == '-' && arg[1] && !g_ascii_isdigit(arg[1])) {
      invalid = TRUE;
      continue;
    }
    if (!source)
      source = arg;
    else if (!destination)
      destination = arg;
    else
      invalid = TRUE;
  }
  if (invalid || !source || !destination)
    return usage();
  char *home = normal(source), *output = normal(destination);
  Capture c = {yyjson_mut_doc_new(NULL), NULL,
               g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL)};
  int status = 1;
  if (!capture(&c, home))
    fputs("Hermes export failed; no declaration written\n", stderr);
  else if (!encrypt_declaration(&c, output))
    fputs("SOPS encryption failed; no declaration written\n", stderr);
  else {
    printf("Encrypted %zu configuration files and %zu job definitions\n",
           yyjson_mut_arr_size(c.files),
           yyjson_mut_arr_size(
               yyjson_mut_obj_get(yyjson_mut_doc_get_root(c.doc), "jobs")));
    status = 0;
  }
  g_hash_table_destroy(c.seen);
  yyjson_mut_doc_free(c.doc);
  g_free(home);
  g_free(output);
  return status;
}
