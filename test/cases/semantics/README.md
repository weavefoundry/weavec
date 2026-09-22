# Semantics cases (RFC 0030)

Each case pins one rule of RFC 0030 (`docs/rfcs/0030-prove-or-trap.md`): the
worked examples of §4, the review cases §17.2 names, and a few cases per rule
of §5–§11. `test/cases/README.md` has the marker grammar and the runner.

Every file starts with two comment lines:

```c
// RFC 0030 §7.4: <the rule the case pins>
// STAGE: S3
```

## Stages

`STAGE` names the implementation stage (RFC 0030, *Implementation plan*)
whose work makes the case pass, judged in the mode that stage's gate uses:
`--no-emission` for S3 and S4, where a `TRAP` is matched by a checked facet
with that template, and the default trap mode from S5 on. Every case must
keep passing in trap mode once S5 has landed. To select one stage:

```sh
scripts/run-cases.py --no-emission $(grep -l '^// STAGE: S3' -r test/cases/semantics | sed 's|test/cases/|--filter |')
```

| Stage | Cases |
| --- | --- |
| S3 | 41 |
| S4 | 13 |
| S5 | 9 |
| S6 | 30 |
| S7 | 10 |
| S8 | 1 |
| total | 104 |

## Conventions

- `Inputs/rfc0030.h` gives fallback spellings of the macros weavec.h 0.9
  adds (`WEAVEC_COUNTED_BY`, `WEAVEC_ENDED_BY`, `WEAVEC_STRING`,
  `WEAVEC_REQUIRE_SAFE`), for a `weavec.h` that predates them. Each is
  guarded by `#ifndef`, so the header's own definition wins once it exists.
- A case that expects a trap has a `main` that reaches it and a `RUN-INPUT`
  per trap. Every run of a case with a `TRAP` marker must trap, so the
  correct use of a rule that must not trap is a separate `CLEAN` case.
- Values that decide a trap come from `argc`/`argv`, so the context runs of
  §2.6 see unknown values and report no definite error in the callee.
- `ASAN` marks the cases whose defect ASan confirms, built as the runner
  builds its oracle (`-fsanitize=address -fsanitize=array-bounds`). The
  `CLEAN` cases with `ASAN` also run clean under
  `-fsanitize=address,undefined`.
- A unit under `<feature>/Inputs/` belongs to one case (`UNITS`). A unit with
  `FLAGS: -fno-weavec` is a link input without a WeaveC record.

## The review cases of §17.2

| Review case | File |
| --- | --- |
| Lua's `upvals[1]` (flexible trailing array) | `extents/trailing-array-upvals.c` |
| `memcpy` into `&ts->contents` | `extents/member-address-memcpy.c` |
| `memset` through a first member | `extents/member-address-memset.c` |
| flat walk of `m[3][4]` | `extents/flat-walk.c` (and `extents/subarray-subscript.c` for the row bound) |
| `qsort` comparator (clean) | `kinds/qsort-comparator.c` |
| `p + 1` into a callee given `int[2]` (clean) | `kinds/plus-one-into-pair.c` |
| sentinel read `p[n]` (clean) | `kinds/sentinel-read.c` |
| `two(p, p)` | `aliasing/two-same-argument.c` |
| `b->cur` aliasing `b->data` | `aliasing/cursor-into-data.c` |
| `get(a + 4, …)` into a static function | `kinds/static-cursor-argument.c` |
| a cursor stored through a `char **` | `kinds/cursor-through-char-pp.c` |
| `remember(p); free(p); peek()` | `boundary/remember-free-peek.c` |
| `free(g); exit(0)` with an `atexit` reader | `boundary/atexit-reader.c` |
| `realloc(p, 0)` | `library/realloc-zero-definite.c`, `library/realloc-zero-possible.c` |
| an empty vector's `memcpy(NULL, NULL, 0)` | `library/memcpy-null-empty.c` |
| a negative count field | `emission/negative-count.c` |
| a wrapping `qsort` size | `emission/qsort-wrapping-size.c` |
| a zero-trip loop | `kinds/zero-trip-loop.c` |
| a `size_t` `i - 1` loop | `kinds/size-t-minus-one-loop.c` |
| a call that exits before the access | `kinds/exits-before-access.c` |
| a cast end sentinel | `extents/cast-end-sentinel.c` |
| a flexible-array struct smaller than `sizeof` | `extents/flexible-smaller-than-sizeof.c` |
| the `ok()` guard | `library/guard-ok.c` |
| a lossy wrapper | `library/wrapper-lossy.c` |
| a `nonnull`-only destructor | `ledger/nonnull-destructor.c` |
| a handler racing a null test | `concurrency/handler-race.c` |
| a worker using a freed global | `concurrency/worker-freed-global.c` |
| a C99 `inline` definition inlined at `-O2` | `emission/c99-inline-o2.c` |
| `fp = memcpy` | `slots/fp-memcpy.c`, `slots/fp-memcpy-open.c` |
| a `-Wno-error`-lowered out-of-bounds store | `emission/lowered-out-of-bounds.c` |

