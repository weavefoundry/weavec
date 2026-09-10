# RFC 0021: Practical C traversal and inductive buffer contracts

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-09
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Extends RFCs 0017–0019's pointer,
  numeric, initialization and checked contract models. RFC 0020 continues
  to govern reuse, accounting and validated checkpoints.

## Summary

Check common C buffer traversals under inferred sufficient contracts. Carry
same-array pointer positions through assignments, cursor updates, comparisons,
differences and calls. Infer bounded loop invariants for ordinary counted,
cursor and guarded variable-step traversals, verifying entry and preservation
against the CFG. Transport consumed/produced intervals and cursor outputs
through helpers and compiler objects. Support direct local `goto` control
flow already represented by the CFG. The acceptance targets are unchanged
Jansson UTF and cJSON minifier interfaces with positive and adversarial callers.

The owner requested an RFC first followed by end-to-end implementation.
Drafting and acceptance preceded implementation under that authorization.
The [validation record](../validation-rfc0021.md) documents every acceptance
gate and the remaining coverage limits. This status records the completed
implementation and validation; independent review and merge are separate.

## Motivation

At baseline `90caa95`, all 1,029 CTest entries pass. Nevertheless, checked
mode accepts a helper's `for (unsigned i = 0; i < n; ++i) p[i] = 1` and
rejects the equivalent `while` and pointer-cursor fills. It also rejects
subtraction and ordering of pointers into one local array, a guarded
variable-step scan, and a direct `goto` to a local cleanup label.

The RFC 0020 corpus completes 3/12 selected functions in log.c, 18/151 in
cJSON, 20/88 in linenoise, 18/211 in Jansson and 40/1,157 in Lua. These are
conditional function contracts, not successful whole-project invocations.
The performance milestone makes these limits measurable; it does not remove
them. Jansson's previous positive evaluation deliberately selects only
`utf8_encode` and `utf8_check_first`. Its three decoder/traversal definitions
remain incomplete. cJSON's minifier moves related read/write cursors through
helpers, exercising a common in-place compaction discipline.

The intended improvement is successful checking of these interfaces without
rewriting upstream function bodies or inserting trusted loop assumptions.

## Soundness

### Guarantee and assumptions

Retain RFC 0018's conditional source guarantee. Every reachable operation
must have its separate lifetime, provenance, nullness, bounds, initialization,
writability and arithmetic obligations accounted for. Inferred requirements
are obligations on callers. A candidate invariant is not an assumption:
it must hold at all entries and be preserved on all relevant back edges.
An unavailable proof, unsupported transfer or exhausted limit stays unresolved.

Clang supplies C evaluation, CFG structure, target layout and integer types.
The supported execution model remains single threaded; external library and
annotation trust remains explicit. No runtime instrumentation, pointer ABI
change or unconditional executable certificate is introduced.

### Required distinctions

- A cursor may be formed one past its array but cannot be dereferenced there.
- Difference and ordering require compatible same-array evidence, not merely
  unequal addresses or a may-alias edge. Pointer difference additionally fits
  the target `ptrdiff_t` and uses the pointed-to element size.
- Unknown or released provenance does not become valid through comparison.
- A guarded advance `step <= remaining` can preserve a cursor bound. An
  advance without the guard, a stale bound or a wrapping size cannot.
- Early return, `break`, `continue`, conditional stores and calls preserve
  only facts they establish. A possible access interval is distinct from a
  must-written interval. Zero-iteration paths establish no new written bytes.
- Overlapping input/output traversal is permitted only with an established
  ordering that keeps each read within still-initialized input and every write
  within writable capacity. Pointer-holder aliasing remains a separate issue.
- A helper's success can establish output initialization or cursor progress;
  failure supplies only its own postconditions. Saved input identities refer
  to entry values even when the helper replaces their holders.
- Loop invariants do not prove termination. Every finite execution still has
  its memory operations checked; a nonterminating loop cannot invent an exit.

