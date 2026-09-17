RFC 0029 first-zero string-length composition regression, frozen before candidate90.
The unchanged upstream duplication helper computes strlen(input)+1, allocates
through a callback, then copies through the zero. Generic equivalents include
direct and extracted allocation, overread, stale storage, missing zero and an
uninitialized prefix. Baseline: immutable candidate89d-bin.
