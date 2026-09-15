/* RFC 0027: unchanged upstream implementation and closed lifecycle client. */
#include "../../../../build/corpus/cJSON-program/cJSON.c"
int main(void){cJSON_InitHooks(0);cJSON *child=cJSON_CreateArray();if(!child)return 0;cJSON *ref=cJSON_CreateArrayReference(child);if(!ref){cJSON_Delete(child);return 0;}cJSON_Delete(ref);int n=cJSON_GetArraySize(child);cJSON_Delete(child);return n;}
