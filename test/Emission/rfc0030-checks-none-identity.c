// RFC 0030, sections 10.5 and 10.7, gate G7 in miniature: with
// -fweavec-checks=none weavec-cc emits no check, injects no prelude and does
// not zero-initialise, so its objects are byte-identical to the reference
// Clang's at -O2, -O0 -g and -O2 -flto=thin, for units whose default build is
// full of checks and zero-initialisation (scripts/codegen-identity.py, the
// G7 driver, over Inputs/identity.txt).
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: python3 %S/../../scripts/codegen-identity.py --weavec-cc %weavec_cc --clang %clang --root %S/.. --resource-include %resource_dir --list %S/Inputs/identity.txt --min 6 --tmp %t | FileCheck %s
//
// CHECK: units: 6;
// CHECK-DAG: -O2 6 identical, 0 different, 0 weavec-failed, 0 skipped
// CHECK-DAG: -O0 -g 6 identical, 0 different, 0 weavec-failed, 0 skipped
// CHECK-DAG: -O2 -flto=thin 6 identical, 0 different, 0 weavec-failed, 0 skipped
