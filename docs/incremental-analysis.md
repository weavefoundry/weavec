# Reusing analysis work

[RFC 0020](rfcs/0020-scalable-modular-checked-analysis.md) adds optional work
statistics, persistent translation-unit checkpoints, and compact checked
reports. The cache does not change which operations must be checked or make
an incomplete contract complete.

## Commands

```sh
weavec --whole-program --checked \
  --analysis-cache=build/weavec-cache \
  --analysis-stats=build/analysis-stats.json \
  --checked-report-format=compact \
  --checked-report=build/checked.json a.c b.c -- -std=c17

weavec-cc -fweavec-checked -fweavec-analysis-cache=build/weavec-cache \
  -fweavec-analysis-stats=build/link-stats.json \
  -fweavec-checked-report-format=compact \
  -fweavec-checked-report=build/link-checked.json a.o b.o -o program
```

`weavec` enables persistent reuse for both independent source invocations and
`--whole-program`. The compiler uses checkpoints during source replay at link
time; compiling an object still runs Clang code generation and produces its
sidecar. A link cache does not authorize reuse of a stale object. Keep the
usual source, header, command and object bindings valid. Sidecar version 18
retains the preprocessing bindings introduced in version 16, including conditional-include probes. Rebuild
objects carrying older sidecars. A checked link rejects inputs whose preprocessing
cannot be reproduced or validated. Preprocessing is bound before AST creation
and verified afterwards. Observed input changes disable source cache reuse;
checked compiler replay fails if its binding changes while parsing.

The cache is off by default. Removing its directory is safe. Cache read or
write failures lose an optimization; they do not suppress diagnostics. An
explicitly requested statistics or checked-report output that cannot be
written fails the invocation.

## What is reused

Within an invocation, each retained Clang AST owns its function preparation.
CFGs and lexical lifetimes are reusable. Liveness is reused only while the
inferred nonreturning blocks agree; statement classification and mutable flow
state remain specific to the current analysis. Contextual summaries record
observed function and global-fact revisions, including unavailable callees.
Nested cache hits contribute their dependencies to the active caller. Silent
generic analyses also skip settled work under unchanged consulted revisions
within that unit run. Recursive reseeding, widening-mode changes and new
incompleteness invalidate that memo; reporting still runs. Whole-program
scheduling distinguishes imported summaries from context requests sent back
to defining units. It tracks actual symbol lookups (including missing symbols),
retains static imports and indirect candidate types, and remains conservative
for global facts. Internal callback symbols retain their source-file prefix.

Repeated imported contracts share their renumbered copy for the lifetime of
the translation-unit store. The source contract, destination AST and database
generation identify an import; changing the database creates a new generation.
Old imported pointers remain valid for active callers. Every lookup still
records its dependency and context request, including on a reuse hit.
Resolved generic, contextual and imported contracts are published as immutable
shared objects. Calls and their effects retain that same object across CFG
transfers; they do not keep a second full copy to extend its lifetime. Replacing
or invalidating a cache entry preserves any active caller's contract. Builtin
specialization and target joins use private copies before publication.

Between unit runs, whole-program analysis releases a stale working database
before constructing its replacement. Completed reporting releases each old
approximation, and then the working database, before extending the settled
program. This avoids keeping duplicate complete databases and unit exports
alive together; no active analysis loses its imported facts.

Ordinary whole-program runs without a persistent cache keep one recently used
AST after discovery, releasing preparation before evicting an AST. A currently
running unit can temporarily be the second. Revisited units are parsed again.
Checked mode, checked annotations or selections, checked input bindings, dumps
and persistent caches preserve their retained AST lifetimes. This keeps ordinary
memory use bounded without rebinding checked inputs or pending cache keys.

Checked analysis uses Clang's reverse-postorder worklist, retaining the
immutable order with the CFG. Acyclic joins follow their predecessors.
Ordinary analysis retains FIFO throughout, preserving convergence and the
memory cost of its legacy heuristic domains. Both orders reschedule changed
inputs under the same visit limits.
The final diagnostic pass keeps its existing traversal order.
Checked branch and nullness facts travel only through proven aliases. In
particular, two cursors that were equal before a loop can diverge; testing
one null must not erase reachable checks through the other.

Within a function, spatial facts join directly from the incoming maps,
preserving the existing null-object rule and unknown-pointer weakening.
The final diagnostic pass consumes settled non-exit states and avoids a
copy for the last edge. The original exit state remains available for
summary finalization, checked coverage and analysis dumps.
Alias relations keep value-owned adjacency maps. Ordered joins preserve
copies, offsets, element witnesses and share identity without adding transitive
aliases. Read-only mirror queries borrow edges while mutating callers retain
snapshots. Sharing entire alias rows did not establish a memory improvement.

Checked ledgers share both immutable entries and whole snapshots. A changed
ledger detaches its ordered index while preserving the storage of unchanged
identities, messages and call paths. Canonical call-origin projections are
also cached within a shared snapshot. An invocation-scoped pool shares exactly
equal entries across independent contexts. Its bounded index holds weak
references; resetting the index leaves all live ledger facts intact. These
representations preserve obligation limits and the weakest-outcome join.

The invocation also reuses callee-specific escaped origin identities across
call sites. This index retains at most 1,024 preparations and 64 MiB of
accounted storage, validates the originating projection's weak owner, and
bypasses oversized keys. Eviction only repeats preparation. Statistics expose
`explanation_call_hits`, `explanation_call_misses`, `explanation_call_resets`
and `explanation_call_rejections`. Caller identity, unsafe conversion,
truncation and destination ledger limits remain part of each application.
Exactly equal completed ledgers can also share their whole ordered index;
subsequent changes still detach before mutation.
Normalized call paths also have independently shared storage, so different
obligation rows can retain the same route. Edits detach and invalidate the
normalization marker; cycle collapse, origin retention and depth limits are
unchanged. Bulk propagation prepares the caller identity once and avoids
copying paths for entries rejected by the ledger's existing rules.