## Readings of the RFC these cases rely on

- §4 *Require levels* quotes one error for the unknown-extent example, but
  its call to `get_buffer` is unresolved(unknown-callee) too, which §6.3
  makes a second `unresolved-operation` error; `examples/require-checked.c`
  pins both.
- §4 examples that pair a definite error with code that must build (`g`/`h`,
  the two assumptions) are split into two cases, and the link half of the
  unknown-callee example is its own S8 case.
- Trap templates the RFC implies without naming: a count requirement at a
  call is `len` (§10.4), a `str` requirement is `len` (§10.3 rule 2), a
  lowered spatial violation gets its facet's own `index` check and a lowered
  temporal one `violation` (§3.4). No template is named for `EndedBy` at a
  call, so `kinds/declared-ended-by.c` pins a definite violation instead.
- Where §5.3's `trusted(concurrency)` and §9.4's propagated
  `dangling-escape` could both apply to a worker's access, the case pins
  `NOT-PROVEN: temporal`. The §4 example keeps `TRUSTED`, as the RFC states
  it; §14 applies the concurrency override after propagation.
- An open slot with known targets is `trusted(extern-contract)` in §9.3 but
  may become `unresolved(callback)` (*Unresolved questions*), so
  `slots/fp-memcpy-open.c` pins `NOT-PROVEN: temporal`.
- `a5` is read from the unit's own summary (`/units/0/summary/a5`), where
  §12.1 puts it.
- Whether an unreachable end of body is an exit site is not stated, so
  `ledger/no-site-unevaluated.c` counts sites in functions without `return`.
- Whether a C99 `inline` definition's inferred requirement is checked at its
  calls (like a static function) or trusted (like an exported one) is not
  stated, so `emission/c99-inline-o2.c` guards its access and the check
  stays in the body either way.
- The "`size_t` `i - 1` loop" is read as R2 with `k = -1`: a downward loop
  is not canonical and gives no requirement.

## Cases

One row per case: the section it pins, its stage, and what it expects (`BUG` ids with severity, `TRAP` templates, `rows` for ledger markers or `EXPECT-LEDGER`, `ASan` for the oracle, then `FLAGS` and extra units).

### `examples/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `assumption-checked.c` | §4 "Assumptions" (a) | S3 | trap assert |
| `assumption-contradicted.c` | §4 "Assumptions" (c) | S3 | contradicted-assumption definite; ASan |
| `checked-index.c` | §4 "Checked, index" | S6 | trap index; trap nonnull |
| `checked-library-length.c` | §4 "Checked, library length" | S6 | trap len; trap nonnull; rows |
| `checked-null.c` | §4 "Checked, null" | S6 | trap nonnull; rows |
| `proven-static-extent.c` | §4 "Proven" | S6 | clean; rows |
| `require-checked.c` | §4 "Require levels" | S3 | unresolved-operation definite; rows; `-fweavec-require=checked` |
| `require-proven.c` | §4 "Require levels" | S6 | unchecked-operation definite; rows; `-fweavec-require=proven` |
| `temporal-definite.c` | §4 "Violation versus possible, temporal" (g) | S3 | use-after-free definite; ASan |
| `temporal-possible.c` | §4 "Violation versus possible, temporal" (h) | S3 | use-after-free possible; rows; ASan |
| `trusted-concurrency.c` | §4 "Trusted, concurrency" | S4 | use-after-free; rows; ASan |
| `trusted-system-api.c` | §4 "Trusted, system API" | S3 | clean; rows |
| `trusted-unsafe.c` | §4 "Trusted, unsafe" | S3 | clean; rows |
| `unresolved-inexpressible.c` | §4 "Unresolved, inexpressible" | S6 | rows |
| `unresolved-unknown-callee-link.c` | §4 "Unresolved, unknown callee", at link | S8 | use-after-free definite; ASan; + unit |
| `unresolved-unknown-callee.c` | §4 "Unresolved, unknown callee" | S3 | clean; rows |
| `unresolved-unknown-extent.c` | §4 "Unresolved, unknown extent" | S3 | trap nonnull; rows |
| `violation-null.c` | §4 "Violation, null" | S3 | null-dereference definite; ASan |
| `violation-spatial.c` | §4 "Violation, spatial" | S3 | out-of-bounds definite; ASan |

