/* RFC 0022: every reaching target must establish its obligations. */
#include <stdlib.h>
#include <string.h>
static size_t ignore(const char *p) {
  (void)p;
  return 0;
}
int main(int argc, char **argv) {
  (void)argv;
  size_t (*fn)(const char *) = argc > 1 ? strlen : ignore;
  char s[2] = {7, 7};
  return fn(s);
}
