// RUN: %weavec --checked-function=main %S/../evaluation/rfc0027/runtime-good.c -- 2>&1 | FileCheck %s --check-prefix=GOOD --allow-empty
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0027/reverse-drop-bad.c -- -Wno-everything 2>&1 | FileCheck %s --check-prefix=DROP
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0027/borrowed-tree-cycle-bad.c -- -Wno-everything 2>&1 | FileCheck %s --check-prefix=CYCLE
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0027/partial-cleanup-bad.c -- -Wno-everything 2>&1 | FileCheck %s --check-prefix=PARTIAL
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0027/saved-alias-bad.c -- -Wno-everything 2>&1 | FileCheck %s --check-prefix=ALIAS
// RFC 0027: conservation and structural validity are independent obligations.
// These source fixtures are frozen; the lit checks pin their public messages.
// GOOD-NOT: error:
// DROP: error: cannot establish checked safety: container operation loses part of the owned allocation footprint [weavec::checking-incomplete]
// DROP: error: checked safety requirements were not established
// CYCLE: error: cannot establish checked safety: callee recursive ownership precondition must hold [weavec::checking-incomplete]
// CYCLE: error: checked safety requirements were not established
// PARTIAL: error: cannot establish checked safety: recursive cleanup does not establish complete input footprint consumption [weavec::checking-incomplete]
// PARTIAL: partial-cleanup-bad.c:2:50: note: checked obligation originates here
// PARTIAL: partial-cleanup-bad.c:2:36: note: checked obligation originates here
// PARTIAL: error: checked safety requirements were not established
// ALIAS: error: checked safety failed: use of 'saved' after it was freed [weavec::checking-failed]
// ALIAS: saved-alias-bad.c:2:82: note: checked obligation originates here
// ALIAS: error: use of 'saved' after it was freed [weavec::use-after-free]
// ALIAS: saved-alias-bad.c:2:82: note: freed here (through 'p->left')
