// RFC 0030 §7.4: a flat walk of int m[3][4] through &m[0][0] stays inside the whole array.
// STAGE: S3
// A pointer made by '&a[i]' has the extent of the complete object, so q[k] for k < 12 is in
// bounds; the row m[0] (four ints) is not the bound. The direct subscripts m[i][j] use the
// row's bound, as C does, and are in bounds here too. No error, no trap.
// CLEAN
// ASAN
int sum(void) {
  int m[3][4];
  int *q = &m[0][0];
  for (int k = 0; k < 12; k++) q[k] = k;
  int s = 0;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 4; j++) s += m[i][j];
  return s;
}

int main(void) { return sum() == 66 ? 0 : 1; }
