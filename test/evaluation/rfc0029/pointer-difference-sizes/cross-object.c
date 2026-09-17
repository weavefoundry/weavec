#include <stdlib.h>
#include <stddef.h>
int main(int argc,char **argv) {
  (void)argv;
  const char data[]="abcdef";
  const char *end=data+(argc>1?3:6);
  const char unrelated[7]={0};
  size_t size=(size_t)(end-unrelated);
  char *output=malloc(size+1);
  if (!output) return 0;
  const char *input=data;
  char *cursor=output;
  while(input<end) *cursor++=*input++;
  *cursor=0;
  free(output);
  return 0;
}