### Conservative limits

General recursive heap/collector invariants, arbitrary nonlinear induction,
concurrency, signals, `setjmp`/`longjmp`, computed jumps, inline assembly,
arbitrary union/effective-type reinterpretation and encoded pointer bytes
remain outside this milestone. An irreducible loop or an unrepresentable
cursor relationship may remain incomplete. Direct gotos retain all existing
scope/lifetime and initialization checks; accepting their syntax is not a
license to skip a CFG edge or a variable initializer.

## Detailed design

### 1. Pointer positions and compatible arrays

Add a checked-domain description of a pointer position: storage/array
identity, mathematical byte offset, optional extent, stable input origin and
evidence dependencies. Preserve the distinction between an allocation and
its array subobjects. An enclosing allocation alone does not permit ordering
or subtraction of unrelated subobjects. Exact copies, compatible views,
array decay, address-of, represented derivation and complete call outputs
can establish positions. Raw values, uncertain joins and incompatible views
cannot.

Pointer assignments capture the RHS before overwriting the LHS. Numeric
dependencies use stable values; changing `n` does not change an existing
`end = base + n`. Joins retain agreed identities and conservative position
ranges. Unknown writes, releases, replacement and unrepresentable aliases
invalidate affected positions. Facts have source-site-bounded identities so
iterations cannot create unbounded history.

For a subtraction or ordered comparison, establish compatible element types,
same array identity, live provenance and in-array/one-past positions before
using the offset relationship. Subtraction computes the mathematical byte
difference divided by element size, checks divisibility and the target's
`ptrdiff_t` range, then produces the target integer value/range. Ordered
comparisons refine positions only after these premises hold. Unknown facts
cannot prune an edge. Ordinary diagnostic witness policy remains unchanged.

### 2. Bounded relational reasoning

Use bounded difference constraints for stable integer/byte positions where
the existing affine fast path cannot retain the relationship. Examples are
`0 <= index <= size`, `base <= cursor <= end` and `output <= input` in one
array. Addition, subtraction and conversion first use RFC 0017's target
semantics; mathematical relations are installed only when justified.

Core owns relation algebra, overflow-safe host arithmetic, joins, dependency
invalidation and limit accounting without Clang/LLVM includes. Relations
combine only through proved equalities/inequalities. Joining paths weakens
constraints; inconsistency can remove a path only when it follows from
established premises. Bound each function's active traversal variables at 64,
candidate invariants per loop at 32 and relational closure work at 4,096
steps per operation. Exhaustion loses precision and records coverage; it
never removes an obligation. Existing faster scalar/range proofs remain usable.

Difference widening uses zero as a finite threshold: a changing nonnegative
lower bound may weaken to zero, and a changing nonpositive upper bound may
weaken to zero, before becoming unbounded if a later edge crosses that
threshold. This preserves a proved cursor order when different paths initially
establish different positive gaps. It is a weakening of all incoming facts,
not an assumed order, and cannot generate an unbounded sequence of thresholds.

### 3. CFG loop induction

Normalize loop descriptions from `for`, `while` and `do` statements into
their CFG header, entries, back edges, exits and induction updates. Identify
counted traversals with nonzero starts, increasing or decreasing unit steps,
pointer cursors and bounded positive variable steps. Source syntax proposes
candidates; CFG entry and transfer verification establish them.

For each candidate, check initial values on every entry. Forget facts for
loop-written quantities before verifying an arbitrary iteration. Install only
the candidate being checked and independent premises, apply normal checked
transfers and complete callee effects, and verify preservation on each back
edge. Reject circular proofs whose memory/call requirements depend on the
unproved conclusion. Candidates that fail are removed; only the surviving
inductive set can substantiate reporting-pass proofs. Bound validation rounds
at 32. The existing function/context limits remain unchanged.

