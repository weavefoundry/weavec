# Buffer capacity and additional entry intervals (RFC 0029)

Candidate 66a falsely diagnosed spare physical storage as too small. Candidate
67a removes that invented exact extent but cannot yet project the extra entry
bytes. Candidate 67b projects the extra intervals and exposes unrelated nullable
buffer regressions; 67c restricts the extra projection to extent and initialized
bytes, preserving the existing validity proof.

The original frozen `buffer-lower-extents` and `buffer-entry-intervals`
inventories remain under `build/rfc29-validation`. This reviewed inventory keeps
all six source files and acceptance expectations identical. Two original reason
patterns accidentally omitted the word `extent`, although the intended bounds
precondition was reported. The reviewed patterns include that existing message.
Short storage, an out-of-array index, replaced backing and an uninitialized tail
must still reject for their intended properties. No diagnostic flag is changed.
