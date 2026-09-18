#include "../../../../build/corpus/cJSON-program/cJSON.h"
int main(void) {
    cJSON_InitHooks(0);
    const char text[] = "{}";
    cJSON *value = cJSON_ParseWithLength(text, sizeof text);
    if (!value) return 0;
    cJSON_Delete(value);
    cJSON_Delete(value);
    return 0;
}
