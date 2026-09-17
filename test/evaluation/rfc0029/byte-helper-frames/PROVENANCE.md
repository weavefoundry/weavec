# Byte contents across local helper writes

Frozen before candidate75e. The byte-cursor-content inputs and expected outcomes are unchanged. The scan calls an ordinary complete helper that increments an addressed local scalar. Only the callee's actual represented local write can preserve separate byte storage; unknown calls and input mutation cannot.
