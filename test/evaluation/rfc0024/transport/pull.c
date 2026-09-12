#include "runtime.h"
ssize_t pull(char *b,size_t n){return read(0,b,n);}
