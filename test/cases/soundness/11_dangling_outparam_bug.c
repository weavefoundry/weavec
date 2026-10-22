// Dangling pointer returned through an out-parameter.
// ASAN
// XFAIL: use after return is left to a later RFC (RFC 0036)
static void get(int **out) {
  int local = 5;
  *out = &local;
}
int main(void) {
  int *p;
  get(&p);
  return *p; // TRAP
}