### `ledger/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `budget.c` | §5.5 | S3 | trap nonnull; trap index; rows; `-fweavec-budget=1` |
| `extern-contract.c` | §5.1 | S3 | clean; rows |
| `inline-asm.c` | §5.7 | S3 | clean; rows |
| `no-site-unevaluated.c` | §2.1 | S3 | clean; rows |
| `nonnull-destructor.c` | §5.1 | S3 | clean; rows |
| `setjmp.c` | §5.4 | S3 | trap nonnull; rows |
| `system-api-borrow.c` | §5.2 | S3 | clean; rows |
| `unknown-then-free.c` | §5.1 and §3.1 | S3 | use-after-free definite; rows |

### `unsafe/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `assume-inside-region.c` | §6.1 | S3 | trap assert |
| `definite-violations.c` | §6.1 | S3 | use-after-free definite; out-of-bounds definite |
| `possible-temporal.c` | §6.1 | S3 | use-after-free possible; rows |
| `raw-outside-region.c` | §6.1 and §2.3 | S3 | unsafe-operation definite; rows |
| `trusted-refines-nothing.c` | §6.1 and §3.2 | S3 | trap nonnull; rows |

### `assume/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `assumed-after-site.c` | §6.2 | S3 | trap assert; rows |
| `contradicted.c` | §6.2 | S3 | contradicted-assumption definite |
| `proven.c` | §6.2 | S3 | clean; rows |

### `require/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `checked-allowed.c` | §6.3 | S6 | clean; `-fweavec-require=checked` |
| `dangling-escape-error.c` | §6.3 | S7 | unresolved-operation definite; `-fweavec-require=checked` |
| `require-safe.c` | §6.3 | S3 | unresolved-operation definite; rows |
| `trusted-allowed.c` | §6.3 | S3 | clean; rows; `-fweavec-require=proven` |

### `kinds/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `attr-alloc-size.c` | §7.2 (alloc_size) | S6 | trap index; + unit |
| `attr-counted-by.c` | §7.2 (counted_by, counted_by_or_null, sized_by, sized_by_or_null) | S6 | out-of-bounds; trap index; ASan |
| `attr-nonnull.c` | §7.2 (nonnull, _Nonnull) | S6 | trap nonnull |
| `attr-ownership.c` | §7.2 (malloc, ownership_returns, ownership_takes, ownership_holds) | S6 | use-after-free definite; rows |
| `const-array-param.c` | §7.2 | S6 | clean; ASan |
| `cursor-through-char-pp.c` | §7.3 | S6 | out-of-bounds; rows; ASan |
| `declared-counted-by-field.c` | §7.2 (WEAVEC_COUNTED_BY on a field) | S6 | out-of-bounds; trap index; ASan |
| `declared-counted-by-param.c` | §7.2 (WEAVEC_COUNTED_BY on a parameter) | S6 | out-of-bounds; trap len; ASan |
| `declared-ended-by.c` | §7.2 (WEAVEC_ENDED_BY) | S6 | out-of-bounds definite |
| `declared-invalid-name.c` | §7.2 | S6 | invalid-annotation possible; rows |
| `declared-nonnull-definite.c` | §7.2 (WEAVEC_NONNULL on a parameter) | S3 | null-dereference definite |
| `declared-nonnull.c` | §7.2 (WEAVEC_NONNULL on a parameter) | S6 | null-dereference; trap nonnull; ASan |
| `declared-nullable.c` | §7.2 (WEAVEC_NULLABLE on a result) | S6 | rows |
| `declared-sized-by.c` | §7.2 (WEAVEC_SIZED_BY) | S6 | out-of-bounds; trap len; ASan |
| `declared-static-array.c` | §7.2 (T p[static N]) | S6 | out-of-bounds definite |
| `declared-string.c` | §7.2 (WEAVEC_STRING) | S6 | out-of-bounds; trap len; ASan |
| `declared-vla-param.c` | §7.2 (T p[n], a VLA parameter) | S6 | out-of-bounds; trap len; rows; ASan |
| `exits-before-access.c` | §7.5 | S6 | clean; ASan |
| `plus-one-into-pair.c` | §7.1 | S3 | clean; ASan |
| `qsort-comparator.c` | §7.1 | S3 | clean; rows; ASan |
| `relies-on-single-call.c` | §7.3 | S6 | out-of-bounds; rows; ASan |
| `sentinel-read.c` | §7.1 | S6 | clean; rows; ASan |
| `size-t-minus-one-loop.c` | §7.5 | S6 | clean; ASan |
| `static-cursor-argument.c` | §7.3 | S6 | out-of-bounds; rows; ASan |
| `zero-trip-loop.c` | §7.5 | S6 | clean; ASan |

