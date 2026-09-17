# Record-array workflow regressions (RFCs 0015/0029)

Frozen before correcting array-arrow storage access and exact cell selection.
Baseline: candidate29b Release, weavec SHA-256
`246dd010435569ba0b37ddf29065927678dae28c86685b50fe489bd931ac602b`.

The positive cases exercise equivalent arrow, subscript and dereference
spellings, a forwarded first record, and a recursive writer called with a
one-element automatic record array. Its node has a known base-case selector;
the case does not claim arbitrary recursive input coverage. The negatives
require repeated-release, initialization and one-past extent rejection.
No source or expectation is amended to fit observed analyzer behavior.
