# Reverse initialization (RFC 0029)

Frozen before candidate 52. A stable reverse unit-stride loop with an
unconditional byte write establishes exactly the visited suffix. Reads still
need bounds and initialization; skipped iterations, early exits, non-unit
strides and an unwritten zero index cannot establish the full prefix.
