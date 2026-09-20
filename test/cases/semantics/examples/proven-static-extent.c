// RFC 0030 §4 "Proven": a [static 4] parameter discharges the null and spatial facets.
// STAGE: S6
// a[0] is a Deref site with null, spatial and temporal proven ([static 4] implies non-null,
// A1); a[3] is an Index site with spatial proven (3 < 4). No code is emitted, so no site of
// the unit is checked, unresolved, trusted or a violation.
// CLEAN
// EXPECT-LEDGER: /summary/checked == 0
// EXPECT-LEDGER: /summary/unresolved == 0
// EXPECT-LEDGER: /summary/trusted == 0
// EXPECT-LEDGER: /summary/violation == 0
int sum4(const int a[static 4]) { return a[0] + a[3]; }
