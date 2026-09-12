# RFC 0024: Checked C runtime contracts and variadic interfaces

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-12
- **Tracking issue**: Local implementation requested by the project owner
- **Supersedes / superseded by**: Extends RFCs 0018, 0019 and 0022

The project owner authorized drafting this RFC followed by end-to-end
implementation in the same task. Its scope was accepted before checker changes.
The completed acceptance results are in the
[validation record](../validation-rfc0024.md).

## Summary

Extend checked analysis to common C runtime operations, compiler intrinsics,
formatted output and bounded variadic forwarding. Validate arguments before
applying library effects; establish only bytes and result relationships promised
by that operation. The implementation uses the existing ownership, integer,
memory, callback, summary and checked-artifact machinery. It changes no C ABI,
adds no runtime instrumentation and introduces no annotation.

## Motivation

At revision `8454b68`, the validated Release executable rejects safe calls to
`__builtin_expect`, `strcmp`, `fabs` and `snprintf` solely because the callee has
no complete checked contract. Ordinary library summaries already cover many
more operations than checked models. These boundaries propagate through cJSON,
linenoise, Jansson, log.c and Lua. Adding another heap predicate cannot discharge
an unavailable contract for a string comparison or a bounded output call.

The previous milestone records 130 complete conditional contracts among 1,619
selected definitions. That is the preservation population, not a promise that
this milestone verifies all five programs. Their other unsupported constructs,
generic callback requirements and heap invariants remain visible.

## Soundness

The conditional, single-threaded source guarantee of RFC 0018 remains unchanged.
Library implementations are explicit trusted dependencies. Their modeled
preconditions must still hold, and their modeled postconditions must follow
from the actual operation. Trust never establishes arbitrary storage facts.

Reject, with an unresolved obligation or a proved violation as appropriate:

- comparisons/searches over inaccessible or uninitialized bytes;
- unterminated strings when an operation may search beyond an established bound;
- reads/writes using dead storage, insufficient capacity or wrong permissions;
- treating an unsuccessful or partial input operation as a complete buffer fill;
- treating the `snprintf` return value as the number of bytes actually stored;
- incorrect promoted format argument types, missing arguments, unsupported
  conversions, invalid `%s` input or overlapping formatting input/output;
- uninitialized, ended, consumed or conditionally initialized argument lists;
- unknown callback alternatives or user definitions mistaken for library code.

Neither a branch prediction hint nor a likely return outcome restricts the
possible executions. EOF/error paths remain feasible. Zero-length behavior is
modeled per function: a zero-size `snprintf` permits a null destination; a
zero byte count does not universally legalize null pointers to C library calls.

Accepted conservative limits include dynamic format strings that cannot be
bound through a supported interface, positional formats, wide-character and
locale-dependent conversions without a modeled bound, `%n`, direct `va_arg`
extraction, escaping argument lists, and unsupported arithmetic compositions.
The implementation may reject additional valid inputs when their proof exceeds
the established budgets. Missing proof must not be converted into a successful
checked contract. No new rule for concurrency, signals or nonlocal jumps is
introduced.

## Detailed design

### 1. Runtime model registry and dispatch

Introduce a dedicated Analysis runtime-model layer. A bounded immutable registry
classifies the supported symbol, arity, variadic shape, pointer/scalar argument
roles and result category. Resolve definitions and callback targets using the
existing SummaryStore precedence. A source or program definition wins over a
library spelling. Validate compatible argument/result categories before using
a runtime model. Compiler-specific operations require a recognized compiler
builtin identity; an arbitrary user function named `expect` is not an intrinsic.

Normalize supported builtin and fortified spellings with their actual argument
positions. Fortified object-size arguments are additional bounds, never evidence
that missing ordinary bounds are safe. Do not infer a checked model merely from
ordinary ownership-table membership. Existing libc models retain their behavior.

The initial runtime population is:

