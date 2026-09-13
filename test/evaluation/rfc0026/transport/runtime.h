#ifndef RFC0026_RUNTIME_H
#define RFC0026_RUNTIME_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct buffer { unsigned char *data; size_t length, capacity; };
int reserve(struct buffer *,size_t);
int append(struct buffer *,unsigned char);
int append_impl(struct buffer *,unsigned char);
void truncate_buffer(struct buffer *,size_t);
unsigned char *steal(struct buffer *);
void destroy(struct buffer *);
#endif
