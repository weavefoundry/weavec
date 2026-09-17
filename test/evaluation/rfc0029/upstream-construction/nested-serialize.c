#include "../../../../build/corpus/cJSON-program/cJSON.h"
int main(void) {
  cJSON_InitHooks(0);
  cJSON *root = cJSON_CreateObject();
  if (!root) return 0;
  if (!cJSON_AddNumberToObject(root, "n", 1.0)) {
    cJSON_Delete(root); return 0;
  }
  cJSON *array = cJSON_AddArrayToObject(root, "items");
  if (!array) { cJSON_Delete(root); return 0; }
  cJSON *item = cJSON_CreateString("hi");
  if (!item) { cJSON_Delete(root); return 0; }
  if (!cJSON_AddItemToArray(array, item)) {
    cJSON_Delete(item); cJSON_Delete(root); return 0;
  }
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (text) cJSON_free(text);
  return 0;
}
