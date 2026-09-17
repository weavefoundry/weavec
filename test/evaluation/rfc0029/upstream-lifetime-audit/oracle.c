#include "../../../../build/corpus/cJSON-program/cJSON.h"
#include <stdlib.h>
static void *fail_allocate(size_t size) {(void)size;return 0;}
static void parse_temporary(void) {
#ifdef STATIC_INPUT
  static const char input[]="{}";
#else
  const char input[]="{}";
#endif
  (void)cJSON_ParseWithLength(input,sizeof input);
}
int main(void) {
  cJSON_Hooks hooks={fail_allocate,free};
  cJSON_InitHooks(&hooks);
  parse_temporary();
  const char *error=cJSON_GetErrorPtr();
  if(!error) return 2;
  return *error=='{'?0:3;
}
