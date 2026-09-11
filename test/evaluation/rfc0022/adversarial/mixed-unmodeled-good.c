// RFC 0022: a complete null-returning target cannot cover an unmodeled target.
#include <stdlib.h>
static char *empty(const char *name) {
  (void)name;
  return 0;
}
static char *(*lookup)(const char *);
void configure(int use_environment) {
  lookup = use_environment ? getenv : empty;
}
int client(void) {
  char *value = lookup("WEAVEC_RFC22_VALUE");
  if (!value)
    return 0;
  int *invalid = 0;
  return *invalid;
}
void ignore_environment(void) {
  lookup = empty;
}
int main(void) {
  ignore_environment();
  return client();
}
