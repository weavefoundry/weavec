// RUN: %weavec --checked-function=bounded --checked-function=wide %s -- 2>&1 | FileCheck %s --check-prefix=GOOD --allow-empty
// RUN: not %weavec --checked-function=unordered %s -- 2>&1 | FileCheck %s --check-prefix=BAD
// GOOD-NOT: error:
// BAD: cannot establish checked safety: unsupported checked C construct or storage type [weavec::checking-incomplete]
// RFC 0029: true finite bounds justify conversion; false comparisons admit NaN.
int bounded(double value) {
  if (value >= -2147483648.0 && value <= 2147483647.0)
    return (int)value;
  return 0;
}
long long wide(double value) {
  if (value >= -9223372036854775808.0 && value < 9223372036854775808.0)
    return (long long)value;
  return 0;
}
int unordered(double value) {
  if (value < -2147483648.0 || value > 2147483647.0)
    return 0;
  return (int)value;
}
