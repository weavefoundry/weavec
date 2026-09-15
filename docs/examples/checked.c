int get(unsigned index) {
  int values[4] = {10, 20, 30, 40};
  if (index >= 4)
    return 0;
  return values[index];
}
