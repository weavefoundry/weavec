The original replaced-head case is safe: the newly configured allocator
always fails, so replacement returns null and the client exits before the tag
write. Candidate70a correctly accepts it. Preserve that original inventory
and failed baseline observation. The reviewed inventory expects this safe
case to pass and adds replaced-live-head, which resets malloc for construction
and reinstalls the failing callback before the actual out-of-bounds branch.
No original source or manifest was modified; the reviewed population has seven
cases instead of six.
