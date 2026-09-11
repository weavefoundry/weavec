/* RFC 0023: unchanged upstream definition, concrete closed caller. */
#include "../../../../build/corpus/cJSON-program/cJSON_Utils.c"
int main(void) {
  cJSON a = {0}, b = {0}, parent = {0};
  parent.child = &a;
  a.next = &b;
  return get_array_item(&parent, 1) != 0;
}
