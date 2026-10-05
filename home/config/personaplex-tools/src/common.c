#define _DEFAULT_SOURCE
#include "tools.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
const char *pp_base_url = "https://huggingface.co/nvidia/personaplex-7b-v1/"
                          "resolve/fdaf4090a61cb315c138a1faee287ffd6c716309";
const Asset pp_assets[] = {
    {"model.safetensors",
     "db1290db583cdaa6cb4de444ed279e0b586ca2a372b41434b07a7461c8c0e2f4",
     UINT64_C(16742874000)},
    {"tokenizer-e351c8d8-checkpoint125.safetensors",
     "09b782f0629851a271227fb9d36db65c041790365f11bbe5d3d59369cf863f50",
     UINT64_C(384644900)},
    {"tokenizer_spm_32k_3.model",
     "78d4336533ddc26f9acf7250d7fb83492152196c6ea4212c841df76933f18d2d",
     UINT64_C(552778)},
    {"voices.tgz",
     "8564e9ca7a06ca723b07c3a77c623f0faa5937d04b2647b3a727b06c5ca0b7bb",
     UINT64_C(6095521)},
    {"dist.tgz",
     "8de47fe2477491fac3dca404185d0430c8d39f9e0daf4c90ada5f03fdf830f45",
     UINT64_C(598195)}};
const size_t pp_assets_count = sizeof(pp_assets) / sizeof(pp_assets[0]);
bool pp_error(char **error, const char *format, ...) {
  va_list args;
  va_start(args, format);
  if (!*error)
    *error = g_strdup_vprintf(format, args);
  va_end(args);
  return false;
}
int pp_matches(const char *path, const Asset *asset) {
  struct stat st;
  if (stat(path, &st))
    return errno == ENOENT || errno == ENOTDIR || errno == ELOOP ||
                   errno == EBADF
               ? 0
               : -1;
  if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size != asset->bytes)
    return 0;
  FILE *file = fopen(path, "rb");
  if (!file)
    return -1;
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  unsigned char data[65536];
  size_t n;
  while ((n = fread(data, 1, sizeof(data), file)))
    g_checksum_update(sum, data, (gssize)n);
  bool ok = !ferror(file),
       valid = ok && !strcmp(g_checksum_get_string(sum), asset->sha256);
  if (fclose(file))
    ok = false;
  g_checksum_free(sum);
  return ok ? valid : -1;
}
static bool space(gunichar c) {
  return g_unichar_isspace(c) || c == 0x85 || (c >= 0x1c && c <= 0x1f);
}
char *pp_token(char **error) {
  const char *env = g_getenv("HF_TOKEN");
  char *value = NULL;
  if (env && *env)
    value = g_strdup(env);
  else {
    const char *home = g_getenv("HF_HOME");
    char *fallback =
        home ? NULL
             : g_build_filename(g_get_home_dir(), ".cache/huggingface", NULL);
    char *path = g_build_filename(home ? home : fallback, "token", NULL);
    GError *failure = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &value, &len, &failure)) {
      if (!g_error_matches(failure, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        pp_error(error, "Could not read Hugging Face credential file");
      g_clear_error(&failure);
    } else if (!g_utf8_validate(value, (gssize)len, NULL)) {
      pp_error(error, "Invalid Hugging Face credential file");
      g_clear_pointer(&value, g_free);
    } else {
      char *start = value, *end = value + len;
      while (*start && space(g_utf8_get_char(start)))
        start = g_utf8_next_char(start);
      while (end > start) {
        char *last = g_utf8_find_prev_char(start, end);
        if (!last || !space(g_utf8_get_char(last)))
          break;
        end = last;
      }
      char *trimmed = g_strndup(start, (gsize)(end - start));
      g_free(value);
      value = trimmed;
    }
    g_free(path);
    g_free(fallback);
  }
  if (!value || !*value) {
    g_free(value);
    pp_error(error, "Accept the PersonaPlex model terms and configure Hugging "
                    "Face access first");
    return NULL;
  }
  if (strchr(value, '\r') || strchr(value, '\n')) {
    g_free(value);
    pp_error(error, "Invalid Hugging Face credential configuration");
    return NULL;
  }
  return value;
}
