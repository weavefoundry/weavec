/* RFC 0022: same layout does not merge unrelated block-scope C tags. */
static int read(void *p) {
  struct S {
    int value;
  };
  return ((struct S *)p)->value;
}
int main(void) {
  struct S {
    int value;
  };
  struct S x = {7};
  return read(&x);
}
