# Blind memory-safety detection set

48 single-file C11 programs. Each one has one real memory-safety bug that
`./prog` (no arguments) triggers deterministically. Each has a fixed twin
built with `-DFIX`. The line where the first invalid access or invalid free
happens carries a trailing `// STOP` comment, and each file's header comment
says what the bug is, its category, and why a reviewer might miss it. The
programs were written without looking at any other bug collection in this
repository.

Every program has been checked with Apple clang 21 (arm64 macOS):

- `clang -std=c11 -O1 -g -DFIX -fsanitize=address,undefined` exits 0 with no
  sanitizer report.
- `clang -std=c11 -O2 -Wall -Wextra` gives no warnings for either variant.
- `clang -std=c11 -O1 -g -fsanitize=address` (the bug variant) gives the
  result in the ASan column below.

Run the whole set with any compiler:

```sh
CC=clang CFLAGS='-O1 -g -fsanitize=address' ./run.sh        # all programs
CC=clang ./run.sh 2                                         # names containing "2"
```

`run.sh` prints `NN_name bug:<status> fix:<status>`. A status is the exit
code, `SIG<NAME>` for a signal (`SIGALRM` means the 10-second limit was hit),
or `build-failed`. Binaries and logs go to `$OUT`, or to a temporary
directory if `$OUT` is not set.

Categories: spatial 19, temporal 17, release 5, null 3, uninitialized 2,
threading 2.

ASan column (`-O1`):

- **yes**: ASan reports the bug itself.
- **SEGV**: ASan reports only a deadly signal at the faulting line, not a
  memory-safety diagnosis.
- **no**: ASan reports nothing.

