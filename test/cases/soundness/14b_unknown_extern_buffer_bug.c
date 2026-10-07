// Unknown external returns a buffer of unknown size; caller indexes it far.
// UNITS: 14b_extern_impl.c
char *get_buffer(void);
int main(void) {
  char *b = get_buffer();
  return b[1000]; // BUG: out-of-bounds // MISS: the buffer comes from a unit built without WeaveC (-fno-weavec): its bounds are unknown
}
