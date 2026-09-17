# Initialization up to a helper's current cursor (RFC 0029)

Frozen before candidate 54 in the validation directory during the stable full
suite. The helper advances an output slot by a locally computed count. Its
initialized prefix must end at the actual new cursor, including nonzero caller
starting offsets. Skipped writes, excessive advances and short storage reject.
