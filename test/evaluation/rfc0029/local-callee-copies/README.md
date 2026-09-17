# Copies stored in tracked automatic slots (RFC 0029)

Frozen before candidate 51. Returning an input pointer through a tracked local
slot does not hand it to unrepresented storage. The input allocation remains
live and releasable; aliases still die with it. Leaking all local aliases and
freeing an unknown interior pointer are unsafe counterparts.
