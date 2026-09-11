/* RFC 0022: packed members cannot supply natural pointer alignment. */
struct __attribute__((packed)) P {
  char tag;
  int value;
};
int main(void) {
  struct P x = {0, 7};
  void *p = &x.value;
  int *q = p;
  return *q;
}
