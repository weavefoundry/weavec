#include "../../../../build/corpus/cJSON-program/cJSON.h"
#include <stdlib.h>
static void *fail_allocate(size_t size) { (void)size; return 0; }
int main(void) {
    cJSON_Hooks hooks = {fail_allocate, free};
    cJSON_InitHooks(&hooks);
    static const char input[] = "{\"x\":[1,true]}";
    cJSON *value = cJSON_ParseWithLength(input, sizeof input);
    if (value) cJSON_Delete(value);
    cJSON_InitHooks(0);
    return 0;
}
