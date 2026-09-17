static int advanced(unsigned char *p) { *p = 7; ++p; return *p; }
int main(void) {
  unsigned char bytes[2] = {1, 42};
  if (advanced(bytes) != 7) bytes[2] = 1;
  return 0;
}
