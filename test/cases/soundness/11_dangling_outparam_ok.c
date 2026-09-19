// CLEAN
// ASAN
static int storage = 5;
static void get(int **out) {
  *out = &storage;
}
int main(void) {
  int *p;
  get(&p);
  return *p;
}
