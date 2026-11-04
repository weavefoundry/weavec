// Pointer to local returned inside a struct by value.
// ASAN
struct view { int *p; };
static struct view make(void) {
  int x = 5;
  struct view v = { &x };
  return v;
}
int main(void) {
  struct view v = make();
  return *v.p; // TRAP
}