Persistent records contain settled unit exports, original diagnostics and
imported-fact identities. Recursive translation units are validated and
published together. A changed member rechecks its component; independent
components remain eligible for reuse. Imported callback target sets, type
candidate buckets, context requests, count fields and sized-field facts are
conservative shared dependencies. This can invalidate more work than the
smallest possible dependency graph.

A warm hit still validates inputs. It parses and preprocesses with the effective
compiler invocation, hashing expanded output, loaded source/header bytes,
compiler options, working directory, checking options and this executable's
contents. A timestamp match is insufficient. A newly available
`__has_include` target can invalidate a record even if no source file includes
it. Volatile date/time macros, modules, precompiled headers, VFS overlays and
frontend plugins bypass persistent reuse. Dump and annotation fix-it modes
also bypass it.

Records have their own version and content digest. Encoded files larger than 256 MiB,
malformed or noncanonical records, invalid diagnostic identifiers and
incompatible versions are misses. Payloads of at least 1 MiB use zstd when
available in LLVM, with a separate 4 GiB decoded-size bound enforced before
decompression. Without compression support, a large checkpoint may lose reuse.
Private checkpoint format 2 shares exact obligation rows, paths and ledgers
across definitions and specializations. It retains originating function names
and exhaustion flags, validates every reference before restoring a unit, and
checks that the remaining contract metadata survives serialization. The public
summary and compiler-sidecar formats retain their existing versions.
Writes use a unique neighboring temporary
file and atomic rename. Checkpoints are local build data, not a substitute for
reviewed external library contracts.

## Statistics

`--analysis-stats` writes JSON version 1. `counters` contains work counts;
`nanoseconds` contains cumulative timed work. A missing counter means no such
work was recorded. Timings of nested operations can overlap and must not be
summed as wall time.

Whole-program invocations atomically publish progress snapshots after completed
units with `final: false`; the final invocation snapshot has `final: true`.
This flag describes statistics collection, not successful checking. Interrupted
runs can leave a progress snapshot. Timers and pooled counters still active at
the snapshot boundary are finalized later. Per-function counts and inclusive
timings use `function:<main-source>#<name>`; summary-finalization timings use
`summary:<main-source>#<name>`. `generic:<symbol>`, `memory:<symbol>` and
`callback:<symbol>` include the full invocation and state destruction. Nested
timings overlap and cannot be summed as wall time. Disabled statistics do not
construct these labels or read the clock.

Useful counters include:

- `silent_function_reuses` for unchanged generic function analyses;
- `unit_invalidation_skips` for component edges whose inputs did not change;
- `program_import_hits`, `program_import_misses` for renumbered contracts;
- `summary_publications`, `summary_shared_uses` for immutable contracts;
- `unit_parses`, `unit_reuses`, `cfg_builds`, `cfg_reuses`,
  `cfg_order_builds`, `cfg_order_reuses`, `liveness_builds`, `liveness_reuses`,
  `unit_evictions`, `cfg_rpo_analyses`, `cfg_fifo_analyses`;
- `function_analyses`, `block_transfers`, `state_joins`, `summary_changes`;
- `specialization_hits`, `specialization_misses`, `specialization_invalidations`;
- `explanation_entry_hits`, `explanation_entry_misses`, `explanation_pool_resets`,
  `explanation_snapshot_hits`, `explanation_snapshot_misses`,
  `explanation_snapshot_resets`;
- `program_fixpoint_rounds`, `unit_fixpoint_rounds`, `function_fixpoint_rounds`;
- `cache_input_validations`, `cache_input_changes`, `cache_hits`, `cache_misses`,
  `cache_dependency_misses`, `cache_invalid_records`, `cache_writes`,
  `cache_write_failures`, `cache_uncompressed_bytes`, `cache_bytes_written`.

A warm unchanged reusable unit has no function analyses. A fast run with a
nonzero analysis count is not evidence of a persistent hit. Counters and timers
are excluded from proof semantics and cache identity.
Imported-fact fingerprints encode symbol and indirect-type keys losslessly;
spaces in a type spelling or source path do not make a unit uncacheable.

## Compact reports

Expanded JSON version 2 remains the default. Version 3 retains the same units,
functions, selection, requirements, guaranteed outputs, trust, completeness,
limits and invocation outcome. It replaces each function's obligation objects
with indices into shared tables:

| Table | Record |
| --- | --- |
| `strings` | A JSON string |
| `locations` | `[file_string_index, line, column]` |
| `call_paths` | An array of location indices |
| `obligation_records` | `[property_string_index, outcome_string_index, location_index, subject_string_index, reason_string_index, call_path_index]` |

Indices are zero based. `obligation_fields` names the six obligation columns.
Different source operations and different retained call paths remain distinct.
Call paths obey the same bounded, cycle-collapsing provenance rules as expanded
reports. Interning changes representation, not the originating evidence.
Both encodings stream to the atomic output file. Compact encoding reads
contracts directly instead of materializing expanded JSON first. The decoder
below retains shared tables and one expanded function at a time.

Consumers can expand a compact report without running the checker:

```sh
python3 scripts/checked-report.py build/checked.json -o build/expanded.json
```

The regression evaluation compares every expanded field, including origin
locations and call routes, rather than comparing only completion totals:

```sh
python3 scripts/incremental-evaluation.py \
  --weavec build/dev/bin/weavec --cc build/dev/bin/weavec-cc
```
