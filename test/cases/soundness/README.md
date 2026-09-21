# Soundness probes

The 113 soundness probes of RFC 0030 (*Motivation*, *Projected outcomes on the
soundness probes*, gate G4): 85 programs with a real memory-safety bug
(`*_bug.c`) and 28 correct twins (`*_ok.c`, plus the two correct programs
`41e_infeasible_double_free_fp.c` and `48_simple_sum_fp.c`). They were
imported unchanged in S0 apart from their markers, a header comment on each
extra unit and one added unit; the file names are the original ones. `scripts/run-cases.py` runs them
(`test/cases/README.md` has the marker grammar):

```sh
scripts/run-cases.py --filter 'soundness/**' --asan                     # gate G4
WEAVEC_GOLDEN_DIR=<golden> scripts/run-cases.py --legacy --filter 'soundness/**'   # S0
```

## Markers

- Every bug probe has one `BUG` marker with the probe's *matching id*, the
  id v0.10.0 reports for the caught probes, on the line where RFC 0030
  projects the report. That is the ASan-confirmed faulting line except
  where the report belongs elsewhere: at the escape for
  `lifetime-too-short` (11, 12, 27), at the call whose argument breaks the
  callee's requirement (02f, 09, 20b, 28, 33, 36, 46), at the contradicted
  assumption (39, c03), at the declaration the link step verifies (38) and
  at the arity-mismatched `printf` (18, whose ASan input exercises the
  user-controlled format on the line before). 23 has a second `BUG`, the
  `alloca` storage returned from `make` (a `lifetime-too-short` error by the
  RFC's Diagnostics table). The ASan column below gives the faulting line.
- `definite` is pinned only where G4 requires an error: 38, 45 and c03.
- `TRAP` pins the projected template on the 12 probes projected to trap.
- `NOT-PROVEN: <facet>` sits beside the `BUG` of the 18 probes projected
  to be reported by a ledger row only: the row satisfies their `BUG`
  marker, as G4 counts it, but a diagnostic or trap would too. Their
  projected reasons are in the table; the markers do not pin them.
- `NEUTRALISED: zero-init` marks 08 and 32 (uninitialised scalars, outside
  every facet).
- `ASAN` marks the 75 bug probes the ASan oracle confirms
  (`-fsanitize=address,array-bounds` with `detect_stack_use_after_return=1`)
  and every correct twin. The ten bug probes without it are listed with the
  reason in the table.
- `RUN-INPUT` gives the arguments that reach the bug (06, 07, 07b, 18, 19,
  29, 31, 34, 49, c09) and, for the twins, the same inputs.
- `UNITS`: 38 links `38_extern_impl.c`, which the link step must verify.
  14, 14b and c13 link a definition that the analysis must not see, so
  their units (`14_extern_impl.c`, `14b_extern_impl.c`, and
  `c13_extern_impl.c`, added in the import because c13 did not link) carry
  `FLAGS: -fno-weavec`: they are compiled as plain Clang and the legacy run
  analyses the probe alone, as v0.10.0 was measured.

Not imported: `38b_lying_annotation_same_tu_bug.c`, `51_deep_path_uaf_bug.c`,
`51b_deep_path_uaf_bug.c` and `52_sysheader_unknown_release_bug.c` sat in the
probe directory but are not among the 113 the RFC's numbers count.

## Bug probes

"v0.10.0" is the classification of the golden run (`--legacy`), which agrees
with the measured table for every probe: 36 CAUGHT, 42 SILENT, 4 SIGNAL,
2 MISLABEL, 1 LEAK-ONLY. "Projected" is the RFC's class: still reported (38),
a new definite error (13), a runtime trap (12), a possible warning (2), a
ledger row only (18, with the projected reason) and neutralised (2). Line
numbers are those of the files here.

Three probes carry a `MISS` marker instead of their `BUG`, because the build
does not reach the projection and the reason is not a marker to be tightened
but an engine gap each file records in full: `07_strcpy_stack_overflow_bug`
(the `len` term would name `argv[1]`, which §10.3 rule 1 cannot spell),
`26_container_uaf_bug` (the filling loop is not the contiguous-fill shape, so
the read reports `use-of-uninitialized` instead) and `41_many_cells_uaf_bug`
(64 cells exceed `core::MaxArrayCells`). All three keep their `ASAN` marker,
and none of the three has the matching facet proven at its line, so gate G4's
silent count is still 0. Two clean twins carry `ALLOW: leak` for a false
possible leak they cannot yet shake: `03_double_free_loop_ok` and
`26_container_uaf_ok`.

| Probe | `BUG` (line: id) | v0.10.0 | Projected | ASan (first in-case frame) |
| --- | --- | --- | --- | --- |
| `01_uaf_field_alias_bug` | 15: use-after-free | CAUGHT | still reported | heap-use-after-free @15 |
| `02_uaf_global_bug` | 6: use-after-free | SILENT | row: dangling-escape | heap-use-after-free @6 |
| `02b_uaf_global_direct_bug` | 10: use-after-free | CAUGHT | still reported | heap-use-after-free @10 |
| `02c_uaf_global_free_in_helper_bug` | 7: use-after-free | SILENT | row: dangling-escape | heap-use-after-free @7 |
| `02d_uaf_heap_field_helper_bug` | 6: use-after-free | SILENT | row: dangling-escape | heap-use-after-free @6 |
| `02e_uaf_heap_field_direct_bug` | 11: use-after-free | CAUGHT | still reported | heap-use-after-free @11 |
| `02f_uaf_param_read_helper_bug` | 9: use-after-free | CAUGHT | still reported | heap-use-after-free @3 |
| `03_double_free_loop_bug` | 10: double-free | CAUGHT | still reported | attempting double-free @10 |
| `04_iter_invalidation_realloc_bug` | 14: use-after-move | CAUGHT | still reported | heap-use-after-free @14 |
| `05_oob_while_offbyone_bug` | 7: out-of-bounds | CAUGHT | still reported | index 8 out of bounds @7 |
| `06_int_overflow_malloc_bug` | 11: out-of-bounds | SILENT | trap | heap-buffer-overflow @11 |
| `07_strcpy_stack_overflow_bug` | 21: `MISS` (was 9: out-of-bounds) | SILENT | trap | stack-buffer-overflow @21 |
| `07b_sprintf_stack_overflow_bug` | 7: out-of-bounds | SILENT | trap | stack-buffer-overflow @7 |
| `08_uninit_index_bug` | 7: use-of-uninitialized | SILENT | neutralised | — (uninitialised scalar (MSan class)) |
| `09_type_confusion_voidp_bug` | 11: out-of-bounds | SILENT | definite error | heap-buffer-overflow @6 |
| `10_union_punning_bug` | 9: out-of-bounds | SILENT | row: raw-cast | SEGV @9 |
| `11_dangling_outparam_bug` | 5: lifetime-too-short | CAUGHT | still reported | stack-use-after-return @10 |
| `12_local_in_global_bug` | 6: lifetime-too-short | CAUGHT | still reported | stack-use-after-return @10 |
| `13_uaf_callback_bug` | 14: use-after-free | CAUGHT | still reported | heap-use-after-free @14 |
| `13b_uaf_callback_registry_bug` | 17: use-after-free | CAUGHT | still reported | heap-use-after-free @17 |
| `14_unknown_extern_frees_bug` | 12: use-after-free | SIGNAL | row: unknown-callee | heap-use-after-free @12 |
| `14b_unknown_extern_buffer_bug` | 6: out-of-bounds | SIGNAL | row: unknown-extent | — (index lands past the redzone) |
| `14c_unknown_extern_param_bug` | 2: out-of-bounds | SILENT | row: caller-contract (trusted) | — (no main (exported function)) |
| `15_data_race_thread_bug` | 14: use-after-free | SILENT | row: concurrency (trusted) | heap-use-after-free @14 |
| `15b_signal_handler_bug` | 13: use-after-free | SILENT | row: concurrency (trusted) | heap-use-after-free @13 |
| `16_setjmp_longjmp_bug` | 12: use-after-free | LEAK-ONLY | row: setjmp | heap-use-after-free @12 |
| `16b_longjmp_dead_frame_bug` | 7: lifetime-too-short | SILENT | row: setjmp | — (longjmp into a dead frame is not an ASan error) |
| `17_variadic_misuse_bug` | 8: out-of-bounds | SILENT | row: raw-cast | SEGV @8 |
| `18_format_string_bug` | 8: out-of-bounds | SILENT | definite error | SEGV @7 |
| `19_vla_size_bug` | 9: out-of-bounds | SIGNAL | definite error | index 4 out of bounds @9 |
| `20_ptr_arith_past_end_bug` | 8: out-of-bounds | SILENT | trap | stack-buffer-overflow @8 |
| `20b_ptr_walk_bug` | 10: out-of-bounds | SILENT | definite error | stack-buffer-overflow @5 |
| `21_memcpy_overlap_bug` | 6: out-of-bounds | SILENT | definite error | memcpy-param-overlap @6 |
| `22_realloc_zero_bug` | 10: use-after-move | CAUGHT | still reported | attempting double-free @10 |
| `22b_realloc_alias_bug` | 11: use-after-move | CAUGHT | still reported | heap-use-after-free @11 |
| `23_alloca_bug` | 7: lifetime-too-short; 11: out-of-bounds | SILENT | definite error | stack-buffer-overflow @11 |
| `24_flexible_array_bug` | 11: out-of-bounds | SILENT | trap | heap-buffer-overflow @11 |
| `25_strtok_static_bug` | 9: use-after-free | SILENT | definite error | — (strtok reads the freed buffer inside libc) |
| `25b_string_literal_write_bug` | 6: out-of-bounds | SILENT | definite error | BUS @6 |
| `25c_static_result_bug` | 9: use-after-free | SILENT | possible warning | heap-use-after-free @9 |
| `26_container_uaf_bug` | 14: `MISS` (was 8: use-after-free) | MISLABEL | still reported | heap-use-after-free @14 |
| `27_struct_return_dangling_bug` | 7: lifetime-too-short | CAUGHT | still reported | stack-use-after-return @11 |
| `28_free_non_heap_bug` | 7: invalid-release | CAUGHT | still reported | attempting free @3 |
| `29_negative_index_bug` | 9: out-of-bounds | SILENT | trap | index -1 out of bounds @9 |
| `30_unterminated_strlen_bug` | 6: out-of-bounds | CAUGHT | still reported | stack-buffer-overflow @6 |
| `31_truncation_memcpy_bug` | 10: out-of-bounds | SILENT | trap | stack-buffer-overflow @10 |
| `32_uninit_heap_read_bug` | 5: use-of-uninitialized | SILENT | neutralised | — (uninitialised heap read (MSan class)) |
| `33_double_free_aliased_records_bug` | 13: double-free | CAUGHT | still reported | attempting double-free @6 |
| `34_goto_cleanup_double_free_bug` | 15: double-free | CAUGHT | still reported | attempting double-free @15 |
| `35_uninit_ptr_field_bug` | 5: use-of-uninitialized | CAUGHT | still reported | — (stack garbage, nondeterministic) |
| `36_heap_oob_param_bug` | 9: out-of-bounds | SILENT | trap | heap-buffer-overflow @4 |
| `37_exported_api_no_callers_bug` | 7: out-of-bounds | SILENT | row: unknown-extent | — (no main (exported API)) |
| `38_lying_annotation_bug` | 6: annotation-mismatch definite | SILENT | definite error | heap-use-after-free @12 |
| `39_false_assume_bug` | 8: contradicted-assumption | SILENT | trap | index 11 out of bounds @9 |
| `40_struct_copy_uaf_bug` | 13: use-after-free | CAUGHT | still reported | heap-use-after-free @13 |
| `41_many_cells_uaf_bug` | 15: `MISS` (was 8: use-after-free) | MISLABEL | still reported | heap-use-after-free @15 |
| `41b_late_iteration_uaf_bug` | 11: use-after-free | CAUGHT | still reported | heap-use-after-free @11 |
| `41c_deep_call_chain_bug` | 15: use-after-free | CAUGHT | still reported | heap-use-after-free @15 |
| `41d_many_branches_bug` | 14: use-after-free | CAUGHT | still reported | heap-use-after-free @14 |
| `42_byte_copied_pointer_bug` | 12: use-after-free | SILENT | row: raw-cast | heap-use-after-free @12 |
| `43_list_free_then_next_bug` | 6: use-after-free | CAUGHT | still reported | heap-use-after-free @6 |
| `44_vector_element_ptr_bug` | 20: use-after-move | CAUGHT | still reported | heap-use-after-free @20 |
| `45_unsafe_region_hides_bug` | 8: use-after-free definite | SILENT | definite error | heap-use-after-free @8 |
| `46_param_null_deref_bug` | 9: null-dereference | SILENT | definite error | SEGV @5 |
| `47_uaf_via_qsort_cb_bug` | 16: use-after-free | SILENT | possible warning | heap-use-after-free @16 |
| `49_negative_index_via_helper_bug` | 6: out-of-bounds | SILENT | trap | index -1 out of bounds @6 |
| `50_param_passthrough_uaf_bug` | 4: use-after-free | CAUGHT | still reported | heap-use-after-free @4 |
| `c01_global_realloc_between_bug` | 17: out-of-bounds | SILENT | row: inexpressible | heap-buffer-overflow @17 |
| `c02_param_global_alias_bug` | 5: use-after-free | CAUGHT | still reported | heap-use-after-free @5 |
| `c03_contradictory_assume_bug` | 6: contradicted-assumption definite | SILENT | definite error | index 10 out of bounds @7 |
| `c04_union_tag_confusion_bug` | 5: out-of-bounds | SILENT | row: raw-cast | SEGV @5 |
| `c05_realloc_shrink_bug` | 8: out-of-bounds | CAUGHT | still reported | heap-buffer-overflow @8 |
| `c06_fam_sizeof_bug` | 9: out-of-bounds | SILENT | definite error | heap-buffer-overflow @9 |
| `c07_fnptr_cast_bug` | 11: use-after-free | CAUGHT | still reported | heap-use-after-free @11 |
| `c08_recursive_free_bug` | 11: use-after-free | CAUGHT | still reported | heap-use-after-free @11 |
| `c09_strncpy_unterminated_bug` | 8: out-of-bounds | SILENT | trap | stack-buffer-overflow @8 |
| `c10_double_free_two_globals_bug` | 4: double-free | CAUGHT | still reported | attempting double-free @4 |
| `c10b_double_free_two_params_bug` | 3: double-free | CAUGHT | still reported | attempting double-free @3 |
| `c10c_double_free_two_fields_bug` | 4: double-free | CAUGHT | still reported | attempting double-free @4 |
| `c10d_uaf_two_globals_bug` | 4: use-after-free | CAUGHT | still reported | heap-use-after-free @4 |
| `c10e_global_and_param_bug` | 4: double-free | CAUGHT | still reported | attempting double-free @4 |
| `c11_loop_40_iterations_bug` | 7: out-of-bounds | CAUGHT | still reported | index 40 out of bounds @7 |
| `c12_uninit_struct_field_bug` | 3: use-of-uninitialized | SILENT | trap | — (stack garbage, nondeterministic) |
| `c13_errno_style_extern_global_bug` | 4: out-of-bounds | SILENT | row: unknown-extent | — (index lands past the redzone) |
| `c14_memcpy_ptr_partial_bug` | 10: out-of-bounds | SIGNAL | row: raw-cast | SEGV @10 |

## Correct twins

All 28 are `CLEAN` and `ASAN` and run clean under ASan. v0.10.0 reports
diagnostics on four of them, so they fail in `--legacy` (24 of 28 clean, as
measured): `03_double_free_loop_ok` (two leaks), `13b_uaf_callback_registry_ok`
(`annotation-required`), `26_container_uaf_ok` (leaks and a null-dereference
error) and `41e_infeasible_double_free_fp` (a double-free error). Gate G5
requires all 28 to build without errors and run their `RUN-INPUT`s without a
trap.