| Family | Operations |
| --- | --- |
| Intrinsics | `__builtin_expect`, `__builtin_expect_with_probability`, compiler floating classification and absolute-value operations supported by the registry |
| Numeric library | `fabs`, `fabsf`, `fabsl`, `floor`, `floorf`, `floorl`, `ceil`, `ceilf`, `ceill`, `trunc`, `truncf`, `truncl` |
| Comparison/search | `memcmp`, `memchr`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strspn`, `strcspn`, `strpbrk` |
| Input/output | `read`, `write`, `fread`, `fwrite`, `fgets`, `fopen`, `fdopen`, `tmpfile`, `fflush`, `puts`, `fputs`, `fputc`, `putchar` |
| Formatting | `printf`, `fprintf`, `sprintf`, `snprintf` and their `v` forms |
| Argument lists | Clang's `va_start`, `va_copy`, `va_end`, including C23 start spelling when available |

No implementation is accepted simply by adding these names to a success list.
Each family needs independent argument/effect/result tests.

### 2. Shared memory checks and precise effects

Factor reusable checks for live storage, accessible/readable/writable intervals,
initialized terminated prefixes, disjointness and stream lifetime. Requirements
on interface inputs use existing sufficient checked requirements. A modeled
read-only operation preserves unrelated memory facts. A write invalidates the
affected termination/value facts before its established initialization is added.
Unknown writes continue to invalidate conservatively.

Comparison models require the bytes they may read. Bounded string comparison
may use a proved earlier terminator or its full bounded interval. Search
results retain source allocation identity and a nullable bounded position;
finding a byte never grants ownership or extends lifetime. Tests distinguish a
saved search result before and after the source allocation is released.

Compiler expectation intrinsics preserve the first argument's value, integer
semantics and branch refinement. Floating operations add no memory accesses;
their results are not converted into invented integer or allocation-size facts.

### 3. I/O and output-qualified initialization

Check stream handles using live matching-family or sufficient input evidence;
standard streams have explicit library provenance. A pointer cast or arbitrary
nonnull address cannot establish a valid stream. Preserve allocation-family
behavior for opened/closed streams and failed opens.

Implicit standard output (`puts`, `putchar`, `printf` and `vprintf`) is also a
stream dependency. A `standard-stream` entry requirement names `stdout` in its
family field; its unused paths and bounds remain their canonical defaults.
It is input-only and transports through helpers, callbacks, objects and caches
even when a helper declares the output function without including stdio headers.
The C entry point starts with the standard streams available; other functions
retain this environmental requirement. Known release, null replacement,
unrepresented replacement or unknown global mutation prevents discharge.
An entry requirement cannot repair a stream closed earlier in the function.
An established replacement with a live matching-family stream may discharge it.
This condition is independent of the explicit format and buffer arguments.

`write` and `fwrite` read initialized input intervals. `read` and `fread` require
writable capacity but establish only the successfully returned prefix. Product
sizes require target-width overflow checking. Keep return units distinct:
`read` returns bytes and `fread` returns complete elements. Failure and partial
results must not initialize the requested capacity. A positive `fgets` result
establishes an initialized terminated prefix within the destination bound;
the failure outcome supplies no such proof.

Use immutable numeric call-result identities and guarded memory facts. Freeze
input sizes before mutation. A returned count can appear in a helper's output
contract; it is not an entry assumption. Facts survive calls and checkpoints
only when their result and input premises survive projection. Unsupported
projection loses the postcondition and any dependent proof.

### 4. Format parsing and argument validation

Add a Clang-free bounded format parser in Core. Its output is a sequence of
literal lengths and conversions with argument indices, length modifiers,
width/precision constants or argument positions, and expected promoted kinds.
Reject malformed or unsupported syntax explicitly. Limits are 4,096 format
bytes, 64 conversions and 128 argument slots. Limits supply no proof.

Support ordinary narrow output conversions for signed/unsigned integers,
characters, strings, pointers, floating values and literal percent. Validate
the exact promoted type appropriate to each supported length modifier. Extra
arguments are permitted; missing or mismatched consumed arguments fail. Width
and precision supplied through `*` must have the required integer type. `%s`
requires an initialized terminated input or a sufficient nonnegative precision
bound, and its storage must satisfy the applicable overlap rule.

Literal format strings can be obtained directly or through established immutable
interface values. Unknown formats must either produce an explicit portable
format-argument requirement or remain incomplete. A format annotation alone
does not validate an arbitrary argument pack.

For unbounded `sprintf`, require a proved upper output bound. Bounded formatting
checks the destination capacity separately from its output length. A successful
`snprintf` writes `min(result, capacity - 1)` output characters and one NUL when
capacity is positive. Negative results establish no successful output facts.
The result is the would-have-written length; it may exceed capacity. Never mark
the whole destination initialized unless every byte is actually guaranteed.
Known literal/conversion bounds may refine output facts, but must not remove
error paths for operations whose modeled contract permits them.

An output-only `terminated-within` record represents an existential initialized
terminated prefix beginning at `begin` and ending before the exclusive `end`
bound. It does not initialize that whole interval. Its guarded state is stored
separately from ordinary initialized ranges and loses evidence on writes,
dependency invalidation and joins. This supplies bounded formatting and `fgets`
postconditions when the exact terminator cannot be named.

### 5. Variadic forwarding and portable contracts

Add an optional checked argument-list typestate domain. A list is active,
consumed, ended or unknown; active lists retain their source pack identity.
`va_start` creates the current function's trailing pack. `va_copy` requires an
active source and creates an independent cursor. Formatting consumes the list
it receives; a consumed list permits `va_end`, not another traversal. Every
locally started/copied list needs an end on each returning path. Joins retain
active evidence only when all incoming paths agree. Unknown mutation/escape
retires the affected evidence. Representation is independent of whether the
target ABI implements `va_list` as a pointer, record or single-element array.

Bounded forwarding through helpers may export sufficient requirements for
active incoming lists and format/argument-pack compatibility. Format inputs and
pack sources use existing portable summary paths; constants use bounded encoded
data. Requirements record the fixed-parameter count for a trailing pack or the
incoming argument-list path. Calls check actual promoted arguments when a pack
is closed, or strictly remap the same requirement into an enclosing wrapper.
No missing argument, format or source may be silently discarded. Consumption
of incoming lists must also cross calls; copied lists do not consume originals.

`format-arguments` uses `path` for the format input, `begin` for the trailing
pack's first argument index, or `begin=-1` and `other` for an incoming list.
`family` is empty or a bounded hexadecimal `literal:` value; `end` is zero.
The sufficient forwarded requirement also separates string conversion inputs
from the interface's fixed writable pointer inputs. `argument-list` requires
an active cursor. `argument-list-consumed` conservatively retires a caller
cursor if any returning path may consume it; these retirement effects join by
union, unlike initialized output facts. Ending a caller's incoming cursor
directly is outside this initial scope; locally started/copied lists are ended
by their owner.

Direct `va_arg` expressions remain unsupported in this milestone. The new
typestate must not accidentally accept them merely because initialization of
`va_list` is understood. Unknown formats or packs remain visible even when
the enclosing helper has an ordinary inferred ownership summary.

### 6. Transport and organization

Keep Core independent of Clang/LLVM. Add format parsing and portable checked
record validation there; place AST binding, runtime effects, formatted output
and argument-list transfer in dedicated Analysis components. Avoid growing
the main dataflow dispatcher with complete implementations of these families.

New public checked records require summary format 19, sidecar format 20 and
checked encoding 6. Readers reject malformed, oversized or incompletely remapped
records. Update exact format assertions and malformed-record tests. Reports
retain expanded version 2 / compact version 3 unless their schema needs a change;
the existing requirement representation can carry the new kinds. Model changes
invalidate persistent reuse through the existing executable/input bindings.

Compile-only analysis may defer a missing contract for a known external
callback target exactly as for a direct external call. An owned list whose
cursor becomes unknown solely at such a deferred call may be ended during
that incomplete compile-stage analysis. Its dependency remains deferred and
the link must reanalyze it against the definition; neither deferral supplies
a complete source contract nor permits checked linking without the definition.

Runtime contracts also increase repeated explanation propagation. Following
RFC 0020's immutable explanation reuse, the existing bounded call-preparation
cache may retain a complete canonical call ledger. Its identity includes the
exact live source projection, callee, call site, caller, trust selection and
unsafe mode. Charge retained rows, strings and paths against the existing
1,024-entry / 64 MiB cache bounds; expired source identities remain misses.
Apply a prepared ledger by canonical ordered merge only when the sum of the
destination size and original origin count cannot exhaust the obligation
budget. Otherwise use the original insertion order, preserving which entries
survive exhaustion and every diagnostic. Prepared rows may also be replayed in
that original order: each key retains its canonical outcome and route, while
its first occurrence receives the original capacity decision. Compare cached and uncached ledgers,
including near-capacity, changed-site, mutation and unsafe cases. This changes
representation and reuse, not semantic budgets or proof obligations.

### 7. Frozen validation and cost

Before checker implementation, freeze source cases and expectations, preserving
the current Release executable and its hashes. Include every family, wrapper
composition, input/output errors, truncation, aliasing, list lifecycle and
signature/definition counterexamples. Negative cases must report the intended
missing property; syntax failures, crashes and timeouts never count as success.

Keep source and object/cache populations distinct. Include separate translation
units, compiler sidecars, callback alternatives, stale-input rejection and
uncached/cold/warm report equivalence. Supplement the original source population
with unchanged upstream clients for cJSON comparison and linenoise output
helpers, recording exact definitions, closed callers and selection timing
separately. Do not require unrelated whole-library proof.

Run full Debug and ASan/UBSan suites, the existing fixed evaluations, new parser
and malformed-record tests, and focused clang-tidy/format checks. Add independent
format tests and paired counterexamples for every established output fact.

Compare the five pinned corpus projects against the preserved binary. Preserve
all 130 exact baseline-complete identities unless a separately demonstrated
false proof requires rejection. Record every added/removed diagnostic. Measure
three sequential ordinary baseline and final observations; median runtime and
peak RSS ratios must be at most 1.10. Checked reports must complete within 600
seconds per project. Warm runs must retain equivalent reports and zero function
analyses for unchanged reusable units. Keep failed observations visible.

## Annotation surface

None. Existing trust and checked selection controls keep their meanings.

## Diagnostics

Use existing `checking-incomplete` for missing runtime/format/list evidence and
`checking-failed` for established violations, with operation-specific reasons.
Existing lifetime, release, bounds and initialization diagnostics remain active.
Pin new reason text with unit and RFC-numbered lit tests. Warning suppression
does not establish a checked obligation.

## Drawbacks

Library semantics enlarge the trusted model and require maintenance. Formatting
and argument lists are target-sensitive, and return-dependent outputs can enlarge
contracts. Bounded parsing, strict signature/record checks, explicit trust and
adversarial tests contain those costs without silently broadening the guarantee.

## Alternatives

Adding only ordinary summary names does not prove memory safety. Treating all
libc calls as unsafe would undermine incremental adoption. Inlining library
implementations ties analysis to platform internals. Arbitrary symbolic format
execution is more expensive and less predictable than a bounded contract model.
Mutable-object inference and richer containers remain useful later milestones.

## Prior art

RFCs 0018–0019 separate sufficient preconditions from ordinary bug witnesses;
RFC 0022 supplies actual callback binding. Clang's format checking supplies
target type information but cannot by itself prove pointee initialization or
capacity. The library contracts follow the C/POSIX operation specifications:

- [POSIX formatted output](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fprintf.html)
- [POSIX read](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html)
- [POSIX fread](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fread.html)
- [Clang builtin and variadic semantics](https://clang.llvm.org/docs/LanguageExtensions.html)
- [C11 draft, library and stdarg](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)

## Unresolved questions

No unresolved design choice blocks the bounded initial population. An input
whose effects cannot be represented remains incomplete. New semantic scope
requires an explicit RFC amendment before implementation.

## Future work

Dynamic/positional/wide formats, `%n`, direct variadic extraction, scanning
functions, broader locale/OS contracts, archive distribution, inferred mutable
object invariants, recursive ownership and concurrency remain separate work.
