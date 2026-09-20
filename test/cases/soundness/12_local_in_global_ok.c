// CLEAN
// ASAN
static int *g_ptr;
static int value = 7;
static void stash(void) {
  g_ptr = &value;
}
int main(void) {
  stash();
  return *g_ptr;
}