| File | Category | Bug | ASan (-O1) |
| --- | --- | --- | --- |
| 01_ring_buffer_wrap.c | spatial | Ring buffer wrap test `head > cap` writes one record past the slot array | yes |
| 02_strbuf_terminator.c | spatial | String builder reserves no room for the NUL; 1-byte heap overflow at an exact power-of-two fill | yes |
| 03_csv_quoted_field.c | spatial | CSV unquoting caps characters at 16 but stores the terminator at `out[16]` (stack, 1 byte) | yes |
| 04_matrix_transpose.c | spatial | Transpose uses the source's column count as the destination stride (heap overflow) | yes |
| 05_lexer_peek.c | spatial | Lexer peeks one character past an unterminated, exactly-sized source buffer | yes |
| 06_strncpy_unterminated.c | spatial | `strncpy` into 8 bytes leaves no terminator; `strlen` over-reads the stack buffer | yes |
| 07_hash_table_rounding.c | spatial | Mask uses the rounded-up capacity but `calloc` uses the requested size; a hash-derived index overflows | yes |
| 08_histogram_max_bucket.c | spatial | Histogram maximum maps to bucket `NBUCKETS`, one past a global array | yes |
| 09_snprintf_offset.c | spatial | `snprintf(buf + off, sizeof buf, ...)` passes the full size at an offset (stack overflow) | yes |
| 10_tlv_record_length.c | spatial | Record length is checked against the input but copied into a 32-byte stack buffer (4 bytes over) | yes |
| 11_base64_encode_nul.c | spatial | Base64 output sized `(n+2)/3*4` without the NUL; 1-byte heap overflow | yes |
| 12_strip_slashes_underflow.c | spatial | Stripping trailing slashes from "/" reads `path[-1]` (stack underflow) | yes |
| 13_missing_sentinel.c | spatial | Lookup walks to a NULL sentinel that the heap table never got | yes |
| 14_utf8_truncated.c | spatial | UTF-8 decoder trusts the lead byte's length on a truncated chunk (heap over-read) | yes |
| 15_smallvec_intra_object.c | spatial | Embedded vector's push check `len > CAP` lets item 9 overwrite the next field (intra-object) | no |
| 16_realloc_bytes_not_elems.c | spatial | `realloc(v, ncap)` with an element count instead of bytes; first store overflows | yes |
| 17_letter_count_negative.c | spatial | `counts[c - 'a']` for an upper-case letter gives a negative index into a stack array | no (lands beyond the redzone) |
| 18_erase_memmove_count.c | spatial | Erase uses `memmove` count `n - i` instead of `n - i - 1`; heap over-read by one element | yes |
| 19_bitmap_round_down.c | spatial | Bitmap allocates `nbits / 8` bytes (rounds down); setting bit 96..99 overflows by 1 byte | yes |
| 20_list_purge_next.c | temporal | List purge frees a node, then the for-increment reads `s->next` from it | yes |
| 21_symtab_stale_pointer.c | temporal | References keep pointers into a symbol array that `realloc` later moves | yes |
| 22_label_out_param_stack.c | temporal | Label text points to a callee's local buffer stored through an out-parameter (stack use after return) | yes (as use-after-scope, after inlining) |
| 23_callback_ctx_freed.c | temporal | Closed session stays subscribed; next broadcast's callback updates the freed context | yes |
| 24_lru_evicted_value.c | temporal | Pointer returned by an LRU `get` is used after a `put` evicts and frees that entry | yes |
| 25_hashmap_iter_rehash.c | temporal | Inserting while iterating rehashes the map; the iterator reads the freed bucket array | yes |
| 26_arena_reset_stale.c | temporal | Connection keeps a Host pointer into a per-request arena that is reset and reused | no (arena block stays allocated) |
| 27_config_reload_dangling.c | temporal | Reload clears (frees) the old config before parsing; the error path prints the freed host | yes |
| 28_refcount_borrowed_unref.c | temporal | Borrowing helper drops a reference it never owned; the queue's `buf_ref` touches freed memory | yes |
| 29_global_ctx_stack.c | temporal | Stack parse context published in a global outlives its frame; a later warning reads it | yes (as use-after-scope, after inlining) |
| 30_queue_pop_name.c | temporal | `pop` frees the node and returns a pointer to its embedded name | yes |
| 31_bst_delete_successor.c | temporal | BST delete removes (frees) the successor before copying its key | yes |
| 32_timer_cancel_in_callback.c | temporal | Timer callback cancels (frees) its own timer; the loop then reads `t->next` | yes |
| 33_realloc_result_ignored.c | temporal | One call site ignores the moved buffer returned by a realloc-style append | yes |
| 34_block_scope_label.c | temporal | Pointer to a block-scoped buffer is used after the block ends | yes |
| 35_worklist_push_invalidates.c | temporal | Pointer to the current worklist item goes stale when pushing children reallocates | yes |
| 36_error_pointer_into_copy.c | temporal | Error out-parameter points into a private copy that is freed before the caller prints it | yes |
| 37_double_free_error_path.c | release | Decoder frees the body on a validation error without nulling it; the caller's cleanup frees it again | yes |
| 38_free_trimmed_value.c | release | `trim()` returns an interior pointer that is later passed to `free` | yes |
| 39_free_default_name.c | release | "Caller frees" function returns a global default buffer on one path | crash inside ASan's `free` (BUS), no diagnosis |
| 40_shallow_copy_double_free.c | release | Struct assignment copies the pixel pointer; both images free it | yes |
| 41_realloc_free_old.c | release | Frees the old pointer after `realloc` moved the block (already freed by `realloc`) | yes |
| 42_skip_level_null.c | null | Skip-level manager lookup checks one level for NULL but dereferences two | SEGV |
| 43_shape_parse_null.c | null | Parser returns NULL for an unknown shape kind; the caller passes it to `area()` | SEGV |
| 44_uninit_stream_buffer.c | uninitialized | Unbuffered stream's `buf` field is never set (`malloc`), then freed on close | SEGV (on the 0xbe fill) |
| 45_strchr_null_flag.c | null | `strchr(line, '=')` returns NULL for a bare flag line, which is then written through | SEGV |
| 46_uninit_token_text.c | uninitialized | Lexer leaves `text` unset for number tokens; the echo copies from the garbage pointer | SEGV (depends on stack contents) |
| 47_thread_stack_arg.c | threading | Worker threads read job structs from a helper's dead stack frame | yes (as use-after-scope, after inlining) |
| 48_logger_thread_freed_msg.c | threading | Logger queues caller pointers without copying; the writer thread reads them after they are freed | yes |

ASan diagnoses 39 of the 48 bugs. It ends 5 more with a SEGV report and 1
with a crash inside its own `free`. It reports nothing for 3: an
intra-object overflow (15), a negative index far below the array (17), and a
stale pointer into a recycled arena (26). For 22, 29 and 47, the bug is a use
after return, but at `-O1` the callee is inlined. ASan then reports it as
stack-use-after-scope. Without inlining it needs
`ASAN_OPTIONS=detect_stack_use_after_return=1`.
