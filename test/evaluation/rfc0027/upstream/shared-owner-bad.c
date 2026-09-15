/* RFC 0027: unchanged upstream implementation and closed lifecycle client. */
#include "../../../../build/corpus/cJSON-program/cJSON.c"
int main(void){cJSON_InitHooks(0);cJSON *a=cJSON_CreateArray();if(!a)return 0;cJSON *b=cJSON_CreateArray();if(!b){cJSON_Delete(a);return 0;}if(!cJSON_AddItemToArray(a,b)){cJSON_Delete(b);cJSON_Delete(a);return 0;}cJSON_Delete(a);cJSON_Delete(b);return 0;}
