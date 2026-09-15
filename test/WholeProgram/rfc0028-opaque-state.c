// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0028/buffer-good.c %S/../evaluation/rfc0028/library.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0028/hooks-explicit-good.c %S/../evaluation/rfc0028/library.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0028/saved-alias-bad.c %S/../evaluation/rfc0028/library.c -- 2>&1 | FileCheck %s --check-prefix=ALIAS
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0028/buffer-uninitialized-bad.c %S/../evaluation/rfc0028/library.c -- 2>&1 | FileCheck %s --check-prefix=INITIALIZATION
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0028/state-disabled-bad.c %S/../evaluation/rfc0028/library.c -- 2>&1 | FileCheck %s --check-prefix=STATE
// RFC 0028: representation does not grant ownership, lifetime or initialized
// bytes. These clients see only forward declarations in the public header.
// CLEAN-NOT: error:
// ALIAS: error: checked safety failed: use of 'q' after it was freed [weavec::checking-failed]
// INITIALIZATION: error: cannot establish checked safety: read interval must be initialized [weavec::checking-incomplete]
// STATE: error: cannot establish checked safety: container operation loses part of the owned allocation footprint [weavec::checking-incomplete]
