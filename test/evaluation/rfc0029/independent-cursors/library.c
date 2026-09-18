#include <stddef.h>
void copy_bytes(const unsigned char *src, unsigned char *dst, size_t count) {
 const unsigned char *p=src; unsigned char *q=dst;
 while(p<src+count) {*q++=*p++;}
}
