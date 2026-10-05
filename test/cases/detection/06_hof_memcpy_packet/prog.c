// RFC 0034 detection set, case 06 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 4 HELLO_WORLD_PAYLOAD
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct packet {
  unsigned char type;
  size_t len;
  unsigned char *payload;
};

static struct packet *packet_parse(const unsigned char *raw, size_t rawlen) {
  if (rawlen < 2)
    return NULL;
  struct packet *p = malloc(sizeof *p);
  if (!p)
    return NULL;
  p->type = raw[0];
  p->len = raw[1]; /* declared payload length */
  p->payload = calloc(1, p->len ? p->len : 1);
  if (!p->payload) {
    free(p);
    return NULL;
  }
#ifdef FIX
  size_t n = rawlen - 2 < p->len ? rawlen - 2 : p->len;
#else
  size_t n = rawlen - 2; /* copies what arrived, not what was declared */
#endif
  memcpy(p->payload, raw + 2, n); // STOP
  return p;
}

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  unsigned char raw[256];
  size_t body = strlen(argv[2]);
  if (body > sizeof raw - 2)
    body = sizeof raw - 2;
  raw[0] = 'T';
  raw[1] = (unsigned char)atoi(argv[1]);
  memcpy(raw + 2, argv[2], body);
  struct packet *p = packet_parse(raw, body + 2);
  if (!p)
    return 1;
  printf("type=%c len=%zu first=%c\n", p->type, p->len, p->len ? p->payload[0] : '-');
  free(p->payload);
  free(p);
  return 0;
}
