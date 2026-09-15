/* RFC 0028: unchanged cJSON compiled separately. */
#include "../../../../build/corpus/cJSON-program/cJSON.h"
#include <stdlib.h>
int main(void) { cJSON_Hooks hooks = {malloc, free}; cJSON_InitHooks(&hooks); cJSON *p = cJSON_CreateArray(); if (!p) return 0; cJSON_Delete(p); return 0; }
