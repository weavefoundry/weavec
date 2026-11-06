/* The host of shared-images.c: loads the image argv[1] and calls its
 * lib_overflow(argv[2]); or, given "runtime" and a path, loads that file. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  void *image;
  int (*overflow)(long);
  if (argc < 3)
    return 2;
  if (strcmp(argv[1], "runtime") == 0) {
    image = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    printf("loaded: %s\n", image != NULL ? "yes" : dlerror());
    return 0;
  }
  image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (image == NULL) {
    fprintf(stderr, "%s\n", dlerror());
    return 2;
  }
  overflow = (int (*)(long))dlsym(image, "lib_overflow");
  if (overflow == NULL)
    return 2;
  printf("read: %d\n", overflow(atol(argv[2])));
  return 0;
}
