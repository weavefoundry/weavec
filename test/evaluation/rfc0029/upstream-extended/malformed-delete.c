#include "../../../../build/corpus/cJSON-program/cJSON.h"
int main(void) {
    cJSON_InitHooks(0);
    const char input[] = "{\"x\":[1,true,{\"a\":\"unterminated";
    cJSON *value = cJSON_ParseWithLength(input, sizeof input);
    if (value) cJSON_Delete(value);
    return 0;
}
