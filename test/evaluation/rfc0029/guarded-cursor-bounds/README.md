# Result-guarded cursor bounds (RFC 0029)

Frozen before candidate 56c. A zero result leaves the cursor unchanged, while
success advances at least one initialized byte. Mutation retires the old bound.
