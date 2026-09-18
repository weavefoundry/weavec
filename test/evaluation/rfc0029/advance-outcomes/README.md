# Cursor advances across return outcomes (RFC 0029)

Frozen before candidate 54f. Success writes its advanced prefix; a zero return
may leave the pointer unchanged. A missing write on either outcome stays rejected.
