// Unit of emission/c99-inline-o2.c: the external definition of 'get'.
int get(const int *p, int i) {
  if (i < 0) return -1;
  return p[i];
}
