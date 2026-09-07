#include "../../Inputs/prelude.h"
void release_then_write(char *a, char *b) {
  free(a);
  *b = 1;
}
void write_then_release(char *a, char *b) { *b = 1; free(a); }
void replace_related(char **a, char **b) {
  free(*a); *a = malloc(4); if (*b) **b = 1;
}
