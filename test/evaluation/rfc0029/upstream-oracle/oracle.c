/* RFC 0029: finite independent allocation-failure audit of pinned cJSON. */
#include "../../../../build/corpus/cJSON-program/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *live[128];
static size_t attempts, fail_at, released, acquired;

static void *allocate(size_t size) {
    ++attempts;
    if (attempts == fail_at) return NULL;
    void *result = malloc(size);
    if (!result) abort();
    for (size_t i = 0; i < sizeof live / sizeof *live; ++i) {
        if (!live[i]) {
            live[i] = result;
            ++acquired;
            return result;
        }
    }
    abort();
}

static void release(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < sizeof live / sizeof *live; ++i) {
        if (live[i] == pointer) {
            live[i] = NULL;
            ++released;
            free(pointer);
            return;
        }
    }
    /* A repeated release or a pointer outside the tracked allocation set. */
    abort();
}

struct example { const char *input; const char *output; };
static const struct example examples[] = {
    {"{}", "{}"},
    {"[]", "[]"},
    {"{\"x\":[1,true,\"hi\",null,{\"a\":[]}]}",
     "{\"x\":[1,true,\"hi\",null,{\"a\":[]}]}"},
    {"[1,-2,0.5,false,\"a\\nb\\t\\u0063\"]",
     "[1,-2,0.5,false,\"a\\nb\\tc\"]"},
    {"{\"x\":[1,true,{\"a\":\"unterminated", NULL},
    {"[1,2,", NULL},
    {"{\"a\": [}", NULL},
    {"", NULL}
};

static size_t workflow(const struct example *example, size_t failure) {
    attempts = acquired = released = 0;
    fail_at = failure;
    cJSON_Hooks hooks = {allocate, release};
    cJSON_InitHooks(&hooks);
    cJSON *value = cJSON_ParseWithLength(example->input,
                                       strlen(example->input) + 1);
    if (value) {
        char *output = cJSON_PrintUnformatted(value);
        if (output) {
            if (!example->output || strcmp(output, example->output)) abort();
            cJSON_free(output);
        } else if (!failure) {
            abort();
        }
        cJSON_Delete(value);
    } else if (!failure && example->output) {
        abort();
    }
    cJSON_InitHooks(NULL);
    if (acquired != released) abort();
    for (size_t i = 0; i < sizeof live / sizeof *live; ++i)
        if (live[i]) abort();
    return attempts;
}

int main(void) {
    size_t runs = 0, failures = 0;
    for (size_t i = 0; i < sizeof examples / sizeof *examples; ++i) {
        const size_t count = workflow(&examples[i], 0);
        ++runs;
        for (size_t failure = 1; failure <= count + 1; ++failure) {
            const size_t observed = workflow(&examples[i], failure);
            ++runs;
            failures += failure <= observed;
        }
    }
    printf("{\"documents\":%zu,\"runs\":%zu,\"injected_failures\":%zu}\n",
           sizeof examples / sizeof *examples, runs, failures);
    return 0;
}
