# Guarded initialization after a cursor advance (RFC 0029)

Frozen before candidate 56. Only successful calls initialize the advanced bytes.
The actual result guard must reach a later read; replacing or dropping it fails.