### `extents/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `cast-end-sentinel.c` | §7.4 | S3 | clean; ASan |
| `flat-walk.c` | §7.4 | S3 | clean; ASan |
| `flexible-smaller-than-sizeof.c` | §7.4 | S3 | clean; ASan |
| `member-address-memcpy.c` | §7.4 | S3 | clean; ASan |
| `member-address-memset.c` | §7.4 | S3 | clean; ASan |
| `subarray-subscript.c` | §7.4 | S3 | out-of-bounds; trap index; ASan |
| `trailing-array-upvals.c` | §7.4 | S3 | clean; rows; ASan |

### `library/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `guard-ok.c` | §9.2 | S4 | trap nonnull |
| `longjmp-noreturn.c` | §8.3 | S4 | rows |
| `memcpy-null-empty.c` | §8.3 | S4 | clean; ASan |
| `realloc-zero-definite.c` | §8.2 (realloc) | S4 | double-free definite |
| `realloc-zero-possible.c` | §8.2 (realloc) | S4 | double-free possible |
| `regfree-local.c` | §8.2 | S4 | clean; ASan |
| `releasers.c` | §8.3 | S4 | use-after-free definite |
| `static-results.c` | §8.3 | S4 | clean; ASan |
| `system-null.c` | §8.3 | S4 | clean; ASan |
| `wrapper-lossy.c` | §9.1 | S7 | use-after-free possible; ASan |
| `wrapper-param-keyed.c` | §9.1 | S7 | use-after-free definite |
| `wrapper-result-keyed.c` | §9.1 | S7 | clean; ASan |
| `zero-length-null.c` | §8.3 | S4 | clean; ASan |

### `slots/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `fp-memcpy-open.c` | §8 and §9.3 | S7 | rows |
| `fp-memcpy.c` | §8 and §9.3 | S7 | out-of-bounds; trap len; ASan |
| `open-slot-callback.c` | §9.3 and §5.1 | S7 | trap nonnull; rows |

### `boundary/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `atexit-reader.c` | §9.4 and §5.3 | S7 | use-after-free; rows; ASan |
| `remember-free-peek.c` | §9.4 | S7 | use-after-free; rows; ASan |

### `aliasing/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `cursor-into-data.c` | §3.1 | S3 | use-after-free; rows; ASan |
| `owning-fields-distinct.c` | §3.1 | S7 | clean; rows |
| `two-same-argument.c` | §3.1 and §2.6 | S3 | use-after-free definite; rows; ASan |
| `unknown-callee-later-named.c` | §5.1 | S7 | not proven: a place first named after an unknown call (a soundness gap found in S4) |

### `concurrency/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `handler-race.c` | §5.3 | S4 | null-dereference; trap nonnull; rows; ASan |
| `worker-freed-global.c` | §5.3 | S4 | use-after-free; rows; ASan |

### `zero-init/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `bypassed-declaration.c` | §11 | S5 | rows |
| `lowered-malloc-field.c` | §11 | S5 | trap nonnull |
| `no-zero-init-flag.c` | §11 | S5 | rows; `-fno-weavec-zero-init` |
| `uninitialised-pointer-local.c` | §11 | S5 | trap nonnull |

### `emission/`

| Case | Pins | Stage | Expects |
| --- | --- | --- | --- |
| `c99-inline-o2.c` | §2.6 | S5 | trap nonnull; `-O2`; + unit |
| `lowered-out-of-bounds.c` | §3.4 | S5 | out-of-bounds possible; trap index; ASan; `-Wno-error=weavec-out-of-bounds` |
| `lowered-use-after-free.c` | §3.4 | S5 | use-after-free possible; trap violation; ASan; `-Wno-error=weavec-use-after-free` |
| `memcpy-zero-then-deref.c` | §8.3 | S5 | trap nonnull; `-O2` |
| `negative-count.c` | §10.2 | S6 | trap index |
| `qsort-wrapping-size.c` | §10.2 | S5 | out-of-bounds; trap len; ASan |
