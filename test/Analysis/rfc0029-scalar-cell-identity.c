// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/scalar-write-offsets/advanced-read.c -- 2>&1 | FileCheck %s
// CHECK: checked safety failed: 'bytes[2]' is out of bounds: index 2 of an object of 2 bytes [weavec::checking-failed]
// RFCs 0017/0021/0029: moving a pointer retains its allocation identity,
// but scalar facts about the old pointee cannot describe the next cell.
