// The formatting unit of sprintf-guard.c: the destination's extent is not
// visible here.
#include <string.h>
#include <stdio.h>
void fmt(char *out, const char *host, int port) { sprintf(out, "%s:%d", host, port); } // STOP
void fmt2(char *out, const char *host) { strcpy(out, host); }