Track which bound, base and step values must remain stable. Calls may remain
inside a traversal when complete effects establish preservation of the
relevant values. A pointer-to-pointer output requires its actual contract;
it is not treated as an arbitrary stable scalar. `continue` paths must pass
through the required advance, or be validated with their actual transfer.
Loop entry via a goto must be included or prevent the candidate's acceptance.

An equivalent constructive implementation may infer a must-invariant through
ordinary transfer and fixpoint joining instead of installing a speculative
candidate. A join can make an implicit fact explicit only after proving it
on that incoming state: for example, `[0,i)` is empty when `i == 0`.
Value-preserving updates substitute the old index in existing must-written
intervals, and exactly adjacent successful stores can extend those intervals.
Every entry and back edge still participates in the join; an edge lacking the
fact removes it. Reporting uses only the converged states. This supplies the
same initial/preservation rule without circular candidate assumptions.

For small fixed-trip loops, bounded expansion of abstract states may establish
the invariant or final value without widening away the needed numeric range.
This remains CFG analysis with a 32-iteration bound and explicit fallback;
it does not rewrite the source or assume the trip count.

At acyclic joins, finite scalar unions retain target integer ranges instead
of applying a loop widening. Small-loop expansion partitions CFG states by
the number of completed back edges. All successors, calls, exits and lifetime
events use ordinary transfers. An eligible region has one entry header and
no internal cycle after removing its back edges; indirect entry, nested cycles
or an unproved initial bound keep the ordinary fixpoint. Any reachable edge
beyond the expansion budget merges into a widened fallback state and remains
part of final obligation accounting.

### 4. Accessible, consumed and produced intervals

Use an established induction bound to project sufficient entry requirements
for every possible read/write. Infer full initialization only if every
iteration covering that interval executes a successful store. A fill from
`start` to `end` establishes exactly that interval on the normal exit.
Partial fills and early exits can establish a represented prefix tied to
the actual exit cursor/count; they cannot establish the requested capacity.

Equal initialized intervals may combine guarded alternatives only when their
logical union is represented exactly. In particular, merging numeric guards
must retain excluded values; an interval-domain approximation that fills a
hole cannot justify a must-write. Facts proved on each incoming state may be
made unconditional there before joining. Postconditions may remove premises
already established by the final state, while retaining every unresolved
input premise.

For byte scans, a terminated initialized input supplies a finite witness
within accessible storage. A nonzero byte read before that witness can
justify the next position. A lookahead still needs its own bound; termination
does not permit unbounded reads after the first zero. Unknown writes retire
termination evidence. In-place compaction may retain unread input evidence
only when a validated output/input order proves the write cannot destroy it.

A termination witness consists of an initialized prefix, a represented zero
byte within accessible live storage, and its value dependencies. It need not
be the first zero: overwriting an earlier byte with zero can preserve a later
witness. A nonzero read at a position no greater than the witness proves that
position is strictly before it. A zero read does not establish equality with
the witness. A store preserves the witness only when its interval excludes
the zero byte, or the store itself establishes zero there; every written byte
must separately remain initialized. Unknown writes and releases discard the
affected evidence.

Checked affine endpoints gain an explicit `terminator` quantity on an entry
pointer path. This is the byte index of the witness chosen when satisfying
that input's terminated-prefix requirement, not a C integer value or an
assertion of the first `strlen` result. The requirement's `begin` is a lower
bound on that index, and its `end` remains zero. For example, a helper that
first skips two bytes can require a terminated initialized prefix with a
witness at index at least two. All endpoints referring to that path use the
same captured witness. A missing witness cannot be replaced with capacity.

A `terminated` postcondition uses `begin` for the initialized prefix start
and `end` for a proved zero-byte position relative to its memory path. It
preserves or establishes that witness; it does not claim that intervening
bytes are nonzero. Entry dependencies are captured before mutation. The
ordinary memory initialization and pointer-position postconditions remain
separate. Portable parsing distinguishes integer and terminator quantities,
rejects a quantity without a pointer path, and rejects result references in
entry witnesses.

