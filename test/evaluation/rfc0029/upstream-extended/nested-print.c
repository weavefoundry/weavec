#include "../../../../build/corpus/cJSON-program/cJSON.h"
int main(void) {
    cJSON_InitHooks(0);
    const char input[] = "{\"x\":[1,true,\"hi\",null,{\"a\":[]}]}";
    cJSON *value = cJSON_ParseWithLength(input, sizeof input);
    if (!value) return 0;
    char *output = cJSON_PrintUnformatted(value);
    cJSON_Delete(value);
    if (output) cJSON_free(output);
    return 0;
}
