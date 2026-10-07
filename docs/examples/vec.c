#include <stdio.h>
#include <stdlib.h>

struct vec {
  int *data;
  size_t len;
};

int get(struct vec *v, size_t i) { return v->data[i]; }

int main(int argc, char **argv) {
  struct vec v = {calloc(4, sizeof(int)), 4};
  if (!v.data || argc < 2)
    return 1;
  printf("%d\n", get(&v, (size_t)atoi(argv[1])));
  free(v.data);
  return 0;
}
