#include "../../../../build/corpus/cJSON-program/cJSON.h"
#include <stdlib.h>
static void *fail_allocate(size_t size) { (void)size; return 0; }
int main(void) {
    cJSON_InitHooks(0);
    cJSON *value = cJSON_CreateObject();
    if (!value) return 0;
    cJSON_Hooks hooks = {fail_allocate, free};
    cJSON_InitHooks(&hooks);
    char *output = cJSON_PrintUnformatted(value);
    if (output) cJSON_free(output);
    cJSON_Delete(value);
    cJSON_InitHooks(0);
    return 0;
}
