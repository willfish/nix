#include "agenda.h"
#include <curl/curl.h>
int main(int argc, char **argv) {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    return 1;
  int status = agenda_main(argc, argv);
  curl_global_cleanup();
  icaltimezone_free_builtin_timezones();
  return status;
}
