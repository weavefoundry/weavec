/* RFC 0021: an initialized object still needs a terminator for scanning. */
#include "cJSON.h"
int main(void) {
  char text[2] = {'/', '/'};
  cJSON_Minify(text);
  return 0;
}
