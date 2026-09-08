# RFC 0019 evaluation

The accepted RFC froze `manifest.json` (16 positive/negative pairs),
`transport-manifest.json` (five pairs), and the selected interfaces in
`real-modules.json` before implementation. Do not change the expected results
to accommodate a checker regression.

```sh
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0019/manifest.json --json build/rfc0019-cases.json
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0019/transport-manifest.json
python3 scripts/checked-memory-build.py --cc build/dev/bin/weavec-cc
python3 scripts/checked-memory-corpus.py --weavec build/dev/bin/weavec
```

Run from the repository root. The first three commands are registered with
CTest. The final command requires the pinned Jansson checkout in
`build/corpus/jansson`, as populated by the ordinary corpus harness. It checks
the revision and unchanged upstream source, independently validates C syntax,
and records selected definitions, requirements, trust, input hashes and report
sizes. Allocator adapters and callers live in `real/`; upstream implementation
bodies are not copied or patched. `scripts/corpus/support/jansson` supplies the
same configuration headers used by the ordinary pinned corpus.

Acceptance requires the intended selected scope and rejection reason, with no
entry assumptions on `main` or hidden unsafe/annotation trust. A compiler
object test must report an unavailable helper as deferred at compilation,
resolve it at link time, and produce an executable only on accepted cases.
An unrelated syntax error, timeout, crash, missing report or mismatched scope
cannot satisfy a negative case.

The real selections cover all eight `strbuffer_*` definitions and the two
UTF encoding interfaces named in `real-modules.json`. They do not select
Jansson's configurable allocator hooks or UTF decoding/traversal functions.
The buffer caller exercises init, growth, append, pop, clear, steal and close;
the UTF caller supplies a known output capacity and consumes its encoded
prefix after success. Adversarial callers cover failed allocation, short
output, partial initialization and stale aliases.
