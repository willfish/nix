#define _DEFAULT_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
/* Disposable process-local routing. Preserve the Google Host/SNI and TLS
 * checks. No hook, endpoint override or environment variable exists in
 * production. */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **result) {
  int (*next)(const char *, const char *, const struct addrinfo *,
              struct addrinfo **);
  void *symbol = dlsym(RTLD_NEXT, "getaddrinfo");
  memcpy(&next, &symbol, sizeof next);
  if (node && !strcmp(node, "calendar.google.com") &&
      getenv("AGENDA_FIXTURE_PORT"))
    node = "127.0.0.1";
  return next(node, service, hints, result);
}
int connect(int fd, const struct sockaddr *address, socklen_t length) {
  int (*next)(int, const struct sockaddr *, socklen_t);
  void *symbol = dlsym(RTLD_NEXT, "connect");
  memcpy(&next, &symbol, sizeof next);
  const char *port = getenv("AGENDA_FIXTURE_PORT");
  if (port && address->sa_family == AF_INET &&
      length >= sizeof(struct sockaddr_in)) {
    struct sockaddr_in copy;
    memcpy(&copy, address, sizeof copy);
    if (ntohs(copy.sin_port) == 443 &&
        ntohl(copy.sin_addr.s_addr) == INADDR_LOOPBACK) {
      copy.sin_port = htons((unsigned short)strtoul(port, NULL, 10));
      return next(fd, (struct sockaddr *)&copy, sizeof copy);
    }
  }
  return next(fd, address, length);
}
