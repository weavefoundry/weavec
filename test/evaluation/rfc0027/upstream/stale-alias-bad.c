/* RFC 0027: unchanged upstream implementation and closed lifecycle client. */
#include "../../../../build/corpus/cJSON-program/cJSON.c"
int main(void){cJSON_InitHooks(0);cJSON *p=cJSON_CreateObject();if(!p)return 0;cJSON *saved=p;cJSON_Delete(p);return saved->type;}
