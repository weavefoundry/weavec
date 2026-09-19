// RFC 0030 §8.3: freeaddrinfo and freeifaddrs are releasers.
// STAGE: S4
// getaddrinfo and getifaddrs store fresh lists through their out-parameters
// (out(fresh(freeaddrinfo)), out(fresh(freeifaddrs))), and freeaddrinfo and freeifaddrs
// release them, so reading a list after its release is a definite use-after-free.
#include <ifaddrs.h>
#include <netdb.h>
#include <stddef.h>
#include <sys/socket.h>

int family(const char *host) {
  struct addrinfo *res = NULL;
  if (getaddrinfo(host, NULL, NULL, &res) != 0) return -1;
  freeaddrinfo(res);
  return res->ai_family; // BUG: use-after-free definite
}

int first_flags(void) {
  struct ifaddrs *ifa = NULL;
  if (getifaddrs(&ifa) != 0) return -1;
  freeifaddrs(ifa);
  return ifa == NULL ? 0 : (int)ifa->ifa_flags; // BUG: use-after-free definite
}
