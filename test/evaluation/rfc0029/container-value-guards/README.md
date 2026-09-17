# Container value guards (RFC 0029)

Reduced from the unchanged forced-print cJSON client after candidate70a.
Its live forest retains flags=1 in the head predicate after configuring a
failing allocation callback. The helper returns null, but its value guard
cannot be discharged through the ordinary scalar map alone. The baseline
probe in build/rfc29-validation/container-guard-probes/forest.c rejects the
release of that impossible result and loses the unrelated forest footprint.

The shared-query implementation was drafted before this reduced population
was frozen and has not yet been built or validated. Expectations here follow
the actual C paths: the null result must exclude the out-of-bounds branch;
mutating/replacing the head tag or selecting an actual allocator reaches it.
Releasing the head before the call invalidates its storage and its predicate.
No source name, layout candidate or callback prototype supplies a value fact.
