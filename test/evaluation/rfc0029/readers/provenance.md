# Reader population

Independently frozen after candidate 15, before adding a fully initialized
reader predicate. The original RFC 0029 populations are unchanged. These cases
exercise const byte input, reordered cursor/extent fields, unrelated flags and
depth, separate-source forwarding, a runtime loop, truncated or uninitialized
input, and a cursor outside its advertised extent. Generic helpers must infer
explicit reader premises; the closed client must discharge them.