Variable-step decoding retains the correlation among remaining length,
consumed count, success outcome and numeric outputs. Integer accumulators
used by decoding remain subject to shift/overflow checks. Bounded iteration
facts must establish those operations rather than suppress their obligations.

### 5. Compositional cursor contracts

Extend the existing checked contract vocabulary as needed with same-array
requirements and bounded pointer-position postconditions. A position output
names its final destination, an entry pointer origin, a byte interval and
applicable input guard/return outcome. Exact positions use equal interval
endpoints. Uncertain outputs use inclusive enclosing endpoints, never an
invented exact count. Keep these distinct from half-open memory intervals.

The `position` output record uses `path` for the destination pointer and
`other` for its entry pointer origin. Its `begin` and `end` are inclusive
mathematical byte displacements from that entry value. A root parameter is
not an observable destination; a result, global or indirect parameter is.
Memory postcondition endpoints may name an integer `result`, meaning the
actual returned count on that same outcome. Result references are forbidden
in entry requirements and position origins. Capture all input dependencies
before call effects; result endpoints refer to the call's output value and
must not be captured as if they were inputs. A returned interior pointer
does not shift an initialized prefix to its new address.

Callers establish same-array and position premises before application.
Capture inputs before mutation; apply output positions after ordinary call
effects, retaining allocation lifetime and subobject identity. Join outputs
by union of possibilities within the existing alternative bounds. Only
complete postconditions can initialize produced bytes. Missing output paths,
unavailable globals or lost guards invalidate dependent completeness.

For paired cursors, a `progress` postcondition states that the first cursor's
byte advance from its own entry value is no greater than the second cursor's
advance plus a constant. The checker proves this using their relative
coordinates on every return edge. Callers may combine it with an established
same-array entry order to preserve the final order, after both output
positions are installed. It does not establish same-array identity for
unrelated inputs or invent an exact advance. This permits a copy helper's
lockstep advances to compose with an in-place compactor's output/input order.
Both progress paths identify globals or indirect parameters with their own
entry pointer values. A result has no entry value and cannot be a progress
path. The first encoding uses a zero `begin` and a constant byte difference
in `end`.

Instantiation of a terminator quantity at an advanced cursor uses the proved
remaining distance `zero - cursor`. This subtraction requires represented
nonnegative same-array coordinates with `cursor <= zero`. Composing the
distance with that cursor cancels to the original zero position. An output
cursor no greater than the input gives an upper bound, not an equality.
Target-typed additions and subtractions still require their own no-overflow
premises before these mathematical identities can preserve positions.

Generic helpers can require relationships that selected callers establish.
Established related cursor contexts must not receive a contradictory blanket
separation requirement. An absence of an alias edge still does not prove
separation. Unknown callbacks continue to be checked boundaries.

Portable memory contexts can carry a directed same-array entry order between
two byte-pointer paths. Capture this only from live, represented in-array
positions and a proved ordering. The order records share RFC 0016's path,
fact, context-count and depth budgets and participate in strict parsing,
global remapping and context identity. A definite shared-allocation alias
with unknown displacement accompanies an order; the order does not imply
equal addresses, equal ownership shares or a constant displacement.

A contextual traversal can keep separate byte advances relative to each
entry pointer. Those advances start at zero; equality of these numeric counts
does not equate the pointers. Combining entry order with an inequality between
the advances proves an absolute ordering. For potentially overlapping writes,
retain a termination witness only after proving the write excludes its zero
byte using these premises, or proves zero at the affected byte. A generic
helper may instead infer separation for the actual input pointer paths whose
byte contents interact, including indirect paths. Separation of pointer
holders alone does not separate the objects they point into.

A null optional pointer has no referent to overlap another input. Conditional
requirements are checked under their antecedents, after excluding predicates
proved false at the call. Facts obtained only under an antecedent do not become
unconditional caller state. Required initialization and valid-range premises
remain separate from an aliasing condition.

