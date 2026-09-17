# Constant byte storage across global writes

Frozen against candidate74g before introducing immutable-storage premises. The original byte-content inputs and outcomes are unchanged. The scan increments an unrelated static unsigned counter before reading its input. The positive inputs are actual const-qualified character arrays. A casted write to const bytes and an invalid release remain rejected; const pointer spelling alone is not an immutable-storage premise.
