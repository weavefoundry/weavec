/* RFC 0021: a trailing zero cannot initialize the bytes preceding it. */
#include "cJSON.h"
int main(void) {
  char text[5];
  text[0] = '"';
  text[4] = 0;
  cJSON_Minify(text);
  return 0;
}