Requirements with the same antecedent may be discharged together in one
conditioned caller state. A fact used by a later requirement must follow from
an independent proof or a recorded sufficient entry requirement in that same
state. No callee output or write is applied during this discharge. Restore the
unconditioned state when leaving the group; only requirements with a proved
antecedent can establish unconditional caller-entry facts. This reuses the
antecedent translation and refinement without introducing trusted assumptions.

Copies of unchanged checked requirement and output sets may share their ordered
storage. Every insertion, replacement or intersection detaches shared storage
before mutation, and iteration exposes only immutable entries. Equality still
compares every entry when storage identities differ. Preserve the existing
requirement cap, union of sufficient entry requirements, intersection of must
outputs and exact portable encoding. Sharing is an allocation optimization;
it neither drops a fact nor changes the convergence or coverage rules.

### 6. Local control flow

Remove the unconditional checked-syntax rejection for ordinary `GotoStmt`.
The existing Clang CFG and lifetime transfer remain authoritative. Test
forward cleanup jumps, backward loops, jumps over initialization, scope exit,
and labels inside loops. Computed goto and nonlocal control flow stay
unresolved. A direct jump into a loop cannot bypass invariant entry checks.

### 7. Transport and cache identity

When exporting a sufficient entry requirement, omit antecedent predicates
that refer to a known private global. This strengthens the requirement:
`private_flag && public_condition => R` becomes `public_condition => R`.
The caller must establish more, never less. Keep the precise conditional
contract for calls within the source unit. This applies only to antecedents
of entry requirements, as permitted by RFC 0019's sufficient-implication rule;
it does not drop a requirement or a dependency of its required property.
Private references in its object, interval or other operand still invalidate
portable completeness. Output premises and unavailable external globals keep
the existing strict remapping rules. A conditional private-callback releaser
must retain a sufficient portable release contract without assuming anything
about the callback's value or weakening release checks at callers.

New interface facts use stable summary paths, never AST addresses or local
place numbers. Summary format 16 and sidecar format 17 carry any new checked
requirement/position records. Bump the checked record encoding when its
vocabulary changes; reports retain the existing expanded/compact mechanism
with documented new kinds. Validate enum values, count bounds, offsets,
guards, type widths, endpoints and all referenced paths. Unknown mandatory
records are rejected rather than ignored. Old compiler metadata requires a
rebuild.

Visit every new premise/output in remapping, semantic equality, dependency
collection, contextual specialization, checkpoint validation and compact
report expansion. Cold, warm and uncached results must agree on proof,
requirements, outputs, trust, diagnostics and limits. Cached object validation
retains all RFC 0020 source/header/preprocessing/object bindings.

### 8. Frozen acceptance and cost

Freeze a source manifest and real-interface manifest before checker edits.
The initial source population is 18 positive/negative pairs (36 cases).
Frozen SHA-256 identities are recorded in the evaluation README. Additional
regressions supplement this population without changing its expectations.
The source population covers counted/while/do/cursor fills, nonzero starts,
reverse traversal, pointer differences/order, bounded lookahead, variable
advances, helper-produced prefixes, related cursors and direct gotos. Pair
positives with short buffers, skipped writes, early exits, changed bounds,
unrelated/released pointers, arithmetic failures and invalid initialization.
Keep rejected unsupported cases separate from positive acceptance cases.
Exercise applicable pairs inline, through helpers, across translation units
and compiler objects, with diagnostic demotion and cache reuse.

Required unchanged real-source selections:

1. Jansson revision `851a2145e3256f2e67e5dfe24b0e456bf198b741`, all five
   definitions in `src/utf.c`: `utf8_encode`, `utf8_check_first`,
   `utf8_check_full`, `utf8_iterate`, `utf8_check_string`.
2. cJSON revision pinned by `scripts/corpus/rfc0015.json`, `cJSON_Minify`,
   `skip_oneline_comment`, `skip_multiline_comment`, `minify_string`.

