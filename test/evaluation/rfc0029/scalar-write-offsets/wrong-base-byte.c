static void fill(unsigned char *p) {
  unsigned char *out = p;
  *out++ = 7;
  *out = 0;
}
int main(void) {
  unsigned char bytes[2] = {1, 1};
  fill(bytes);
  if (bytes[0] == 7) bytes[2] = 1;
  return 0;
}
