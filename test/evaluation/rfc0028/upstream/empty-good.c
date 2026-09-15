/* RFC 0028: unchanged cJSON compiled separately. */
#include "../../../../build/corpus/cJSON-program/cJSON.h"
#include <stdlib.h>
int main(void) { cJSON_InitHooks(0); cJSON *p = cJSON_CreateObject(); if (!p) return 0; cJSON_Delete(p); return 0; }
