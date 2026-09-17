#include "../../../../build/corpus/cJSON-program/cJSON.h"
int main(void) {
    cJSON_InitHooks(0);
    cJSON *value = cJSON_CreateObject();
    if (!value) return 0;
    char *text = cJSON_PrintUnformatted(value);
    cJSON_Delete(value);
    if (text) cJSON_free(text);
    return 0;
}
