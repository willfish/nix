#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *path, *url, *sha256, *host;
  uint64_t bytes;
  bool stt, experimental;
} Asset;
typedef struct {
  long idle_ms;
  unsigned backoff_ms;
} DownloadPolicy;
extern const Asset assets[];
extern const size_t assets_count;
extern const DownloadPolicy model_policy;
bool selected(const Asset *asset, const char *host, bool stt_only,
              bool experimental);
// -1 means an I/O failure, 0 a missing/invalid asset, 1 a verified asset.
int matches(const char *path, const Asset *asset);
int install_model(const char *root, const Asset *asset, bool check_only,
                  const DownloadPolicy *policy);
