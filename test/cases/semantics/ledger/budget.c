// RFC 0030 §5.5: a function over its analysis budget takes the default outcomes with reason budget.
// STAGE: S3
// With a budget of one block transfer 'walk' stops early. Its facets take the §2.6 defaults
// with reason budget: null checked; spatial checked when the extent is exact from the type
// and its terms are constants or unassigned parameters (a[k]), otherwise
// unresolved(budget); temporal unresolved(budget). The function's row and the summary list
// it as over budget.
// FLAGS: -fweavec-budget=1
// EXPECT-LEDGER: /summary/overBudget/0 == "walk"
// EXPECT-LEDGER: /units/0/functions/0/overBudget == true
int walk(const int *p, int n, int k) {
  int a[4] = {0};
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i]; // UNRESOLVED: spatial:budget // UNRESOLVED: temporal:budget // TRAP: nonnull
  return s + a[k]; // TRAP: index
}
