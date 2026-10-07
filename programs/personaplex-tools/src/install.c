#define _DEFAULT_SOURCE
#include "tools.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char *commas(uint64_t value) {
  char number[32];
  snprintf(number, sizeof(number), "%" PRIu64, value);
  size_t n = strlen(number);
  GString *s = g_string_new(NULL);
  for (size_t i = 0; i < n; i++) {
    g_string_append_c(s, number[i]);
    if (i + 1 < n && (n - i - 1) % 3 == 0)
      g_string_append_c(s, ',');
  }
  return g_string_free(s, false);
}
bool pp_install(const char *root, const Asset *assets, size_t count,
                bool check_only, const char *base_url, long idle_ms,
                char **error) {
  char *parent = g_path_get_dirname(root);
  bool ready = g_mkdir_with_parents(parent, 0777) == 0;
  g_free(parent);
  if (!ready || (mkdir(root, 0700) != 0 &&
                 (errno != EEXIST || !g_file_test(root, G_FILE_TEST_IS_DIR))))
    return pp_error(error, "Could not create model directory");
  for (size_t i = 0; i < count; i++) {
    const Asset *asset = &assets[i];
    char *destination = g_build_filename(root, asset->name, NULL);
    int match = pp_matches(destination, asset);
    if (match == 1) {
      printf("Verified %s\n", asset->name);
      fflush(stdout);
      g_free(destination);
      continue;
    }
    if (match < 0) {
      g_free(destination);
      return pp_error(error, "Could not read model file");
    }
    if (check_only) {
      g_free(destination);
      return pp_error(error, "Missing or invalid %s; run personaplex-models",
                      asset->name);
    }
    char *size = commas(asset->bytes);
    printf("Downloading %s (%s bytes)\n", asset->name, size);
    fflush(stdout);
    g_free(size);
    char *token = pp_token(error);
    if (!token) {
      g_free(destination);
      return false;
    }
    char *temporary = g_build_filename(root, "tmpXXXXXX.partial", NULL);
    int fd = mkstemps(temporary, 8);
    bool ok = fd >= 0;
    if (!ok)
      pp_error(error, "Could not create model temporary file");
    char *url = g_strdup_printf("%s/%s", base_url, asset->name);
    if (ok)
      ok = pp_download(url, token, fd, asset, idle_ms, error);
    if (ok && pp_matches(temporary, asset) != 1)
      ok = pp_error(error, "Integrity check failed for %s", asset->name);
    if (ok && rename(temporary, destination))
      ok = pp_error(error, "Could not install model file");
    if (fd >= 0 && unlink(temporary) != 0 && errno != ENOENT)
      ok = pp_error(error, "Could not remove model temporary file");
    g_free(url);
    g_free(token);
    g_free(temporary);
    g_free(destination);
    if (!ok)
      return false;
  }
  if (!check_only &&
      (!pp_extract(root, "voices", error) || !pp_extract(root, "dist", error)))
    return false;
  const char *needed[] = {"dist/index.html", "voices/NATF2.pt"};
  for (size_t i = 0; i < G_N_ELEMENTS(needed); i++) {
    char *path = g_build_filename(root, needed[i], NULL);
    bool exists = g_file_test(path, G_FILE_TEST_IS_REGULAR);
    g_free(path);
    if (!exists)
      return pp_error(error, "Missing %s; run personaplex-models", needed[i]);
  }
  return true;
}