Select the interfaces and their positive callers; require complete, nonlimited,
nondeferred contracts, no new unsafe/annotation trust and zero entry
requirements on the closed callers. Record generic interface requirements
separately. Negative callers must fail for their intended property, not a
parse error, timeout, missing report, unrelated boundary or crash. Freeze
truncated UTF, insufficient output, unterminated/partially initialized input
and invalid cursor counterexamples. Do not remove targets or loosen expected
outcomes to accommodate an implementation limit.

Run the entire existing Debug and ASan/UBSan suites, strict relevant
clang-tidy, formatting and Core include checks. Add independent concrete
small-domain checks for relation joins/updates and pointer-difference bounds,
plus adversarial CFG entry/back-edge cases. Preserve all existing ordinary
and checked evaluation populations and their completion sets.

Preserve the RFC 0020 final Release executable and run three sequential
ordinary observations on the unchanged five-project corpus. Repeat with the
final implementation binary after builds/tests finish. Maximum median total
runtime and peak RSS growth is 10%; diagnose and fix excess. Measure all five
checked projects with a 600-second per-project timeout, retaining every
selected function in the denominator. Require completed reports and no lost
previously complete selected functions; publish limits, cost, report sizes,
trust and exact diagnostic changes. Real-interface cases must complete
without limits. Record cold/warm equality and successful reuse separately.

## Annotation surface

None. Existing annotation spellings and trust remain unchanged. This milestone
infers and validates supported invariants rather than adding trusted loop
annotations. Requirements and outputs are visible in checked reports.

## Diagnostics

Reuse `checking-incomplete` and `checking-failed`, independent of warning
demotion. Retain the existing pointer relationship message when shared-array
evidence is missing. Add specific reasons under these IDs for an unproved
pointer-difference range and exhausted traversal limits, each with unit and
exact-message lit coverage. A missing traversal invariant or unrepresentable
cursor output leaves the dependent operation's bounds, initialization or
pointer relationship obligation unresolved, using that obligation's existing
reason. An unused output does not require a separate failure merely because
its position could not be exported. This keeps diagnostics attached to actual
proof obligations while dropping unavailable postconditions conservatively.
Established ordinary violations retain their stable IDs and witness policy.
Failure to prove an invariant does not itself demonstrate an out-of-bounds
execution. No new diagnostic ID is required.

## Drawbacks

Relational pointer positions, inductive proofs and conditional progress
contracts complicate joins and dependency invalidation. Rechecking loop
transfers can increase analysis cost. Generic sufficient contracts may be
stronger than necessary. The source, transport and real-interface populations
and fixed cost gates constrain these risks without equating rejection with
proof.

## Alternatives

Implement callback contracts, broaden library models, add archive transport,
or infer recursive heap invariants first. Each addresses a real remaining
boundary but leaves the demonstrated common traversal gaps. Rewriting source
loops to a preferred spelling undermines source compatibility. Raising
iteration limits cannot establish induction. Treating ordinary quietness or
an unverified candidate as proof violates the checked guarantee. A general
solver is unnecessary for the bounded relationships selected here.

## Prior art

RFC 0017 supplies target arithmetic and stable size snapshots, RFC 0016
supplies caller identities, RFC 0019 supplies must-initialization and call
frames, and RFC 0020 supplies dependency-safe reuse. This RFC adds the
inductive invariant rule: establish the invariant initially, prove its
preservation, then use it at a loop header. Difference constraints provide a
bounded representation for common cursor orderings. Clang's CFG supplies
the actual C paths instead of introducing a second C execution semantics.

## Unresolved questions

Data layout, helper organization and fast-path selection are implementation
choices. No acceptance target, trust relaxation or semantic-limit increase
is delegated to implementation. Unexpected counterexamples must be resolved
in the proof or documented as a design amendment before changing behavior.

## Future work

Recursive containers and collectors, checked callback protocols, broader
libc/POSIX/formatting models, archive distribution, concurrency, runtime
enforcement and independent proof certification remain separate milestones.
