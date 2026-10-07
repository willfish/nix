#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  const char *name, *sha256;
  uint64_t bytes;
} Asset;
extern const Asset pp_assets[];
extern const size_t pp_assets_count;
extern const char *pp_base_url;
bool pp_error(char **error, const char *format, ...) G_GNUC_PRINTF(2, 3);
int pp_matches(const char *path, const Asset *asset);
char *pp_token(char **error);
bool pp_download(const char *url, const char *token, int fd, const Asset *asset,
                 long idle_ms, char **error);
bool pp_extract(const char *root, const char *name, char **error);
bool pp_install(const char *root, const Asset *assets, size_t count,
                bool check_only, const char *base_url, long idle_ms,
                char **error);
char *pp_patch(const char *source, const char *guard, char **error);
