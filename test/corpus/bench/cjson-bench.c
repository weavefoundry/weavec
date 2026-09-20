/*
 * The cJSON workload of the corpus gate's overhead benchmark (RFC 0030,
 * gate G14): build a document of 20,000 records once, print it compactly,
 * then parse that text and print the result formatted, repeatedly.
 *
 *   cjson-bench [iterations]      (default 30)
 *
 * The gate builds this file with cJSON.c from the pinned checkout (see
 * test/corpus/manifest.json) and compares the checksum line it prints
 * between the reference and the WeaveC build. Written in C89, like cJSON.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static unsigned long next_random(unsigned long *state) {
    *state = *state * 1103515245UL + 12345UL;
    return (*state >> 8) & 0xffffffUL;
}

static int add_record(cJSON *items, int index, unsigned long *state) {
    char text[48];
    cJSON *item = cJSON_CreateObject();
    cJSON *tags;
    cJSON *dims;
    int j;

    if (item == NULL) {
        return 0;
    }
    cJSON_AddItemToArray(items, item);
    sprintf(text, "item-%d-%lu", index, next_random(state) % 100000UL);
    cJSON_AddStringToObject(item, "name", text);
    cJSON_AddNumberToObject(item, "id", (double)index);
    cJSON_AddNumberToObject(item, "price",
                            (double)(next_random(state) % 100000UL) / 100.0);
    cJSON_AddBoolToObject(item, "active", (int)(next_random(state) & 1UL));
    sprintf(text, "line one\nline \"%d\"\t\\ end", index % 1000);
    cJSON_AddStringToObject(item, "note", text);
    tags = cJSON_AddArrayToObject(item, "tags");
    if (tags == NULL) {
        return 0;
    }
    for (j = 0; j < 4; j++) {
        sprintf(text, "tag%d", (index + j * 7) % 31);
        cJSON_AddItemToArray(tags, cJSON_CreateString(text));
    }
    dims = cJSON_AddObjectToObject(item, "dims");
    if (dims == NULL) {
        return 0;
    }
    cJSON_AddNumberToObject(dims, "w", (double)(index % 100) * 1.5);
    cJSON_AddNumberToObject(dims, "h", (double)(index % 37) / 3.0);
    cJSON_AddNullToObject(dims, "d");
    return 1;
}

static cJSON *make_document(int records) {
    unsigned long state = 2026UL;
    cJSON *root = cJSON_CreateObject();
    cJSON *items;
    int i;

    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "generator", "cjson-bench");
    items = cJSON_AddArrayToObject(root, "items");
    if (items == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    for (i = 0; i < records; i++) {
        if (!add_record(items, i, &state)) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

int main(int argc, char **argv) {
    int iterations = argc > 1 ? atoi(argv[1]) : 30;
    unsigned long checksum = 0;
    cJSON *document = make_document(20000);
    char *text;
    int i;

    if (document == NULL) {
        fprintf(stderr, "cjson-bench: cannot build the document\n");
        return 1;
    }
    text = cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    if (text == NULL) {
        fprintf(stderr, "cjson-bench: cannot print the document\n");
        return 1;
    }
    for (i = 0; i < iterations; i++) {
        cJSON *parsed = cJSON_Parse(text);
        char *printed;

        if (parsed == NULL) {
            fprintf(stderr, "cjson-bench: parse failed\n");
            cJSON_free(text);
            return 1;
        }
        printed = cJSON_Print(parsed);
        if (printed == NULL) {
            fprintf(stderr, "cjson-bench: print failed\n");
            cJSON_Delete(parsed);
            cJSON_free(text);
            return 1;
        }
        checksum += (unsigned long)strlen(printed);
        checksum += (unsigned long)cJSON_GetArraySize(
            cJSON_GetObjectItemCaseSensitive(parsed, "items"));
        cJSON_free(printed);
        cJSON_Delete(parsed);
    }
    printf("%lu %lu\n", (unsigned long)strlen(text), checksum);
    cJSON_free(text);
    return 0;
}
