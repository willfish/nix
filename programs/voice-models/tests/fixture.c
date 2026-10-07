#include "models.h"
#include <curl/curl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "defaults")) {
    printf("{\"idle_ms\":%ld,\"backoff_ms\":%u}\n", model_policy.idle_ms,
           model_policy.backoff_ms);
    return 0;
  }
  if ((argc == 2 && !strcmp(argv[1], "assets")) ||
      (argc == 5 && !strcmp(argv[1], "select"))) {
    bool first = true;
    puts("[");
    for (size_t i = 0; i < assets_count; i++) {
      const Asset *a = &assets[i];
      if (argc == 5 &&
          !selected(a, argv[2], atoi(argv[3]) != 0, atoi(argv[4]) != 0))
        continue;
      if (!first)
        puts(",");
      first = false;
      printf(
          "{\"path\":\"%s\",\"url\":\"%s\",\"sha256\":\"%s\",\"bytes\":%" PRIu64
          ",\"experimental\":%s",
          a->path, a->url, a->sha256, a->bytes,
          a->experimental ? "true" : "false");
      if (a->host)
        printf(",\"hosts\":[\"%s\"]", a->host);
      if (a->stt)
        fputs(",\"role\":\"stt\"", stdout);
      putchar('}');
    }
    puts("\n]");
    return 0;
  }
  if (argc != 9 || strcmp(argv[1], "install"))
    return 2;
  Asset asset = {.path = argv[3],
                 .url = argv[4],
                 .sha256 = argv[5],
                 .bytes = strtoull(argv[6], NULL, 10)};
  DownloadPolicy policy = {.idle_ms = strtol(argv[7], NULL, 10),
                           .backoff_ms = 20};
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    return 1;
  int result = install_model(argv[2], &asset, atoi(argv[8]) != 0, &policy);
  curl_global_cleanup();
  return result;
}
