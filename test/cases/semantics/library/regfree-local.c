// RFC 0030 §8.2: regfree on a local regex_t ends a resource, not the storage.
// STAGE: S4
// regcomp and regfree carry init(regfree) and fini(regfree): the object behind the argument
// acquires and ends a resource while its own storage stays valid, so regfree(&re) on a
// local is no invalid release, and nothing leaks. No error, no warning, no trap.
// CLEAN
// ASAN
#include <regex.h>
#include <stddef.h>

int matches(const char *s) {
  regex_t re;
  if (regcomp(&re, "^a+$", REG_EXTENDED | REG_NOSUB) != 0) return -1;
  int r = regexec(&re, s, 0, NULL, 0) == 0;
  regfree(&re);
  return r;
}

int main(void) { return matches("aaa") == 1 && matches("b") == 0 ? 0 : 1; }
