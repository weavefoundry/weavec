#include <stdio.h>
#include <stdlib.h>

static const int table[4] = {10, 20, 30, 40};

int lookup(int i) {
  if (i < 4)
    return table[i];
  return -1;
}

int main(int argc, char **argv) {
  printf("%d\n", lookup(argc > 1 ? atoi(argv[1]) : 0));
  return 0;
}
