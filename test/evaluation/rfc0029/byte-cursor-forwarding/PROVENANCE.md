# Byte-content forwarding

Frozen before candidate 74e. This preserves the byte-cursor-content inputs and expected outcomes, adding an ordinary read-only forwarding function in front of the unchanged scan. The wrapper must transport actual live initialized bytes to the rechecked callee. Unknown writes, changed bytes and uninitialized tails remain rejected.
