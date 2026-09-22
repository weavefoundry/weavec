// Pointer to local stored in a global, used after the frame is gone.
// ASAN
static int *g_ptr;
static void stash(void) {
  int local = 7;
  g_ptr = &local; // BUG: lifetime-too-short
}
int main(void) {
  stash();
  return *g_ptr;
}
