# RFC 0029 workflow populations

`manifest.json` and the C inputs were frozen against the immutable baseline
binary before checker changes. `frozen-sha256.json` binds those files; never
rewrite it to accommodate an outcome. The `upstream` population independently
binds unchanged pinned cJSON sources. Its parse/delete and print/delete cases
remain mandatory RFC goals even when the primary population passes.

`serializer` is an independent streaming hexadecimal encoder with reordered
state fields, runtime input, repeated allocation growth, failure cleanup and a
short-input counterpart. `transport` combines mutually recursive cleanup with
allocator and releaser callbacks across source units. Each later population
has its own freeze and provenance, preceding its first analyzer run.

Run each population with:

```sh
python3 scripts/checked-workflows.py --weavec build/dev/bin/weavec \
  --cc build/dev/bin/weavec-cc --clang clang \
  --population source --output build/rfc0029-source
```

Other populations: `transport`, `serializer`, `readers`, `offsets`, `traversal`,
`construction`, `mutual-construction`, `construction-helpers`,
`output-construction`, `output-transport`, `output-cache`,
`construction-oracle`, `corpus-regressions`, `recursive-transport`,
`recursive-cache`, `objects`, `cache`, `upstream`.
All except upstream run in CTest. The optional upstream runner requires the pinned
checkout under `build/corpus/cJSON-program`; it retains failing observations.

The later `readers` population preserves its original probe manifest and uses
an independently frozen reviewed manifest. Its audit documents why a guarded
out-of-range cursor is safe and replaces that invalid negative with an actual
escaped access. Truncated and partially initialized input exposed a pre-existing
false proof: only the incoming cursor cell had been required for an entire loop.
Both remain mandatory rejections through source and ordinary-object transport.

The `offsets` population was frozen during the candidate-17 proof-boundary
review. It requires exact recursive arguments: an interior or one-past pointer
cannot borrow the induction hypothesis for the allocation's base node.

`traversal` checks read-only recursive forests and includes a separately frozen
review of a safe forwarding cycle: an unchanged edge is permitted when every
cycle also contains a strict child edge. The original negative expectation and
failed development observation remain intact. A cycle with no strict edge is
a required rejection.

`construction` checks runtime-length fresh chains and failure allocation
accounting. `mutual-construction` audits atomic publication and unchanged
forwarding between constructors. `construction-helpers` adds a binary tree
whose second recursive allocation may fail after its left subtree is built;
a complete inferred helper must release that subtree. `recursive-transport`
composes construction, traversal and destruction across source units, and
`recursive-cache` compares their full cold/warm/uncached/compact reports and
dependency invalidation. Object generation lowers ordinary WeaveC errors to
warnings so checked linking independently exercises negative fixtures.

`output-construction` freezes success-returning constructors that publish an
owned forest through a pointer slot. Its failures must actually leave that
slot null, and its success must publish the whole forest. False success,
unchanged failure slots, lost or repeated cleanup, null slots, short input and
nondecreasing recursion have separate cases. `output-transport` splits the
constructor, traversal and cleanup from their client; `output-cache` checks
equivalent reports, reuse and invalidation. The concrete allocation oracle
also covers all three allocation failure points of this output-slot client.

`construction-oracle` instruments allocation/release at runtime, independently
of the abstract checker. It runs each frozen finite positive with failure at
every allocation point, and rejects its leak and repeated-cleanup mutants.
ASan/UBSan also checks the executed paths. This finite ledger is corroborating
evidence, not a proof for arbitrary runtime inputs.

`reader-construction` and `mutable-reader-construction` each freeze seven
recursive constructor cases with an extra reader field, runtime input lengths,
and their truncated, uninitialized, nondecreasing, escaped-interval and cleanup
counterparts. Both run through source, ordinary objects and the concrete
allocation-failure oracle. Copied readers preserve their caller record; mutable
reader hypotheses supply no cursor/count postcondition. These populations do
not replace the unchanged mandatory cJSON parser and serializer clients.

`corpus-regressions` preserves reductions of two lost baseline-complete cJSON
helpers. An external child destructor must not mask imported recursive links,
and buffer discovery must not hide a valid unchanged-entry pointer premise.
Its initial cursor probe lacked enough usage to nominate buffer roles, so the
separate `discovered` inventory adds a role-nominating helper without rewriting
the original population. All four cases and their object clients run in CTest.

The harness verifies fixture and executable identities, rejects parse/tool
failures as evidence, tests intended negative properties, and compares complete
expanded reports across cold, warm, uncached and compact execution. Cache
mutation removes a child's release and must invalidate the entire dependent
proof. Corrupt checkpoints must recompute; older sidecars must be rebuilt.

Core tests independently enumerate progress graphs and concrete byte masks.
Analysis tests cover hidden global effects, nondecreasing cycles, shared and
cyclic child ownership, per-node callbacks, stale/reassigned inputs and forged
callback behavior. These finite tests do not establish arbitrary-C soundness.

The separately reviewed `recursive-writer-reviewed` population checks recursive
byte writers and partial initialized prefixes. `recursive-writer/audit.md`
retains the original expectation audit; complete serialization is still a
separate obligation. `writer-transport` covers separate source, objects and
checkpoint invalidation; `writer-oracle` checks 18,513 finite prefix/capacity
combinations with an independent concrete oracle.

`cursor-readers` preserves the original expectation audit and the reviewed
reader-cursor population. `cursor-cache` checks canonical reuse and mutation;
`cursor-oracle` runs 1,123 finite cursor/input combinations for both ordinary
and partial-return helpers, and detects cursor escape. `recursive-cases` and
`recursive-contexts` distinguish actual input-case completion from generic
recursive exhaustion without raising any budget. `mutual-cases` adds a finite
call chain inside a syntactically recursive component.
