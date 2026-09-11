/* RFC 0022: same layout does not merge unrelated block-scope C tags. */
struct S {
  int value;
};
static int read(void *p) {
  return ((struct S *)p)->value;
}
int main(void) {
  struct S x = {7};
  return read(&x);
}
