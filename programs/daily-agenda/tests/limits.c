#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
static unsigned long long hard(int name) {
  struct rlimit r;
  return getrlimit(name, &r) ? 0 : (unsigned long long)r.rlim_max;
}
int main(void) {
  printf("[{\"cpu\":%llu,\"as\":%llu,\"file\":%llu,\"core\":%llu,\"secret\":%s,"
         "\"preload\":%s,\"lang\":\"%s\",\"tz\":\"%s\"}]",
         hard(RLIMIT_CPU), hard(RLIMIT_AS), hard(RLIMIT_FSIZE),
         hard(RLIMIT_CORE), getenv("AGENDA_PARENT_SECRET") ? "true" : "false",
         getenv("LD_PRELOAD") ? "true" : "false", getenv("LANG"), getenv("TZ"));
  return 0;
}
