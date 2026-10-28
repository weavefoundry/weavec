// Pointer to local stored in a global, used after the frame is gone.
// ASAN
// XFAIL: use after return is left to a later RFC (RFC 0036)
static int *g_ptr;
static void stash(void) {
  int local = 7;
  g_ptr = &local;
}
int main(void) {
  stash();
  return *g_ptr; // TRAP
}
