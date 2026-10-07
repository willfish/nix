#define _DEFAULT_SOURCE
#include "managed.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const runtime[] = {
    "last_run_at",         "next_run_at",   "last_status",    "last_error",
    "last_delivery_error", "last_dispatch", "failure_streak", "fire_claim"};
typedef struct {
  char *path;
  GBytes *data;
  mode_t mode;
} File;
static void file_free(void *p) {
  File *f = p;
  g_free(f->path);
  if (f->data)
    g_bytes_unref(f->data);
  g_free(f);
}
static File *file_new(char *path, GBytes *bytes, mode_t mode) {
  File *f = g_new0(File, 1);
  f->path = path;
  f->data = bytes;
  f->mode = mode;
  return f;
}
static bool allowed(const char *name, bool index) {
  const char *files[] = {"config.yaml", "SOUL.md",
                         "AGENTS.md",   "profiles/qwen/config.yaml",
                         "auth.json",   "profiles/qwen/auth.json"};
  for (size_t i = 0; i < (index ? 4 : 6); i++)
    if (!strcmp(name, files[i]))
      return true;
  const char *roots[] = {"skills", "scripts", "hooks", "plugins", "assets"};
  for (size_t i = 0; i < G_N_ELEMENTS(roots); i++) {
    size_t n = strlen(roots[i]);
    if (!strncmp(name, roots[i], n) && (!name[n] || name[n] == '/'))
      return true;
  }
  return false;
}
static GBytes *decode(Value *v) {
  const char *s = cstring(v);
  size_t n = strlen(s);
  if (failed || n % 4) {
    failed = true;
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (g_ascii_isalnum(c) || c == '+' || c == '/')
      continue;
    if (c == '=' && (i >= n - 2) && ((i == n - 1) || s[i + 1] == '='))
      continue;
    failed = true;
    return NULL;
  }
  gsize length;
  guchar *bytes = g_base64_decode(s, &length);
  return g_bytes_new_take(bytes, length);
}
static Value *jobs(Value *declared, Value *existing) {
  if (!array(declared) || !array(existing)) {
    failed = true;
    return NULL;
  }
  for (size_t i = 0; i < existing->values->len; i++)
    if (!map(existing->values->pdata[i]) ||
        !get(existing->values->pdata[i], "id") ||
        map(get(existing->values->pdata[i], "id")) ||
        array(get(existing->values->pdata[i], "id"))) {
      failed = true;
      return NULL;
    }
  Value *result = value(SEQUENCE), *ids = value(SEQUENCE);
  for (size_t i = 0; i < declared->values->len && !failed; i++) {
    Value *spec = declared->values->pdata[i], *id = get(spec, "id");
    if (!map(spec) || !id || map(id) || array(id)) {
      failed = true;
      break;
    }
    for (size_t j = 0; j < ids->values->len; j++)
      if (equal(id, ids->values->pdata[j]))
        failed = true;
    append(ids, id);
    Value *job = copy(spec), *old = NULL;
    for (size_t n = existing->values->len; n > 0; n--) {
      Value *candidate = existing->values->pdata[n - 1];
      if (equal(get(candidate, "id"), id)) {
        old = candidate;
        break;
      }
    }
    if (equal(get(old, "schedule"), get(spec, "schedule"))) {
      for (size_t n = 0; n < G_N_ELEMENTS(runtime); n++) {
        Value *v = get(old, runtime[n]);
        if (v)
          put(job, runtime[n], copy(v));
      }
      Value *repeat = get(job, "repeat");
      if (map(repeat)) {
        Value *previous = get(old, "repeat"), *completed = NULL;
        if (previous && !map(previous)) {
          failed = true;
          break;
        }
        if (previous)
          completed = get(previous, "completed");
        put(repeat, "completed", completed ? copy(completed) : number("0"));
      }
      Value *state = get(old, "state");
      if (state && state->kind == TEXT &&
          !strcmp(cstring(state), "completed")) {
        put(job, "state", string("completed"));
        put(job, "enabled", boolean(false));
        put(job, "next_run_at", value(NIL));
      }
    }
    append(result, job);
  }
  return result;
}
static Value *yaml_file(File *f) {
  gsize length;
  const char *s = g_bytes_get_data(f->data, &length);
  return parse_yaml(s, length);
}
static char *fresh_key(void) {
  unsigned char data[32];
  int fd = open("/dev/urandom", O_RDONLY);
  size_t at = 0;
  if (fd < 0) {
    failed = true;
    return NULL;
  }
  while (at < sizeof data) {
    ssize_t n = read(fd, data + at, sizeof data - at);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      failed = true;
      break;
    }
    at += (size_t)n;
  }
  close(fd);
  if (failed)
    return NULL;
  char *out = g_malloc(65);
  for (size_t i = 0; i < 32; i++)
    g_snprintf(out + i * 2, 3, "%02x", data[i]);
  return out;
}
static bool backup_write(const char *path, GBytes *data, mode_t mode,
                         const char *backups) {
  if (exists(path)) {
    GBytes *old = read_bytes(path);
    if (!old)
      return false;
    if (!g_bytes_equal(old, data)) {
      gsize n;
      const char *s = g_bytes_get_data(old, &n);
      char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                               (const guchar *)s, n),
           *target = path_join(backups, hash);
      g_free(hash);
      atomic_write(target, old, 0600, ".hermes-managed-", true);
      g_free(target);
    }
    g_bytes_unref(old);
  }
  return !failed && atomic_write(path, data, mode, ".hermes-managed-", true);
}
static gint text_compare(gconstpointer a, gconstpointer b) {
  const Value *x = *(Value *const *)a, *y = *(Value *const *)b;
  return strcmp(x->text, y->text);
}
bool apply_declaration(const char *declaration_path, const char *home,
                       const char *overlay_path, const char *key_file) {
  Value *declaration = read_json(declaration_path),
        *overlay = overlay_path ? read_json(overlay_path) : NULL;
  char *root = path_resolve(home);
  GPtrArray *files = g_ptr_array_new_with_free_func(file_free),
            *stale = g_ptr_array_new_with_free_func(g_free);
  File *new_key = NULL;
  char *jobs_path = NULL, *lock_path = NULL, *index_path = NULL,
       *backups = NULL, *marker = NULL, *profile_marker = NULL;
  int lock = -1;
  bool ok = false;
  if (failed || !map(declaration) ||
      !equal(get(declaration, "version"), number("1"))) {
    failed = true;
    goto done;
  }
  Value *declared = get(declaration, "files"),
        *seeds = get(declaration, "seed_files"),
        *specs = get(declaration, "jobs");
  if (!array(declared) || (seeds && !array(seeds))) {
    failed = true;
    goto done;
  }
  for (size_t group = 0; group < 2 && !failed; group++) {
    Value *items = group ? seeds : declared;
    if (!items)
      continue;
    for (size_t i = 0; i < items->values->len && !failed; i++) {
      Value *item = items->values->pdata[i];
      const char *relative = cstring(get(item, "path"));
      char *path = failed ? NULL : safe_path(root, relative);
      if (!path)
        break;
      if (group && exists(path)) {
        g_free(path);
        continue;
      }
      char *norm = path_normal(relative);
      bool permitted = allowed(norm, false);
      g_free(norm);
      if (!permitted) {
        g_free(path);
        failed = true;
        break;
      }
      GBytes *data = decode(get(item, "content"));
      if (!data) {
        g_free(path);
        break;
      }
      File *file =
          file_new(path, data, truth(get(item, "executable")) ? 0700 : 0600);
      size_t n = 0;
      for (; n < files->len; n++) {
        File *old = files->pdata[n];
        if (!strcmp(old->path, path)) {
          file_free(old);
          files->pdata[n] = file;
          break;
        }
      }
      if (n == files->len)
        g_ptr_array_add(files, file);
    }
  }
  jobs(specs, value(SEQUENCE));
  if (failed)
    goto done;
  if (overlay_path) {
    char *profile_path = path_join(root, "profiles/qwen/config.yaml");
    File *profile = NULL;
    for (size_t i = 0; i < files->len; i++) {
      File *f = files->pdata[i];
      if (!strcmp(f->path, profile_path))
        profile = f;
    }
    g_free(profile_path);
    Value *original = profile ? yaml_file(profile) : NULL;
    if (!map(original) || !key_file) {
      failed = true;
      goto done;
    }
    char *key = NULL;
    if (exists(key_file)) {
      key = strip(read_text(key_file));
      if (!key || !*key)
        failed = true;
    } else {
      Value *stored = get(get(original, "model"), "api_key");
      key = truth(stored) ? g_strdup(cstring(stored)) : fresh_key();
      if (!failed) {
        char *line = g_strconcat(key, "\n", NULL);
        new_key = file_new(path_normal(key_file),
                           g_bytes_new_take(line, strlen(line)), 0600);
      }
    }
    Value *updated = failed ? NULL : merge_profile(original, overlay);
    if (!failed)
      inject_key(updated, string(key));
    g_free(key);
    if (failed)
      goto done;
    GBytes *data = yaml_bytes(updated);
    if (!data)
      goto done;
    g_bytes_unref(profile->data);
    profile->data = data;
  }
  for (size_t i = 0; i < files->len && !failed; i++) {
    File *f = files->pdata[i];
    char *config = path_join(root, "config.yaml");
    bool match = !strcmp(f->path, config);
    g_free(config);
    if (!match)
      continue;
    Value *loaded = yaml_file(f);
    if (!loaded)
      break;
    if (loaded->kind != NIL && !map(loaded))
      continue;
    if (loaded->kind == NIL)
      loaded = value(MAPPING);
    char *house_path = path_join(root, "house-telegram.json");
    Value *house = regular(house_path) ? read_json(house_path) : NULL;
    g_free(house_path);
    if (!failed)
      merge_routes(loaded, house);
    if (failed)
      break;
    GBytes *data = yaml_bytes(loaded);
    if (!data)
      break;
    g_bytes_unref(f->data);
    f->data = data;
  }
  if (failed)
    goto done;
  jobs_path = safe_path(root, "cron/jobs.json");
  lock_path = safe_path(root, "cron/.jobs.lock");
  if (failed)
    goto done;
  char *root_child = path_join(root, ".placeholder");
  bool made = parents(root_child, 0700);
  g_free(root_child);
  if (!made || chmod(root, 0700) != 0 || !parents(lock_path, 0700)) {
    failed = true;
    goto done;
  }
  lock = open(lock_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (lock < 0) {
    failed = true;
    goto done;
  }
  gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
  while (flock(lock, LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      failed = true;
      break;
    }
    if (g_get_monotonic_time() >= deadline) {
      failed = true;
      break;
    }
    g_usleep(100000);
  }
  if (failed)
    goto done;
  Value *existing = exists(jobs_path) ? read_json(jobs_path) : value(MAPPING);
  if (!get(existing, "jobs"))
    put(existing, "jobs", value(SEQUENCE));
  index_path = safe_path(root, ".home-manager-files.json");
  Value *previous = failed               ? NULL
                    : exists(index_path) ? read_json(index_path)
                                         : value(SEQUENCE),
        *paths = value(SEQUENCE);
  if (!map(existing) || !array(previous)) {
    failed = true;
    goto done;
  }
  for (size_t i = 0; i < declared->values->len; i++)
    append(paths, copy(get(declared->values->pdata[i], "path")));
  if (failed)
    goto done;
  g_ptr_array_sort(paths->values, text_compare);
  for (size_t i = 0; i < previous->values->len && !failed; i++) {
    const char *name = cstring(previous->values->pdata[i]);
    char *norm = path_normal(name);
    bool permitted = allowed(name, true);
    if (!permitted) {
      const char *roots[] = {"skills", "scripts", "hooks", "plugins", "assets"};
      for (size_t n = 0; n < G_N_ELEMENTS(roots); n++) {
        size_t length = strlen(roots[n]);
        if (!strncmp(norm, roots[n], length) &&
            (!norm[length] || norm[length] == '/'))
          permitted = true;
      }
    }
    g_free(norm);
    if (!permitted) {
      failed = true;
      break;
    }
    bool keep = false;
    for (size_t j = 0; j < paths->values->len; j++)
      if (equal(previous->values->pdata[i], paths->values->pdata[j]))
        keep = true;
    if (!keep) {
      char *p = safe_path(root, name);
      if (p)
        g_ptr_array_add(stale, p);
    }
  }
  backups = safe_path(root, "backups/home-manager");
  marker = safe_path(root, ".managed");
  profile_marker = safe_path(root, "profiles/qwen/.managed");
  Value *next_jobs = jobs(specs, get(existing, "jobs"));
  if (failed)
    goto done;
  Value *next = copy(existing);
  put(next, "jobs", next_jobs);
  GBytes *job_data = json_bytes(next, true);
  if (failed)
    goto done;
  if (new_key)
    backup_write(new_key->path, new_key->data, new_key->mode, backups);
  for (size_t i = 0; i < files->len && !failed; i++) {
    File *f = files->pdata[i];
    backup_write(f->path, f->data, f->mode, backups);
  }
  if (!failed)
    backup_write(jobs_path, job_data, 0600, backups);
  g_bytes_unref(job_data);
  for (size_t i = 0; i < stale->len && !failed; i++) {
    const char *p = stale->pdata[i];
    if (regular(p)) {
      GBytes *data = read_bytes(p);
      if (!data)
        break;
      gsize n;
      const void *s = g_bytes_get_data(data, &n);
      char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, s, n),
           *target = path_join(backups, hash);
      g_free(hash);
      atomic_write(target, data, 0600, ".hermes-managed-", true);
      g_free(target);
      g_bytes_unref(data);
      if (!failed && unlink(p) != 0)
        failed = true;
    }
  }
  if (!failed) {
    GBytes *data = json_bytes(paths, false);
    atomic_write(index_path, data, 0600, ".hermes-managed-", true);
    if (data)
      g_bytes_unref(data);
  }
  if (!failed) {
    GBytes *data = g_bytes_new_static("home-manager\n", 13);
    atomic_write(marker, data, 0600, ".hermes-managed-", true);
    atomic_write(profile_marker, data, 0600, ".hermes-managed-", true);
    g_bytes_unref(data);
  }
  ok = !failed;
done:
  if (lock >= 0)
    close(lock);
  if (new_key)
    file_free(new_key);
  g_ptr_array_free(files, TRUE);
  g_ptr_array_free(stale, TRUE);
  g_free(root);
  g_free(jobs_path);
  g_free(lock_path);
  g_free(index_path);
  g_free(backups);
  g_free(marker);
  g_free(profile_marker);
  return ok;
}
