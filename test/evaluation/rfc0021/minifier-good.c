/* RFC 0021: comments, escaped quotes and whitespace exercise all helpers. */
#include "cJSON.h"
int main(void) {
  char text[] = "{ \"s\": \"a \\\" b\", /* block */ \"n\": 1 // line\n }";
  cJSON_Minify(text);
  return text[0];
}
