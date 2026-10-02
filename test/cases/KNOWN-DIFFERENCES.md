# Known differences from the golden run

`engine/` holds v0.10.0's lit engine pins, reduced to (line, id) from the
golden run (RFC 0030 §17.2, `GOLDEN.md`). Gate G3 requires at least 95% of
them to be reproduced at any severity. A pin whose golden message said a
pointer "may be null", or an access "may be" out of bounds, counts as
reproduced when the matching facet at that line is *checked*.

Every pin that is not reproduced gets an entry below. The list may hold at
most 25 entries. Each entry gives:

- the case and line;
- the golden id and severity;
- what the current run reports instead;
- why the difference is intended, with the RFC section that decides it.

## Entries

In S0, `run-cases.py --legacy --filter 'engine/**'` reproduced every pin.
No pin is currently listed as a miss.

## Unit tests

Unit tests of `WeaveCAnalysisTests` written for the old engine whose
expectations the object engine (RFC 0031) does not meet for a reason below.
Each test asserts what the object engine does, which is sound (a lost
finding or a possible one, never a false proof), with a comment that points
here. They do not count toward the 25 entries.

| Test | Old engine | Now | Why |
| --- | --- | --- | --- |
| `PointerIdentity.HelperContextsKeepEachCallbackWithItsUserdata` | `invoke(keep, p)` releases nothing, `invoke(drop, p)` is a definite release | one summary of `invoke` for every caller: through the slot `{keep, drop}` both calls release possibly; `clean` gets two possible findings, `bad`'s use after free is possible | RFC 0031 §6.1 drops the callback context requests of format 29 |
| `PointerIdentity.AComparisonAfterTheCallCanRefuteAConditionalConsume` | a comparison `p != q` after `release_same(p, q)` refutes its release under `p == q` | the release is possible where the call does not decide the comparison: a possible finding on correct code | RFC 0031 *Pointer comparisons*: a pair test selects the effect at the call (a caller's earlier `p != q` refutes it); unlike a result class, it is not kept pending for a later test |
| `EngineExtents.MembersAreBoundedByTheirObject` (`flexible`) | `b->data[5]` checked against `malloc(sizeof *b + 4 * (size_t)n)` | `unresolved(inexpressible)` | no witness spells the conversion `(size_t)n` of an `int` (§5.3); with a `size_t` count the access is checked |
| `ArrayOwnership.CopyingReferenceCountedPointersDoesNotRetainAShare` | `double-free` (a share released twice) | a possible `use-after-free` warning, also in the balanced `clean` | the object engine does not yet infer RFC 0010 reference-count functions (`++p->rc` / `if (--p->rc == 0) free(p)`), so `unref` is a possible release (RFC 0031 §5.5) |
| `HeapState.AFieldWriteAfterConditionalPublicationStillRuns` | `out-of-bounds`, error | `g->data[4]` spatial `unresolved(unknown-index)` | RFC 0031 *Entry tests*: `set` publishes `g` exactly when `g` was null at entry, but its store to `g->data` runs after that join on every path where `g` is non-null (the new box or the caller's), and the exit where allocating the box failed stores nothing: no entry test separates it, so the caller's `g->data` is the old (freed) data or the new block |
| `HeapState.AReturnedRecordSharesAPublishedGlobalObject` | `use-after-free`, error | `g->data[0]` temporal `unresolved(may-alias-released)`; leak warnings at `free(g)` | `get`'s `result.p` is `g`'s value after a possible publication (the entry box or a new one), which no format-30 value spells (§6.1), so the caller cannot tell `a.p` is `g` |

## Converted pins

RFC 0030 removes `analysis-incomplete` and `annotation-required`
(*Diagnostics*, *Removed ids*). What the engine cannot model becomes an
`unresolved` row whose reason §15 item 3 maps from the old text (`budget` for
a limit, `raw-cast` for a reinterpretation, `inexpressible` for
"unrepresentable …", `unanalysed` otherwise), and a call into unknown code
becomes `unresolved(unknown-callee)` (§5.1). No diagnostic can reproduce these
ten pins, so in S3 their `BUG` markers were replaced by `UNRESOLVED` markers
for the rows that replace them. They are no longer pins: G3's denominator is
the remaining 141 pins, and these lines are checked as ledger rows instead.
They are listed for traceability and do not count toward the 25 entries.

| Case and line | Golden | Now |
| --- | --- | --- |
| `engine/Analysis-rfc0006-elements.c:60` | `analysis-incomplete`, warning | `a[0]` temporal not proven (`NOT-PROVEN`): the object engine (RFC 0031 §4.9) keeps the loop's range `[0, n)` of nulled elements, and whether `a[0]` is in it depends on `n`; its reason is `may-alias-released`, not the old engine's `unanalysed` |
| `engine/Analysis-rfc0014-pointer-identity.c:33` | `analysis-incomplete`, warning | `memcpy` temporal `unresolved(raw-cast)`: unsupported memory copy of pointer-containing storage |
| `engine/Analysis-rfc0014-pointer-identity.c:43` | `analysis-incomplete`, warning | `release_field(p)` temporal `unresolved(raw-cast)`: incompatible or unknown object view at call |
| `engine/Analysis-rfc0016-boundaries.c:10` | `analysis-incomplete`, warning | the call's temporal facet is proven: RFC 0031 §6.6 runs the alias context of the 13 aliased arguments, whose reads all precede the release |
| `engine/Analysis-rfc0016-boundaries.c:21` | `analysis-incomplete`, warning | the call's temporal facet is proven: the alias context of the 33 aliased arguments runs (RFC 0031 §6.6) |
| `engine/Analysis-rfc0016-boundaries.c:27` | `analysis-incomplete`, warning | the call's temporal facet is proven: a fresh allocation is distinct from the parameter `q` (RFC 0031 §4.5 D4), so the context unifies only `a` and `b` |
| `engine/Analysis-rfc0016-boundaries.c:33` | `analysis-incomplete`, warning | the call's temporal facet is proven: the context unifies `a` and `b`, and `*b->data` is written before `a->data` is freed |
| `engine/Analysis-rfc0016-context-limits.c:6` | `analysis-incomplete`, warning | nothing on this line: the limit is reached inside the context run of `recurse(p, p, 20)` (line 11), which decides no rows (§2.6); that call's temporal facet is `unresolved(budget)` |
| `engine/Analysis-rfc0003-wrappers.c:70` | `annotation-required`, warning | the call's temporal `unresolved(unknown-callee)` (§5.1) |
| `engine/Analysis-rfc0003-wrappers.c:71` | `annotation-required`, warning | the call's temporal `unresolved(unknown-callee)` (§5.1) |

## Cases

Cases S0 wrote for what RFC 0031 set out to prove that the object engine
does not prove. Each asserts what it does, which is sound (the facets are
unresolved, never proven), with a comment that points here. They do not
count toward the 25 entries.

| Case | Expected at S0 | Now | Why |
| --- | --- | --- | --- |
| `semantics/objects/list-destructor.c` | every temporal facet proven | `p->next` and `free(p)` in the loop `unresolved(may-alias-released)` | RFC 0031 *Unresolved questions*, "Loops over owning links": the loop head's focus object stands for the node the loop freed on one path and the next one on another, so a join cannot say the node `p` points to is not released; a list-segment abstraction is the candidate fix |
| `semantics/objects/materialise-pop.c` | every temporal facet proven | the pop itself is proven; the destructor loop after it, as above | the same |

## Excluded

These 11 lit files test flags that RFC 0030 removes or replaces (§17.6).
They stay in lit and are rewritten in S3. They are outside G3's
denominator, and once S3 removes the flags they are outside
`--compare-golden` too. They do not count toward the 25 entries.

| File | Flag |
| --- | --- |
| `test/Analysis/rfc0002-borrows.c` | `--exclusive-borrows` |
| `test/Analysis/rfc0004-function-pointers.c` | `--strict-externs` |
| `test/Analysis/rfc0004-posix.c` | `--strict-externs` |
| `test/Analysis/rfc0016-clean.c` | `--strict-externs` |
| `test/Annotations/ownership-annotations.c` | `--report-unannotated` |
| `test/Annotations/rfc0003-unknown-extern.c` | `--strict-externs` |
| `test/WholeProgram/rfc0016-composition.c` | `--strict-externs` |
| `test/WholeProgram/rfc0017-numeric.c` | `--strict-externs` |
| `test/Driver/headers-analysed.c` (was `headers-skipped.c`) | `--analyze-headers`, inverted in S3 |
| `test/Driver/version.c` | `--help` lists `--report-unannotated` and `--analyze-headers` |
| `test/Driver/rfc0005-flags.c` | warning control over `annotation-required`, which S3 removes |

## Lit tests

Lit tests written for the old engine whose finding, or inserted check, the
object engine (RFC 0031) no longer makes, or where it now warns on correct
code. Each test asserts what the object engine does, with a comment that
points here; where a finding is lost, the test checks through `--ledger`
that the facet is not proven, so the difference is a lost diagnostic, never
a false proof. They do not count toward the 25 entries.

| Test | Old engine | Now | Why |
| --- | --- | --- | --- |
| `test/Analysis/rfc0006-conditions.c` (`equal_then_free`) | `use-after-free`, error: `if (p == q) { free(p); use(q); }` | `use(q)` temporal `unresolved(may-alias-released)` | the state remembers the comparison `p == q` (RFC 0031 *Implementation amendments*, *Pointer comparisons*) but only calls consult it; a use does not take a release through an equal pointer as definite |
| `test/Analysis/rfc0010-outcomes.c` (`put_and_forget`) | `'s' is leaked`, warning, on `bag_put`'s failure edge | no leak | `bag_put` stores through the variable index `b->n`; the summary exports a possible store to some element with no result class (`store *param0[*].items := path param1 may`), so `s` may have escaped on the failure edge (RFC 0031 §4.9, §6.1); a store at a constant index keeps `when result zero`. Leaks have no ledger facet to check instead |
| `test/Analysis/rfc0010-refcount.c` (`local_retained`) | `'p' is leaked`, warning, note "reference taken here" | no leak | same: without count inference a share taken on a borrowed object is not an owned object (RFC 0031 §5.5). Leaks have no ledger facet to check instead |
| `test/Analysis/rfc0010-refcount.c` (`released_borrow`) | `use-after-free`, error: "use of 'o' after its reference was released" | `use-after-free`, warning: "after it may have been freed" | the object engine does not yet infer RFC 0010 reference-count functions, so `unref` is a possible release (RFC 0031 §5.5) |
| `test/Analysis/rfc0011-bounds.c` (`at_n`, the three `guards` accesses, `copies`) | `out-of-bounds`, errors, against `ints(n)`'s extent `n * sizeof(int)` | spatial `unresolved(unknown-extent)` | `malloc(n * sizeof(int))` of a signed `n` may wrap, so the summary gives the result no extent over `n`, and format 30 has no numeric output expressions (RFC 0031 §6.1, §6.2); with an `unsigned` count the extent `param0 scale 4` is kept |
| `test/Annotations/rfc0010-annotations.c` (`retain_local`) | `leak` warning for the share `p->refs++` takes on a `WEAVEC_REFCOUNT` field; the summary exported `increments{n->next->refs}` | no warning; the summary stores an unknown integer into the count | the object engine does not read `WEAVEC_REFCOUNT` (RFC 0031 §5.5 keeps the count-field keys of RFC 0010, which are not implemented yet); a leak is never a facet (RFC 0030 §3.4), so no outcome is lost |
| `test/Driver/compilation-database-p.c` (`double_release`, `second_release`), `test/Driver/diagnostics-format-sarif.c` (`main`) | `double-free`, error, at the second `node_free(n)` | possible `double-free` warning (temporal `unresolved(may-released)`); the link succeeds | `node_new` may return null and nothing tests it; `node_free`'s releases are keyed by `param 0 !=0`, which the argument does not decide, so each call's release is possible (RFC 0031 *Pending cases and exit splitting*); the engine does not correlate the two calls' tests of the same value. With a null test before the calls the second call is a definite `use-after-free` (`test/Driver/rfc0005-weavec-cc.c`) |
| `test/Emission/rewrite-oracle-span-vla.c` (`get`), `rewrite-oracle-span-vla-lvalues.c`, `rfc0030-trap-runtime.c` and `rfc0030-report-runtime.c` (`stack`) | a span check of every access to `int v[n]` against `sizeof(v)` | no check: spatial `unresolved(unknown-extent)` while `n` may be zero or negative; the tests now test `n` first so that the span form is still emitted and run, and `rewrite-oracle-span-vla.c` keeps the unguarded function to pin the outcome | RFC 0031 *Implementation amendments*, "Variable-length arrays": a dimension that may be zero or negative gives no storage to prove or check an access in. This is a lost runtime check, not only a lost finding |
| `test/WholeProgram/rfc0008-validity.c` (`interior_release`) | `free(p)` of `find`'s result: `invalid-release`, warning | `free(p)` temporal `unresolved(unknown-callee)`; `'s' is leaked` warning there | `strchr`'s interior result is a pointer into `s` at an unknown offset, which no format-30 value spells, so `find` returns `unknown` across the unit boundary (RFC 0031 §6.1). In one unit `invalid-release` is reported |
| `test/WholeProgram/rfc0010-shares.c` | `twice`: `'a' is released twice`, error; `lost`: `'p' is leaked`, warning | `twice`: `use of 'a' after it was freed`, error, at its argument; `lost`: nothing | the object engine does not yet infer RFC 0010 reference-count functions: `counted_unref`'s summary possibly releases its argument and no count field is exported (RFC 0031 §5.5, §6.1). Where the caller knows the count, the cross-unit context decides the release (RFC 0031 §7), which is how `balanced` stays clean and `twice` is definite; `lost` has no count |
| `test/WholeProgram/rfc0012-sized-fields.c` | `v->items[…]` checked against the inferred pair `(items, cap, 4)`; dump, record `sizedFields` and `sizedFieldLoads` pinned | every access `unresolved(unknown-extent)`; the dump and record fields are gone | RFC 0031 §6.1 and §7 drop sized-field facts; counted-field invariants are inferred only for records defined in a unit's main file (RFC 0031 *Implementation amendments*), and `struct vec` is `vec.h`'s: confirming it needs every unit's stores, the link verification of RFC 0030 §7.6 (A3) that is not built |
| `test/WholeProgram/rfc0015-arrays.c` (`cleared`) | `use of 'old'` after free, error | the access's temporal `unresolved(may-alias-released)` | format 30 has no per-element release form (RFC 0031 §6.1): `array15_clear`'s release of `a[0..n)` is lossy, in the summary and in the context `cleared` asks for. `copied` and `returned` are found through their contexts (RFC 0031 §7) |
