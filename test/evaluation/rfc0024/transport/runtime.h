#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int render(char *, size_t, const char *, ...);
ssize_t pull(char *, size_t);
int forward(const char *, va_list);
